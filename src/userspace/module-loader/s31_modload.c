// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MODULE_INIT_COMPRESSED_FILE
#define MODULE_INIT_COMPRESSED_FILE 4
#endif

int main(int argc, char **argv)
{
	char parameters[512] = { 0 };
	size_t used = 0;
	int fd;
	int flags = 0;
	int ret;
	unsigned char signature[6];
	static const unsigned char xz_signature[6] = {
		0xfd, '7', 'z', 'X', 'Z', 0x00,
	};

	if (argc == 3 && !strcmp(argv[1], "--remove")) {
		ret = syscall(SYS_delete_module, argv[2], O_NONBLOCK);
		if (ret < 0)
			fprintf(stderr, "delete_module %s: %s\n", argv[2],
				strerror(errno));
		return ret < 0;
	}
	if (argc < 2) {
		fprintf(stderr, "usage: %s MODULE.ko [PARAM=VALUE ...]\n"
				"       %s --remove MODULE_NAME\n", argv[0], argv[0]);
		return 2;
	}
	for (int i = 2; i < argc; i++) {
		int length = snprintf(parameters + used, sizeof(parameters) - used,
				      "%s%s", used ? " " : "", argv[i]);

		if (length < 0 || (size_t)length >= sizeof(parameters) - used) {
			fprintf(stderr, "module parameters are too long\n");
			return 2;
		}
		used += length;
	}

	fd = open(argv[1], O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
		return 1;
	}

	/* Keep the compressed module compressed in SquashFS page cache.  The
	 * kernel expands it directly into module-loader pages, avoiding a second
	 * 5 MiB userspace/file-buffer copy on this 16 MiB board. */
	if (pread(fd, signature, sizeof(signature), 0) == sizeof(signature) &&
	    !memcmp(signature, xz_signature, sizeof(signature)))
		flags = MODULE_INIT_COMPRESSED_FILE;

	ret = syscall(SYS_finit_module, fd, parameters, flags);
	if (ret < 0)
		fprintf(stderr, "finit_module %s: %s\n", argv[1],
			strerror(errno));
	close(fd);

	return ret < 0;
}
