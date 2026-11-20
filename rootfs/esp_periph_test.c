// SPDX-License-Identifier: GPL-2.0-only

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/watchdog.h>
#include <mtd/mtd-user.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define RNG_TEST_BYTES 1024
#define NVMEM_MAX_BYTES 4096
#define SDMMC_TEST_BYTES 4096
#define SDMMC_SECOND_OFFSET (1024 * 1024)
#define MTD_TEST_OFFSET 3
#define MTD_TEST_BYTES 257

static unsigned int bit_count(const unsigned char *buf, size_t len)
{
	unsigned int ones = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		unsigned int value = buf[i];

		while (value) {
			ones += value & 1U;
			value >>= 1;
		}
	}

	return ones;
}

static int read_exact(int fd, unsigned char *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t ret = read(fd, buf + done, len - done);

		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0)
			return -1;
		done += ret;
	}

	return 0;
}

static int read_at(int fd, off_t offset, unsigned char *buf, size_t len)
{
	if (lseek(fd, offset, SEEK_SET) != offset)
		return -1;
	return read_exact(fd, buf, len);
}

static int write_at(int fd, off_t offset, const unsigned char *buf, size_t len)
{
	size_t done = 0;

	if (lseek(fd, offset, SEEK_SET) != offset)
		return -1;
	while (done < len) {
		ssize_t ret = write(fd, buf + done, len - done);

		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0)
			return -1;
		done += ret;
	}
	return 0;
}

static int test_rng(void)
{
	unsigned char first[RNG_TEST_BYTES];
	unsigned char second[RNG_TEST_BYTES];
	unsigned int ones;
	int fd;

	fd = open("/dev/hwrng", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("RNG: open /dev/hwrng");
		return 1;
	}

	if (read_exact(fd, first, sizeof(first)) ||
	    read_exact(fd, second, sizeof(second))) {
		perror("RNG: read");
		close(fd);
		return 1;
	}
	close(fd);

	ones = bit_count(first, sizeof(first));
	if (!memcmp(first, second, sizeof(first)) ||
	    ones < sizeof(first) * 8 * 35 / 100 ||
	    ones > sizeof(first) * 8 * 65 / 100) {
		fprintf(stderr, "RNG: FAIL repeated stream or implausible bit balance\n");
		return 1;
	}

	printf("RNG: PASS (%u/%u one bits)\n", ones,
	       (unsigned int)sizeof(first) * 8);
	return 0;
}

static int open_nvmem(char *path, size_t path_len)
{
	const char *base = "/sys/bus/nvmem/devices";
	struct dirent *entry;
	DIR *dir;
	int fd = -1;

	dir = opendir(base);
	if (!dir)
		return -1;

	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.')
			continue;
		if (snprintf(path, path_len, "%s/%s/nvmem", base,
			     entry->d_name) >= (int)path_len)
			continue;
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0)
			break;
	}

	closedir(dir);
	return fd;
}

static uint32_t fnv1a(const unsigned char *buf, size_t len)
{
	uint32_t hash = 2166136261U;
	size_t i;

	for (i = 0; i < len; i++) {
		hash ^= buf[i];
		hash *= 16777619U;
	}
	return hash;
}

static int find_mtd_by_name(const char *wanted, char *path, size_t path_len)
{
	const char *base = "/sys/class/mtd";
	char name_path[256];
	char name[80];
	struct dirent *entry;
	DIR *dir;
	FILE *file;
	int found = -1;
	size_t i;

	dir = opendir(base);
	if (!dir)
		return -1;
	while ((entry = readdir(dir))) {
		if (strncmp(entry->d_name, "mtd", 3) || !entry->d_name[3])
			continue;
		for (i = 3; entry->d_name[i] >= '0' && entry->d_name[i] <= '9'; i++)
			;
		if (entry->d_name[i])
			continue;
		if (snprintf(name_path, sizeof(name_path), "%s/%s/name", base,
			     entry->d_name) >= (int)sizeof(name_path))
			continue;
		file = fopen(name_path, "r");
		if (!file)
			continue;
		if (!fgets(name, sizeof(name), file)) {
			fclose(file);
			continue;
		}
		fclose(file);
		name[strcspn(name, "\r\n")] = 0;
		if (strcmp(name, wanted))
			continue;
		if (snprintf(path, path_len, "/dev/%s", entry->d_name) >=
		    (int)path_len)
			break;
		found = 0;
		break;
	}
	closedir(dir);
	return found;
}

static int erase_mtd_sector(int fd, uint32_t erase_size)
{
	erase_info_t erase = {
		.start = 0,
		.length = erase_size,
	};

	return ioctl(fd, MEMERASE, &erase);
}

static int test_mtd_rw(void)
{
	unsigned char pattern[MTD_TEST_BYTES];
	char spare_path[64];
	char boot_path[64];
	unsigned char *backup = NULL;
	unsigned char *verify = NULL;
	mtd_info_t info;
	uint32_t before_hash;
	uint32_t programmed_hash = 0;
	int fd = -1;
	int protected_fd;
	int modified = 0;
	int restore_failed = 0;
	int ret = 1;
	size_t i;

	if (find_mtd_by_name("spare", spare_path, sizeof(spare_path)) ||
	    find_mtd_by_name("bootloader", boot_path, sizeof(boot_path))) {
		fprintf(stderr, "MTD-RW: FAIL missing spare or bootloader partition\n");
		return 1;
	}

	protected_fd = open(boot_path, O_RDWR | O_CLOEXEC);
	if (protected_fd >= 0) {
		close(protected_fd);
		fprintf(stderr, "MTD-RW: FAIL bootloader partition is writable\n");
		return 1;
	}

	fd = open(spare_path, O_RDWR | O_CLOEXEC);
	if (fd < 0 || ioctl(fd, MEMGETINFO, &info) < 0) {
		perror("MTD-RW: open/MEMGETINFO spare");
		goto out;
	}
	if (!(info.flags & MTD_WRITEABLE) || info.type != MTD_NORFLASH ||
	    info.erasesize < MTD_TEST_OFFSET + MTD_TEST_BYTES ||
	    info.erasesize > 64 * 1024 || info.size < info.erasesize) {
		fprintf(stderr,
			"MTD-RW: FAIL type=%u flags=%#x size=%u erase=%u\n",
			info.type, info.flags, info.size, info.erasesize);
		goto out;
	}

	backup = malloc(info.erasesize);
	verify = malloc(info.erasesize);
	if (!backup || !verify) {
		fprintf(stderr, "MTD-RW: FAIL allocating sector buffers\n");
		goto out;
	}
	if (read_at(fd, 0, backup, info.erasesize)) {
		perror("MTD-RW: backup read");
		goto out;
	}
	before_hash = fnv1a(backup, info.erasesize);

	if (erase_mtd_sector(fd, info.erasesize)) {
		perror("MTD-RW: erase test sector");
		goto out;
	}
	modified = 1;
	if (read_at(fd, 0, verify, info.erasesize)) {
		perror("MTD-RW: erased-sector read");
		goto restore;
	}
	for (i = 0; i < info.erasesize; i++) {
		if (verify[i] != 0xff) {
			fprintf(stderr, "MTD-RW: FAIL erase verify at byte %zu\n", i);
			goto restore;
		}
	}

	for (i = 0; i < sizeof(pattern); i++)
		pattern[i] = (unsigned char)(0x5a ^ (i * 37U));
	if (write_at(fd, MTD_TEST_OFFSET, pattern, sizeof(pattern))) {
		perror("MTD-RW: unaligned program");
		goto restore;
	}
	if (read_at(fd, 0, verify, info.erasesize) ||
	    memcmp(verify + MTD_TEST_OFFSET, pattern, sizeof(pattern))) {
		fprintf(stderr, "MTD-RW: FAIL program readback\n");
		goto restore;
	}
	programmed_hash = fnv1a(verify, info.erasesize);
	ret = 0;

restore:
	if (erase_mtd_sector(fd, info.erasesize)) {
		perror("MTD-RW: restore erase");
		restore_failed = 1;
	} else {
		int all_erased = 1;

		for (i = 0; i < info.erasesize; i++)
			all_erased &= backup[i] == 0xff;
		if (!all_erased && write_at(fd, 0, backup, info.erasesize)) {
			perror("MTD-RW: restore program");
			restore_failed = 1;
		} else if (read_at(fd, 0, verify, info.erasesize) ||
			   memcmp(backup, verify, info.erasesize)) {
			fprintf(stderr, "MTD-RW: FAIL restored-sector verify\n");
			restore_failed = 1;
		}
	}
	if (restore_failed)
		ret = 1;
	else if (!ret)
		printf("MTD-RW: PASS (%s, erase %u, unaligned %u-byte program, "
		       "digests %08x/%08x, restored)\n",
		       spare_path, info.erasesize, MTD_TEST_BYTES,
		       before_hash, programmed_hash);

out:
	if (fd >= 0)
		close(fd);
	free(verify);
	free(backup);
	if (modified && restore_failed)
		fprintf(stderr, "MTD-RW: CRITICAL test sector restoration failed\n");
	return ret;
}

static int test_mac_cell(const char *provider_path)
{
	unsigned char mac[6];
	char cells_path[512];
	char cell_path[768];
	struct dirent *entry;
	char *slash;
	DIR *dir;
	int fd = -1;
	int valid = 0;

	if (snprintf(cells_path, sizeof(cells_path), "%s", provider_path) >=
	    (int)sizeof(cells_path))
		return 1;
	slash = strrchr(cells_path, '/');
	if (!slash)
		return 1;
	strcpy(slash, "/cells");

	dir = opendir(cells_path);
	if (!dir)
		return 1;
	while ((entry = readdir(dir))) {
		if (strncmp(entry->d_name, "mac-address", 11))
			continue;
		if (snprintf(cell_path, sizeof(cell_path), "%s/%s", cells_path,
			     entry->d_name) >= (int)sizeof(cell_path))
			continue;
		fd = open(cell_path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0)
			break;
	}
	closedir(dir);

	if (fd < 0 || read_exact(fd, mac, sizeof(mac))) {
		if (fd >= 0)
			close(fd);
		return 1;
	}
	close(fd);

	valid = !(mac[0] & 1) && memcmp(mac, "\0\0\0\0\0\0", sizeof(mac));
	if (!valid)
		return 1;

	printf("eFuse MAC cell: PASS (%02x:%02x:%02x:%02x:%02x:%02x)\n",
	       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return 0;
}

static int test_efuse(void)
{
	unsigned char first[NVMEM_MAX_BYTES];
	unsigned char second[NVMEM_MAX_BYTES];
	char path[512];
	ssize_t len, len2;
	size_t i;
	int fd;
	int useful = 0;

	fd = open_nvmem(path, sizeof(path));
	if (fd < 0) {
		perror("eFuse: locate NVMEM provider");
		return 1;
	}

	len = read(fd, first, sizeof(first));
	if (len <= 0 || lseek(fd, 0, SEEK_SET) < 0) {
		perror("eFuse: first read");
		close(fd);
		return 1;
	}
	len2 = read(fd, second, sizeof(second));
	close(fd);
	if (len2 != len || memcmp(first, second, len)) {
		fprintf(stderr, "eFuse: FAIL unstable read-shadow data\n");
		return 1;
	}

	for (i = 0; i < (size_t)len; i++)
		useful |= first[i] != 0 && first[i] != 0xff;
	if (!useful) {
		fprintf(stderr, "eFuse: FAIL provider contains only 00/ff bytes\n");
		return 1;
	}

	/* Print only a digest: raw eFuse contents can contain secret key material. */
	printf("eFuse: PASS (%zd bytes, digest %08x, %s)\n", len,
	       fnv1a(first, len), path);
	if (test_mac_cell(path)) {
		fprintf(stderr, "eFuse MAC cell: FAIL missing or invalid cell\n");
		return 1;
	}
	return 0;
}

static void watchdog_magic_close(int fd)
{
	char magic = 'V';

	if (fd >= 0) {
		(void)write(fd, &magic, 1);
		close(fd);
	}
}

static int test_watchdog(void)
{
	struct watchdog_info info;
	int timeout = 10;
	int actual = 0;
	int fd;
	int ret = 1;

	fd = open("/dev/watchdog0", O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("watchdog: open /dev/watchdog0");
		return 1;
	}

	memset(&info, 0, sizeof(info));
	if (ioctl(fd, WDIOC_GETSUPPORT, &info) < 0 ||
	    ioctl(fd, WDIOC_SETTIMEOUT, &timeout) < 0 ||
	    ioctl(fd, WDIOC_GETTIMEOUT, &actual) < 0) {
		perror("watchdog: ioctl");
		goto out;
	}

	if (ioctl(fd, WDIOC_KEEPALIVE, 0) < 0) {
		perror("watchdog: keepalive");
		goto out;
	}
	sleep(1);
	if (ioctl(fd, WDIOC_KEEPALIVE, 0) < 0) {
		perror("watchdog: second keepalive");
		goto out;
	}

	printf("watchdog: PASS (%s, timeout %d s, magic-close)\n",
	       info.identity, actual);
	ret = 0;
out:
	watchdog_magic_close(fd);
	return ret;
}

static int test_sdmmc(void)
{
	unsigned char first[SDMMC_TEST_BYTES];
	unsigned char verify[SDMMC_TEST_BYTES];
	unsigned long long capacity;
	uint32_t first_hash;
	uint32_t second_hash;
	int fd;

	fd = open("/dev/mmcblk0", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("SDMMC: open /dev/mmcblk0");
		return 1;
	}
	if (ioctl(fd, BLKGETSIZE64, &capacity) < 0) {
		perror("SDMMC: BLKGETSIZE64");
		close(fd);
		return 1;
	}
	if (capacity < SDMMC_SECOND_OFFSET + SDMMC_TEST_BYTES) {
		fprintf(stderr, "SDMMC: FAIL implausible capacity %llu bytes\n",
			capacity);
		close(fd);
		return 1;
	}

	if (read_at(fd, 0, first, sizeof(first)) ||
	    read_at(fd, 0, verify, sizeof(verify)) ||
	    memcmp(first, verify, sizeof(first))) {
		fprintf(stderr, "SDMMC: FAIL unstable first 4 KiB read\n");
		close(fd);
		return 1;
	}
	first_hash = fnv1a(first, sizeof(first));

	if (read_at(fd, SDMMC_SECOND_OFFSET, first, sizeof(first)) ||
	    read_at(fd, SDMMC_SECOND_OFFSET, verify, sizeof(verify)) ||
	    memcmp(first, verify, sizeof(first))) {
		fprintf(stderr, "SDMMC: FAIL unstable 1 MiB-offset read\n");
		close(fd);
		return 1;
	}
	second_hash = fnv1a(first, sizeof(first));
	close(fd);

	printf("SDMMC: PASS (%llu MiB, stable read digests %08x/%08x)\n",
	       capacity / (1024 * 1024), first_hash, second_hash);
	return 0;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s {all|efuse|rng|watchdog|sdmmc|mtd-rw}\n", program);
}

int main(int argc, char **argv)
{
	int failures = 0;

	if (argc != 2) {
		usage(argv[0]);
		return 2;
	}

	if (!strcmp(argv[1], "all") || !strcmp(argv[1], "efuse"))
		failures += test_efuse();
	if (!strcmp(argv[1], "all") || !strcmp(argv[1], "rng"))
		failures += test_rng();
	if (!strcmp(argv[1], "all") || !strcmp(argv[1], "watchdog"))
		failures += test_watchdog();
	/* SDMMC needs a card, so keep it separate from the on-chip-only suite. */
	if (!strcmp(argv[1], "sdmmc"))
		failures += test_sdmmc();
	/* Destructive internally but self-restoring; never include it in all. */
	if (!strcmp(argv[1], "mtd-rw"))
		failures += test_mtd_rw();

	if (strcmp(argv[1], "all") && strcmp(argv[1], "efuse") &&
	    strcmp(argv[1], "rng") && strcmp(argv[1], "watchdog") &&
	    strcmp(argv[1], "sdmmc") && strcmp(argv[1], "mtd-rw")) {
		usage(argv[0]);
		return 2;
	}

	return failures ? 1 : 0;
}
