/* SPDX-License-Identifier: Apache-2.0 */
/* Convert existing encrypted ESP-IDF DS data and its public key. No operation
 * here accesses crypto MMIO, creates a private key or provisions hardware.
 */
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <linux/esp32s31-crypto.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "key-file.h"

struct idf_ds_data {
	uint32_t length;
	unsigned char iv[16], wrapped[ESP32S31_CRYPTO_DS_WRAPPED_BYTES];
};

static int regular_open(const char *path, size_t maximum)
{
	struct stat st; int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) return -1;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 1 || (uint64_t)st.st_size > maximum) {
		close(fd); errno = EINVAL; return -1;
	}
	return fd;
}

static int read_exact(int fd, void *buffer, size_t length)
{
	unsigned char *data = buffer, extra; ssize_t n;
	while (length) {
		n = read(fd, data, length);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return 0;
		data += n; length -= n;
	}
	do n = read(fd, &extra, 1); while (n < 0 && errno == EINTR);
	return n == 0;
}

static int public_pem_header(BIO *bio)
{
	char line[128]; size_t prefix = 0, length;
	while (BIO_gets(bio, line, sizeof(line)) > 0 && prefix < 4096) {
		length = strlen(line); prefix += length;
		if (strncmp(line, "-----BEGIN ", 11)) continue;
		while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = 0;
		/* A generic public-key decoder may also extract a public component
		 * from a private PEM. Reject that input before decoding its body.
		 */
		return (!strcmp(line, "-----BEGIN PUBLIC KEY-----") ||
			!strcmp(line, "-----BEGIN RSA PUBLIC KEY-----")) && BIO_seek(bio, 0) >= 0;
	}
	return 0;
}

static EVP_PKEY *read_public(BIO *bio)
{
	char *name = NULL, *header = NULL; unsigned char *data = NULL;
	const unsigned char *cursor; long length = 0; EVP_PKEY *key = NULL;
	if (!public_pem_header(bio) || !PEM_read_bio(bio, &name, &header, &data, &length) || length <= 0) goto out;
	cursor = data;
	if (!strcmp(name, "PUBLIC KEY")) key = d2i_PUBKEY_ex(NULL, &cursor, length, NULL, "provider=default");
	else if (!strcmp(name, "RSA PUBLIC KEY")) key = d2i_PublicKey(EVP_PKEY_RSA, NULL, &cursor, length);
	if (key && cursor != data + length) { EVP_PKEY_free(key); key = NULL; }
out:
	OPENSSL_free(name); OPENSSL_free(header); OPENSSL_clear_free(data, length > 0 ? (size_t)length : 0); return key;
}

int main(int argc, char **argv)
{
	struct idf_ds_data idf = {0}; struct esp32s31_crypto_ds_key *bundle = NULL;
	struct esp32s31_crypto_km_key recovery = {0}; size_t extra = 0;
	EVP_PKEY *public = NULL; EVP_PKEY_CTX *check = NULL; BIO *bio = NULL;
	unsigned char *der = NULL; char *end; unsigned long slot; int derlen, fd = -1, out = -1, ok = 0;
	size_t length = 0, position = 0; ssize_t n;
	if (argc != 5) { fprintf(stderr, "Usage: s31-ds-pack SLOT_OR_km:RECOVERY IDF_DS_DATA RSA_PUBLIC_PEM NEW_BUNDLE\n"); return 2; }
	if (!strncmp(argv[1], "km:", 3)) {
		if (!s31_key_file(argv[1] + 3, &recovery, sizeof(recovery)) ||
		    recovery.magic != ESP32S31_CRYPTO_KM_MAGIC || recovery.type != 3 ||
		    recovery.length != 3) goto done;
		extra = sizeof(recovery); slot = ESP32S31_CRYPTO_SLOT_UNUSED;
	} else {
		errno = 0; slot = strtoul(argv[1], &end, 10);
		if (errno || !*argv[1] || *argv[1] == '-' || *end || slot >= ESP32S31_CRYPTO_EFUSE_SLOTS) goto done;
	}
	fd = regular_open(argv[2], sizeof(idf));
	if (fd < 0 || !read_exact(fd, &idf, sizeof(idf))) goto done;
	close(fd); fd = -1;
	if (idf.length != 31 && idf.length != 63 && idf.length != 95 && idf.length != 127) goto done;
	fd = regular_open(argv[3], 16384);
	if (fd < 0 || !(bio = BIO_new_fd(fd, BIO_CLOSE))) goto done;
	fd = -1;
	public = read_public(bio);
	if (!public || !EVP_PKEY_is_a(public, "RSA") || EVP_PKEY_is_a(public, "RSA-PSS") ||
	    EVP_PKEY_get_bits(public) != (int)((idf.length + 1) * 32)) goto done;
	check = EVP_PKEY_CTX_new_from_pkey(NULL, public, "provider=default");
	if (!check || EVP_PKEY_public_check(check) <= 0 || (derlen = i2d_PublicKey(public, &der)) <= 0) goto done;
	length = sizeof(*bundle) + extra + derlen;
	bundle = OPENSSL_zalloc(length);
	if (!bundle) goto done;
	bundle->slot = slot; bundle->rsa_bits = (idf.length + 1) * 32; bundle->public_key_len = derlen;
	memcpy(bundle->iv, idf.iv, sizeof(idf.iv)); memcpy(bundle->wrapped, idf.wrapped, sizeof(idf.wrapped));
	bundle->reserved = extra ? ESP32S31_CRYPTO_DS_KEY_MANAGER : 0;
	if (extra) memcpy(bundle->public_key, &recovery, extra);
	memcpy(bundle->public_key + extra, der, derlen);
	out = open(argv[4], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (out < 0) goto done;
	while (position < length) {
		n = write(out, (unsigned char *)bundle + position, length - position);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) goto done;
		position += n;
	}
	if (fsync(out)) goto done;
	ok = 1;
done:
	if (out >= 0) { if (close(out)) ok = 0; if (!ok) unlink(argv[4]); }
	if (fd >= 0) close(fd);
	if (!ok) { fprintf(stderr, "Cannot package DS bundle: invalid data, public key, slot or file operation\n"); ERR_print_errors_fp(stderr); }
	OPENSSL_cleanse(&recovery, sizeof(recovery)); OPENSSL_cleanse(&idf, sizeof(idf)); OPENSSL_clear_free(bundle, length); OPENSSL_free(der);
	EVP_PKEY_CTX_free(check); EVP_PKEY_free(public); BIO_free(bio); return ok ? 0 : 1;
}
