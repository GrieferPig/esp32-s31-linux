/* SPDX-License-Identifier: Apache-2.0 */
#ifndef S31_KEY_FILE_H
#define S31_KEY_FILE_H
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

/* Exact-size, bounded regular-file inputs; a racing truncate/grow fails. */
static inline int s31_key_file(const char *path, void *buffer, size_t length)
{
	struct stat st; unsigned char *p = buffer, extra; size_t done = 0;
	ssize_t n; int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK), ok = 0, saved;
	if (fd < 0) return 0;
	if (fstat(fd, &st)) goto out;
	if (!S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size != length) { errno = EINVAL; goto out; }
	while (done < length) {
		n = read(fd, p + done, length - done);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) { if (!n) errno = EIO; goto out; }
		done += n;
	}
	do n = read(fd, &extra, 1); while (n < 0 && errno == EINTR);
	if (n) { if (n > 0) errno = EFBIG; goto out; }
	ok = 1;
out:
	saved = errno; close(fd); errno = saved; return ok;
}
#endif
