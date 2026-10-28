// SPDX-License-Identifier: GPL-2.0-only
/* Derive a WPA PSK without placing the passphrase in argv or a file. */
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

static int hex_digit(unsigned char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

int main(int argc, char **argv)
{
	unsigned char ssid[32], key[32];
	char password[65] = {0};
	size_t bytes, length = 0;
	int c, result = 1;
	if (argc != 2 || !(bytes = strlen(argv[1])) || bytes > 64 || bytes % 2) {
		fputs("Usage: s31-wifi-psk SSID_HEX < passphrase\n", stderr);
		return 2;
	}
	for (size_t i = 0; i < bytes / 2; i++) {
		int high = hex_digit(argv[1][2 * i]), low = hex_digit(argv[1][2 * i + 1]);
		if (high < 0 || low < 0) goto done;
		ssid[i] = high * 16 + low;
	}
	while ((c = getchar()) != EOF && c != '\n') {
		if (!c || c == '\r' || length >= 63) goto done;
		password[length++] = c;
	}
	if (ferror(stdin) || length < 8 || (c == '\n' && getchar() != EOF)) goto done;
	if (ferror(stdin)) goto done;
	if (!PKCS5_PBKDF2_HMAC(password, length, ssid, bytes / 2, 4096,
			      EVP_sha1(), sizeof(key), key)) goto done;
	for (size_t i = 0; i < sizeof(key); i++)
		if (printf("%02x", key[i]) < 0) goto done;
	if (putchar('\n') == EOF || fflush(stdout)) goto done;
	result = 0;
done:
	OPENSSL_cleanse(password, sizeof(password));
	OPENSSL_cleanse(key, sizeof(key));
	if (result) fputs("Cannot derive WPA PSK\n", stderr);
	return result;
}
