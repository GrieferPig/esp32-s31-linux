/* SPDX-License-Identifier: Apache-2.0 */
/* Deploy a pre-wrapped AES-mode request, saving only chip-bound recovery data.
 * No fuse programming and no raw private-key export from the device.
 */
#include <linux/esp32s31-crypto.h>
#include <sys/ioctl.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include "key-file.h"

int main(int argc, char **argv)
{
	struct esp32s31_crypto_km_deploy job = {0};
	int fd = -1, out = -1, ok = 0; size_t done = 0; ssize_t n;
	if (argc != 3) { fprintf(stderr, "Usage: s31-km-deploy AES_REQUEST NEW_RECOVERY_FILE\n"); return 2; }
	if (!s31_key_file(argv[1], &job, sizeof(job))) goto end;
	/* Reserve the destination before changing volatile hardware state. */
	out = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (out < 0) goto end;
	fd = open("/dev/s31-crypto", O_RDWR | O_CLOEXEC);
	if (fd < 0 || ioctl(fd, ESP32S31_CRYPTO_IOC_KM_DEPLOY, &job)) goto end;
	while (done < sizeof(job.recovery)) {
		n = write(out, (unsigned char *)&job.recovery + done, sizeof(job.recovery) - done);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) goto end;
		done += n;
	}
	if (fsync(out)) goto end;
	ok = 1;
end:
	if (!ok) perror("Key Manager deployment/recovery file");
	if (fd >= 0) close(fd);
	if (out >= 0) { if (close(out)) ok = 0; if (!ok) unlink(argv[2]); }
	OPENSSL_cleanse(&job, sizeof(job)); return ok ? 0 : 1;
}
