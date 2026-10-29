// SPDX-License-Identifier: GPL-2.0-only
/* Read the deliberately small, flat tar format used by esp32-config backups.
 * No tar metadata, links, paths, extensions, or ownership are applied. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK 512
#define MAX_FILE 65536
#define MAX_ARCHIVE (256 * 1024)

static const char *const names[] = {
	"format", "system.conf", "wifi.conf", "wpa_supplicant.conf",
	"bluetooth.conf", "overlays.conf", "gpio.conf", "time.conf",
	"autostart.conf", "autostart.args", "swap.conf", "storage.conf",
	"usb.conf", NULL
};

static int zeroes(const unsigned char *p, size_t n)
{
	while (n--)
		if (*p++)
			return 0;
	return 1;
}

static int octal(const unsigned char *p, size_t n, size_t *value)
{
	size_t v = 0, i = 0;
	int found = 0;
	while (i < n && p[i] == ' ')
		i++;
	for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
		if (v > (SIZE_MAX - 7) / 8)
			return -1;
		v = v * 8 + p[i] - '0';
		found = 1;
	}
	for (; i < n; i++)
		if (p[i] && p[i] != ' ')
			return -1;
	*value = v;
	return found ? 0 : -1;
}

static int read_block(int fd, unsigned char *data)
{
	size_t done = 0;
	while (done < BLOCK) {
		ssize_t got = read(fd, data + done, BLOCK - done);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return -1;
		done += (size_t)got;
	}
	return 0;
}

static int write_all(int fd, const unsigned char *data, size_t size)
{
	while (size) {
		ssize_t written = write(fd, data, size);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return -1;
		data += written;
		size -= (size_t)written;
	}
	return 0;
}

static int unpack(const char *archive, const char *directory)
{
	unsigned char block[BLOCK];
	unsigned int seen = 0;
	struct stat st;
	size_t consumed = 0, zeros = 0;
	int in = -1, dir = -1, out = -1, result = 1;
	const char *reason = "invalid backup archive";

	in = open(archive, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (in < 0 || fstat(in, &st) || !S_ISREG(st.st_mode) ||
	    st.st_size < 2 * BLOCK || st.st_size > MAX_ARCHIVE ||
	    st.st_size % BLOCK)
		goto done;
	dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dir < 0)
		goto done;
	while (consumed < (size_t)st.st_size) {
		size_t checksum, sum = 0, size, remaining, i;
		int index;
		if (read_block(in, block))
			goto done;
		consumed += BLOCK;
		if (zeroes(block, BLOCK)) {
			zeros++;
			continue;
		}
		if (zeros)
			goto done;
		if (octal(block + 148, 8, &checksum))
			goto done;
		for (i = 0; i < BLOCK; i++)
			sum += (i >= 148 && i < 156) ? ' ' : block[i];
		if (checksum != sum || !memchr(block, '\0', 100) ||
		    (block[156] != '0' && block[156] != '\0') ||
		    !zeroes(block + 157, 100) || !zeroes(block + 345, 155) ||
		    (memcmp(block + 257, "ustar", 5) && !zeroes(block + 257, 8)))
			goto done;
		for (index = 0; names[index]; index++)
			if (!strcmp((char *)block, names[index]))
				break;
		if (!names[index] || (seen & (1U << index)) ||
		    octal(block + 124, 12, &size) || size > MAX_FILE ||
		    ((size + BLOCK - 1) / BLOCK) * BLOCK >
			(size_t)st.st_size - consumed)
			goto done;
		seen |= 1U << index;
		out = openat(dir, names[index], O_WRONLY | O_CREAT | O_EXCL |
			     O_NOFOLLOW | O_CLOEXEC, 0600);
		if (out < 0)
			goto done;
		remaining = size;
		while (remaining) {
			size_t chunk = remaining < BLOCK ? remaining : BLOCK;
			if (read_block(in, block))
				goto done;
			consumed += BLOCK;
			/* Every payload is text. NULs and terminal controls are never
			 * part of this configuration format. UTF-8 is preserved. */
			for (i = 0; i < chunk; i++)
				if ((!block[i] || block[i] < 32 || block[i] == 127) &&
				    block[i] != '\n' && block[i] != '\t')
					goto done;
			if (!zeroes(block + chunk, BLOCK - chunk) ||
			    write_all(out, block, chunk))
				goto done;
			remaining -= chunk;
		}
		if (fsync(out))
			goto done;
		if (close(out)) {
			out = -1;
			goto done;
		}
		out = -1;
	}
	if (zeros < 2 || !(seen & 1U))
		goto done;
	result = 0;
done:
	if (out >= 0)
		close(out);
	if (dir >= 0)
		close(dir);
	if (in >= 0)
		close(in);
	if (result)
		fprintf(stderr, "s31-config-archive: %s\n", reason);
	return result;
}

int main(int argc, char **argv)
{
	if (argc != 4 || strcmp(argv[1], "unpack")) {
		fprintf(stderr, "Usage: s31-config-archive unpack BACKUP.tar EMPTY_DIRECTORY\n");
		return 2;
	}
	return unpack(argv[2], argv[3]);
}
