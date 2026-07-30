// SPDX-License-Identifier: GPL-2.0-only
/* Configure concurrent ESP32-S31 DT overlays and persist their full set. */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libfdt.h>
#include <linux/ioctl.h>
#include <mtd/mtd-user.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define OVERLAY_DEVICE "/dev/s31-overlay"
#define OVERLAY_DIR "/usr/lib/s31-overlays"
#define OVERLAY_PREFIX "esp32s31-overlay-"
#define OVERLAY_SUFFIX ".dtbo"
#define CURRENT_FILE "/run/s31-overlay.current"
#define PERSIST_MTD_NAME "dtbo_cfg"
#define PERSIST_MAGIC_V1 "S31OVL1"
#define PERSIST_MAGIC_V2 "S31OVL2"
#define PERSIST_VERSION 2U
#define MAX_DTBO_SIZE (128U * 1024U)
#define MAX_OVERLAYS 32
#define NAME_LEN 32
#define CONFIG_LEN 2048

#define S31_OVERLAY_IOC_MAGIC 'O'
#define S31_OVERLAY_IOC_REMOVE_ALL _IO(S31_OVERLAY_IOC_MAGIC, 0)
#define S31_OVERLAY_IOC_REMOVE_NAME _IOW(S31_OVERLAY_IOC_MAGIC, 2, \
					 struct overlay_name)
#define S31_OVERLAY_IOC_LIST _IOR(S31_OVERLAY_IOC_MAGIC, 3, \
				  struct overlay_list)

struct overlay_name { char name[NAME_LEN]; };
struct overlay_item { int32_t id; char name[NAME_LEN]; uint64_t gpios; };
struct overlay_list { uint32_t count; struct overlay_item items[MAX_OVERLAYS]; };

struct __attribute__((packed)) persist_record_v1 {
	char magic[8];
	uint32_t version;
	uint32_t sequence;
	char profile[32];
	uint32_t crc;
};

struct __attribute__((packed)) persist_record {
	char magic[8];
	uint32_t version;
	uint32_t sequence;
	uint32_t length;
	char config[CONFIG_LEN];
	uint32_t crc;
};

static const char *overlay_dir(void)
{
	const char *path = getenv("S31_OVERLAY_DIR");

	return path && *path ? path : OVERLAY_DIR;
}

static uint32_t crc32_bytes(const void *data, size_t length)
{
	const uint8_t *p = data;
	uint32_t crc = ~0U;
	size_t i;
	unsigned int bit;

	for (i = 0; i < length; i++) {
		crc ^= p[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320U &
						-(int32_t)(crc & 1));
	}
	return ~crc;
}

static int valid_name(const char *name)
{
	size_t i, length = strlen(name);

	if (!length || length >= NAME_LEN)
		return 0;
	for (i = 0; i < length; i++)
		if (!(islower((unsigned char)name[i]) ||
		      isdigit((unsigned char)name[i]) || name[i] == '-' ||
		      name[i] == '_'))
			return 0;
	return 1;
}

static int valid_gpio(unsigned int gpio)
{
	/* GPIO26..32 are occupied by the live XIP flash interface. */
	return gpio < 62 && (gpio < 26 || gpio > 32) &&
	       gpio != 33 && gpio != 34 && gpio != 41;
}

static uint32_t sequence_from_slot(const void *data, ssize_t got,
				   char *config, size_t config_size)
{
	const struct persist_record *v2 = data;
	const struct persist_record_v1 *v1 = data;

	if (got >= (ssize_t)sizeof(*v2) &&
	    !memcmp(v2->magic, PERSIST_MAGIC_V2, sizeof(v2->magic)) &&
	    v2->version == PERSIST_VERSION && v2->length < sizeof(v2->config) &&
	    v2->config[v2->length] == '\0' &&
	    v2->crc == crc32_bytes(v2, offsetof(struct persist_record, crc))) {
		snprintf(config, config_size, "%s", v2->config);
		return v2->sequence;
	}
	if (got >= (ssize_t)sizeof(*v1) &&
	    !memcmp(v1->magic, PERSIST_MAGIC_V1, sizeof(v1->magic)) &&
	    memchr(v1->profile, '\0', sizeof(v1->profile)) &&
	    v1->crc == crc32_bytes(v1, offsetof(struct persist_record_v1, crc))) {
		if (strcmp(v1->profile, "none"))
			snprintf(config, config_size, "%s\n", v1->profile);
		else
			config[0] = '\0';
		return v1->sequence;
	}
	return 0;
}

static int sequence_after(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) > 0;
}

static int find_persist_mtd(char *path, size_t path_size)
{
	char name_path[64], name[64];
	int i;

	for (i = 0; i < 32; i++) {
		FILE *file;

		snprintf(name_path, sizeof(name_path),
			 "/sys/class/mtd/mtd%d/name", i);
		file = fopen(name_path, "r");
		if (!file)
			continue;
		if (fgets(name, sizeof(name), file)) {
			name[strcspn(name, "\r\n")] = '\0';
			if (!strcmp(name, PERSIST_MTD_NAME)) {
				fclose(file);
				snprintf(path, path_size, "/dev/mtd%d", i);
				return 0;
			}
		}
		fclose(file);
	}
	errno = ENODEV;
	return -1;
}

static int read_persist(char *config, size_t config_size, int *best_slot,
			uint32_t *best_sequence)
{
	struct persist_record record;
	struct mtd_info_user info;
	char candidate[CONFIG_LEN], path[32];
	int fd, slots, slot, found = 0;

	if (find_persist_mtd(path, sizeof(path)))
		return -1;
	fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0)
		return -1;
	if (ioctl(fd, MEMGETINFO, &info) < 0 || !info.erasesize ||
	    info.size < info.erasesize) {
		close(fd);
		errno = EINVAL;
		return -1;
	}
	slots = info.size / info.erasesize;
	for (slot = 0; slot < slots; slot++) {
		uint32_t sequence;
		ssize_t got = pread(fd, &record, sizeof(record),
				    (off_t)slot * info.erasesize);

		candidate[0] = '\0';
		sequence = sequence_from_slot(&record, got, candidate,
					      sizeof(candidate));
		if (!sequence)
			continue;
		if (!found || sequence_after(sequence, *best_sequence)) {
			*best_slot = slot;
			*best_sequence = sequence;
			snprintf(config, config_size, "%s", candidate);
			found = 1;
		}
	}
	close(fd);
	if (!found) {
		errno = ENODATA;
		return -1;
	}
	return 0;
}

static int write_persist(const char *config)
{
	struct persist_record record, verify;
	struct erase_info_user erase;
	struct mtd_info_user info;
	char previous[CONFIG_LEN] = "", path[32];
	uint32_t sequence = 0;
	size_t length = strlen(config);
	int best_slot = -1, slot, slots, fd, attempt, write_errno = 0;
	ssize_t written;

	if (length >= sizeof(record.config)) {
		errno = E2BIG;
		return -1;
	}
	read_persist(previous, sizeof(previous), &best_slot, &sequence);
	if (find_persist_mtd(path, sizeof(path)))
		return -1;
	fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0)
		return -1;
	if (ioctl(fd, MEMGETINFO, &info) < 0 || !info.erasesize) {
		close(fd);
		return -1;
	}
	slots = info.size / info.erasesize;
	if (!slots) {
		close(fd);
		errno = EINVAL;
		return -1;
	}
	slot = (best_slot + 1) % slots;
	erase.start = slot * info.erasesize;
	erase.length = info.erasesize;
	if (ioctl(fd, MEMERASE, &erase) < 0) {
		close(fd);
		return -1;
	}
	memset(&record, 0, sizeof(record));
	memcpy(record.magic, PERSIST_MAGIC_V2, sizeof(record.magic));
	record.version = PERSIST_VERSION;
	record.sequence = sequence + 1;
	record.length = length;
	memcpy(record.config, config, length);
	record.crc = crc32_bytes(&record, offsetof(struct persist_record, crc));
	written = pwrite(fd, &record, sizeof(record), erase.start);
	if (written != sizeof(record))
		write_errno = written < 0 ? errno : EIO;
	/*
	 * The S31 ROM proxy can report a late program error after all bytes have
	 * reached NOR.  Accept it only when an independent full-record readback
	 * (including CRC) is byte-identical; otherwise preserve the write error.
	 */
	for (attempt = 0; attempt < 20; attempt++) {
		if (pread(fd, &verify, sizeof(verify), erase.start) == sizeof(verify) &&
		    !memcmp(&record, &verify, sizeof(record)))
			break;
		usleep(10000);
	}
	close(fd);
	if (attempt == 20) {
		errno = write_errno ?: EIO;
		return -1;
	}
	return 0;
}

static int manager_list(struct overlay_list *list)
{
	int fd = open(OVERLAY_DEVICE, O_RDONLY), ret;

	if (fd < 0)
		return -1;
	memset(list, 0, sizeof(*list));
	ret = ioctl(fd, S31_OVERLAY_IOC_LIST, list);
	close(fd);
	return ret;
}

static int manager_remove(const char *name)
{
	struct overlay_name requested = {};
	int fd = open(OVERLAY_DEVICE, O_WRONLY), ret;

	if (fd < 0)
		return -1;
	if (!name)
		ret = ioctl(fd, S31_OVERLAY_IOC_REMOVE_ALL);
	else {
		snprintf(requested.name, sizeof(requested.name), "%s", name);
		ret = ioctl(fd, S31_OVERLAY_IOC_REMOVE_NAME, &requested);
	}
	close(fd);
	return ret;
}

static int load_blob(const char *name, void **blob, size_t *size)
{
	char path[256];
	struct stat st;
	ssize_t done = 0;
	int fd;

	snprintf(path, sizeof(path), "%s/%s%s%s", overlay_dir(),
		 OVERLAY_PREFIX, name, OVERLAY_SUFFIX);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || st.st_size <= 0 || st.st_size > MAX_DTBO_SIZE) {
		close(fd);
		errno = EFBIG;
		return -1;
	}
	*blob = malloc(st.st_size);
	if (!*blob) {
		close(fd);
		return -1;
	}
	while (done < st.st_size) {
		ssize_t got = read(fd, (char *)*blob + done, st.st_size - done);
		if (got <= 0) {
			free(*blob);
			close(fd);
			return -1;
		}
		done += got;
	}
	close(fd);
	*size = st.st_size;
	return 0;
}

static int patch_route(void *blob, const char *assignment)
{
	char route[64], *end;
	const char *kind, *node_route;
	const fdt32_t *pinmux;
	unsigned long gpio;
	int depth = 0, len, node = -1, found = 0;
	size_t route_len = strcspn(assignment, "=");

	if (!assignment[route_len] || !route_len || route_len >= sizeof(route)) {
		errno = EINVAL;
		return -1;
	}
	memcpy(route, assignment, route_len);
	route[route_len] = '\0';
	errno = 0;
	gpio = strtoul(assignment + route_len + 1, &end, 0);
	if (errno || *end || !valid_gpio(gpio)) {
		errno = EINVAL;
		return -1;
	}
	while ((node = fdt_next_node(blob, node, &depth)) >= 0) {
		node_route = fdt_getprop(blob, node, "espressif,route-name", &len);
		if (!node_route || strcmp(node_route, route))
			continue;
		kind = fdt_getprop(blob, node, "espressif,route-kind", &len);
		if (!kind || (strcmp(kind, "matrix-input") &&
			     strcmp(kind, "matrix-output"))) {
			errno = EOPNOTSUPP;
			return -1;
		}
		pinmux = fdt_getprop(blob, node, "pinmux", &len);
		if (!pinmux || len != sizeof(*pinmux)) {
			errno = EINVAL;
			return -1;
		}
		if (fdt_setprop_inplace_u32(blob, node, "pinmux",
					(fdt32_to_cpu(*pinmux) & ~0xffU) | gpio)) {
			errno = EINVAL;
			return -1;
		}
		found++;
	}
	if (found != 1) {
		errno = found ? EEXIST : ENOENT;
		return -1;
	}
	return 0;
}

static int apply_spec(const char *spec)
{
	char copy[512], *save, *token, *name;
	void *blob;
	size_t size;
	int fd, ret = -1;

	if (strlen(spec) >= sizeof(copy)) {
		errno = E2BIG;
		return -1;
	}
	strcpy(copy, spec);
	name = strtok_r(copy, " \t", &save);
	if (!name || !valid_name(name) || load_blob(name, &blob, &size))
		return -1;
	while ((token = strtok_r(NULL, " \t", &save)))
		if (patch_route(blob, token))
			goto out;
	fd = open(OVERLAY_DEVICE, O_WRONLY);
	if (fd < 0)
		goto out;
	ret = write(fd, blob, size) == (ssize_t)size ? 0 : -1;
	close(fd);
out:
	free(blob);
	return ret;
}

static int read_current(char *config, size_t size)
{
	FILE *file = fopen(CURRENT_FILE, "r");
	size_t got;

	config[0] = '\0';
	if (!file)
		return errno == ENOENT ? 0 : -1;
	got = fread(config, 1, size - 1, file);
	if (ferror(file)) {
		fclose(file);
		return -1;
	}
	config[got] = '\0';
	fclose(file);
	return 0;
}

static int write_current(const char *config)
{
	char temporary[] = CURRENT_FILE ".XXXXXX";
	int fd = mkstemp(temporary);
	size_t length = strlen(config);

	if (fd < 0)
		return -1;
	if (write(fd, config, length) != (ssize_t)length || fsync(fd) ||
	    close(fd) || rename(temporary, CURRENT_FILE)) {
		int saved = errno;
		close(fd);
		unlink(temporary);
		errno = saved;
		return -1;
	}
	return 0;
}

static int update_config(char *config, size_t size, const char *name,
			 const char *replacement)
{
	char old[CONFIG_LEN], *save, *line;
	size_t used = 0;

	strcpy(old, config);
	config[0] = '\0';
	for (line = strtok_r(old, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		size_t name_len = strcspn(line, " \t");
		if (strlen(name) == name_len && !strncmp(line, name, name_len))
			continue;
		if (used + strlen(line) + 2 > size) {
			errno = E2BIG;
			return -1;
		}
		used += snprintf(config + used, size - used, "%s\n", line);
	}
	if (replacement) {
		if (used + strlen(replacement) + 2 > size) {
			errno = E2BIG;
			return -1;
		}
		snprintf(config + used, size - used, "%s\n", replacement);
	}
	return 0;
}

static void list_overlays(void)
{
	DIR *dir = opendir(overlay_dir());
	struct dirent *entry;
	size_t prefix = strlen(OVERLAY_PREFIX), suffix = strlen(OVERLAY_SUFFIX);

	if (!dir)
		return;
	while ((entry = readdir(dir))) {
		size_t length = strlen(entry->d_name);
		if (length > prefix + suffix &&
		    !strncmp(entry->d_name, OVERLAY_PREFIX, prefix) &&
		    !strcmp(entry->d_name + length - suffix, OVERLAY_SUFFIX))
			printf("%.*s\n", (int)(length - prefix - suffix),
			       entry->d_name + prefix);
	}
	closedir(dir);
}

static int list_routes(const char *name)
{
	void *blob;
	size_t size;
	int node = -1, depth = 0, len, found = 0;

	if (load_blob(name, &blob, &size))
		return -1;
	while ((node = fdt_next_node(blob, node, &depth)) >= 0) {
		const char *route = fdt_getprop(blob, node,
						"espressif,route-name", &len);
		const char *kind;
		const fdt32_t *pinmux;
		if (!route)
			continue;
		kind = fdt_getprop(blob, node, "espressif,route-kind", &len);
		pinmux = fdt_getprop(blob, node, "pinmux", &len);
		if (kind && pinmux && len == sizeof(*pinmux)) {
			printf("%s=%u (%s)\n", route,
			       fdt32_to_cpu(*pinmux) & 0xff, kind);
			found++;
		}
	}
	free(blob);
	return found ? 0 : 1;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s list|status|restore|routes NAME\n"
		"  %s apply NAME [ROUTE=GPIO ...] [--volatile]\n"
		"  %s remove NAME|--all [--volatile]\n", program, program, program);
}

int main(int argc, char **argv)
{
	char config[CONFIG_LEN] = "", persisted[CONFIG_LEN] = "", spec[512];
	struct overlay_list active;
	uint32_t sequence = 0;
	int slot = -1, persist = 1, i, ret;

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "list")) {
		list_overlays();
		return 0;
	}
	if (!strcmp(argv[1], "routes") && argc == 3) {
		ret = list_routes(argv[2]);
		if (ret < 0)
			perror("list routes");
		return !!ret;
	}
	if (!strcmp(argv[1], "status")) {
		if (manager_list(&active)) {
			perror("list active overlays");
			return 1;
		}
		printf("active-count: %u\n", active.count);
		for (i = 0; i < (int)active.count; i++)
			printf("active: %s id=%d gpios=%016llx\n",
			       active.items[i].name, active.items[i].id,
			       (unsigned long long)active.items[i].gpios);
		if (!read_persist(persisted, sizeof(persisted), &slot, &sequence))
			printf("persisted:\n%s", persisted[0] ? persisted : "  (none)\n");
		else
			puts("persisted: (none)");
		return 0;
	}
	if (!strcmp(argv[1], "restore")) {
		if (read_persist(config, sizeof(config), &slot, &sequence))
			return errno == ENODATA || errno == ENODEV ? 0 : 1;
		if (manager_remove(NULL) && errno != ENOENT)
			return 1;
		strcpy(persisted, config);
		{
			char *save, *line;
			for (line = strtok_r(persisted, "\n", &save); line;
			     line = strtok_r(NULL, "\n", &save)) {
				if (apply_spec(line)) {
					perror(line);
					manager_remove(NULL);
					return 1;
				}
			}
		}
		return write_current(config) ? 1 : 0;
	}
	if (!strcmp(argv[1], "apply")) {
		size_t used;
		if (argc < 3 || !valid_name(argv[2])) {
			usage(argv[0]);
			return 2;
		}
		used = snprintf(spec, sizeof(spec), "%s", argv[2]);
		for (i = 3; i < argc; i++) {
			if (!strcmp(argv[i], "--volatile")) {
				persist = 0;
				continue;
			}
			if (used + strlen(argv[i]) + 2 > sizeof(spec)) {
				errno = E2BIG;
				perror("overlay specification");
				return 1;
			}
			used += snprintf(spec + used, sizeof(spec) - used,
					 "%s%s", " ", argv[i]);
		}
		if (apply_spec(spec)) {
			perror("apply overlay");
			return 1;
		}
		if (read_current(config, sizeof(config)) ||
		    update_config(config, sizeof(config), argv[2], spec) ||
		    write_current(config) || (persist && write_persist(config))) {
			perror("record overlay set");
			return 1;
		}
		return 0;
	}
	if (!strcmp(argv[1], "remove")) {
		const char *name;
		if (argc < 3 || argc > 4) {
			usage(argv[0]);
			return 2;
		}
		if (argc == 4 && !strcmp(argv[3], "--volatile"))
			persist = 0;
		else if (argc == 4) {
			usage(argv[0]);
			return 2;
		}
		name = !strcmp(argv[2], "--all") ? NULL : argv[2];
		if (name && !valid_name(name))
			return 2;
		if (manager_remove(name)) {
			perror("remove overlay");
			return 1;
		}
		if (read_current(config, sizeof(config)) ||
		    (name ? update_config(config, sizeof(config), name, NULL) :
		     (config[0] = '\0', 0)) || write_current(config) ||
		    (persist && write_persist(config))) {
			perror("record overlay set");
			return 1;
		}
		return 0;
	}
	usage(argv[0]);
	return 2;
}
