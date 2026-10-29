// SPDX-License-Identifier: GPL-2.0-only
/* Configure concurrent ESP32-S31 DT overlays and persist their full set. */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libfdt.h>
#include <linux/esp32s31-overlay.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define OVERLAY_DEVICE "/dev/s31-overlay"
#define OVERLAY_DIR "/usr/lib/s31-overlays"
#define OVERLAY_PREFIX "esp32s31-overlay-"
#define OVERLAY_SUFFIX ".dtbo"
#define CURRENT_FILE "/run/s31-overlay.current"
#define LOCK_FILE "/run/s31-overlay.lock"
#define CONFIG_DIR "/etc/esp32-conf"
#define PERSIST_FILE CONFIG_DIR "/overlays.conf"
#define MAX_DTBO_SIZE S31_OVERLAY_MAX_SIZE
#define MAX_OVERLAYS S31_OVERLAY_MAX_ACTIVE
#define NAME_LEN S31_OVERLAY_NAME_LEN
/* Persistent settings live in the merged root's JFFS2 upperdir. */
#define CONFIG_LEN 2024
#define DWC2_DRIVER_DIR "/sys/bus/platform/drivers/dwc2"
#define DWC2_DEVICE "20300000.usb"

static const char *overlay_dir(void)
{
	const char *path = getenv("S31_OVERLAY_DIR");

	return path && *path ? path : OVERLAY_DIR;
}

static const char *persist_file(void)
{
	const char *path = getenv("S31_OVERLAY_PERSIST");

	return path && *path ? path : PERSIST_FILE;
}

static const char *current_file(void)
{
	const char *path = getenv("S31_OVERLAY_CURRENT");
	return path && *path ? path : CURRENT_FILE;
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
	       gpio != 33 && gpio != 34 && gpio != 41 &&
	       gpio != 58 && gpio != 59;
}

static int read_config_line(FILE *file, char *line, size_t capacity)
{
	size_t length = 0;
	int ch;
	while ((ch = fgetc(file)) != EOF && ch != '\n') {
		if (ch == '\0' || length + 1 >= capacity) {
			errno = ch == '\0' ? EINVAL : EOVERFLOW;
			return -1;
		}
		line[length++] = (char)ch;
	}
	if (ferror(file)) { errno = EIO; return -1; }
	if (ch == EOF && !length) return 0;
	if (length && line[length - 1] == '\r') length--;
	line[length] = '\0';
	for (size_t i = 0; i < length; i++)
		if ((unsigned char)line[i] < 0x20 && line[i] != '\t') {
			errno = EINVAL;
			return -1;
		}
	return 1;
}

static int read_persist(char *config, size_t config_size)
{
	FILE *file;
	char line[512], *equal, *name, *value;
	char seen[MAX_OVERLAYS][NAME_LEN];
	unsigned int count = 0, i;
	size_t used = 0;
	int result;

	config[0] = '\0';
	file = fopen(persist_file(), "r");
	if (!file)
		return -1;
	while ((result = read_config_line(file, line, sizeof(line))) > 0) {
		if (!line[0] || line[0] == '#')
			continue;
		equal = strchr(line, '=');
		if (!equal || strncmp(line, "overlay.", 8)) {
			fclose(file);
			errno = EINVAL;
			return -1;
		}
		*equal = '\0';
		name = line + 8;
		value = equal + 1;
		if (!valid_name(name) || !*value ||
		    strlen(name) != strcspn(value, " \t") ||
		    strncmp(name, value, strlen(name)) ||
		    used + strlen(value) + 2 > config_size) {
			fclose(file);
			errno = EINVAL;
			return -1;
		}
		for (i = 0; i < count; i++)
			if (!strcmp(seen[i], name)) {
				fclose(file);
				errno = EINVAL;
				return -1;
			}
		if (count == MAX_OVERLAYS) {
			fclose(file);
			errno = E2BIG;
			return -1;
		}
		strcpy(seen[count++], name);
		used += snprintf(config + used, config_size - used, "%s\n", value);
	}
	if (result < 0) {
		fclose(file);
		return -1;
	}
	fclose(file);
	return 0;
}

static int write_persist(const char *config)
{
	char temporary[256], old[CONFIG_LEN], *save, *line;
	int fd;

	if (strlen(config) >= sizeof(old)) {
		errno = E2BIG;
		return -1;
	}
	if (mkdir(CONFIG_DIR, 0700) && errno != EEXIST)
		return -1;
	snprintf(temporary, sizeof(temporary), "%s.tmp", persist_file());
	fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	strcpy(old, config);
	for (line = strtok_r(old, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		size_t name_len = strcspn(line, " \t");
		if (!name_len || name_len >= NAME_LEN) {
			close(fd);
			unlink(temporary);
			errno = EINVAL;
			return -1;
		}
		if (dprintf(fd, "overlay.%.*s=%s\n", (int)name_len, line,
			    line) < 0) {
			int saved = errno;
			close(fd);
			unlink(temporary);
			errno = saved;
			return -1;
		}
	}
	if (fsync(fd) || close(fd) || rename(temporary, persist_file())) {
		int saved = errno;
		close(fd);
		unlink(temporary);
		errno = saved ?: EIO;
		return -1;
	}
	return 0;
}

static int manager_list(struct s31_overlay_list *list)
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
	struct s31_overlay_name requested = {};
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

static int has_active_overlay(const struct s31_overlay_list *list, const char *name)
{
	uint32_t i;

	for (i = 0; i < list->count; i++)
		if (!strcmp(list->items[i].name, name))
			return 1;
	return 0;
}

static int write_driver_control(const char *control)
{
	char path[256];
	size_t length = strlen(DWC2_DEVICE);
	int fd;

	snprintf(path, sizeof(path), "%s/%s", DWC2_DRIVER_DIR, control);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	if (write(fd, DWC2_DEVICE, length) != (ssize_t)length) {
		int saved = errno ?: EIO;
		close(fd);
		errno = saved;
		return -1;
	}
	return close(fd);
}

/* dr_mode is sampled only when DWC2 probes, not when an OF property changes. */
static int reprobe_usb(void)
{
	char bound[256];

	snprintf(bound, sizeof(bound), "%s/%s", DWC2_DRIVER_DIR, DWC2_DEVICE);
	if (!access(bound, F_OK) && write_driver_control("unbind"))
		return -1;
	return write_driver_control("bind");
}

static int spec_is_usb_device(const char *spec)
{
	size_t length = strlen("usb-device");

	return !strncmp(spec, "usb-device", length) &&
	       (!spec[length] || isspace((unsigned char)spec[length]));
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
	fdt32_t *pinmux;
	unsigned long gpio;
	int depth = 0, len, node = -1, found = 0, i, count;
	size_t route_len = strcspn(assignment, "=");

	if (!assignment[route_len] || !route_len || route_len >= sizeof(route)) {
		errno = EINVAL;
		return -1;
	}
	memcpy(route, assignment, route_len);
	route[route_len] = '\0';
	errno = 0;
	gpio = strtoul(assignment + route_len + 1, &end, 0);
	if (errno || end == assignment + route_len + 1 || *end ||
	    assignment[route_len + 1] == '-' || gpio > UINT32_MAX ||
	    !valid_gpio((unsigned int)gpio)) {
		errno = EINVAL;
		return -1;
	}
	while ((node = fdt_next_node(blob, node, &depth)) >= 0) {
		count = fdt_stringlist_count(blob, node,
					     "espressif,route-names");
		if (count > 0) {
			pinmux = fdt_getprop_w(blob, node, "pinmux", &len);
			if (!pinmux || len != count * (int)sizeof(*pinmux)) {
				errno = EINVAL;
				return -1;
			}
			for (i = 0; i < count; i++) {
				node_route = fdt_stringlist_get(blob, node,
						"espressif,route-names", i, NULL);
				if (!node_route || strcmp(node_route, route))
					continue;
				kind = fdt_stringlist_get(blob, node,
						"espressif,route-kinds", i, NULL);
				if (!kind || (strcmp(kind, "matrix-input") &&
					     strcmp(kind, "matrix-output") &&
					     strcmp(kind, "matrix-bidirectional"))) {
					errno = EOPNOTSUPP;
					return -1;
				}
				pinmux[i] = cpu_to_fdt32(
					(fdt32_to_cpu(pinmux[i]) & ~0xffU) | gpio);
				found++;
			}
		}
		node_route = fdt_getprop(blob, node, "espressif,route-name", &len);
		if (!node_route || strcmp(node_route, route))
			continue;
		kind = fdt_getprop(blob, node, "espressif,route-kind", &len);
		if (!kind || (strcmp(kind, "matrix-input") &&
			     strcmp(kind, "matrix-output") &&
			     strcmp(kind, "matrix-bidirectional"))) {
			errno = EOPNOTSUPP;
			return -1;
		}
		pinmux = fdt_getprop_w(blob, node, "pinmux", &len);
		if (!pinmux || len <= 0 || len % sizeof(*pinmux)) {
			errno = EINVAL;
			return -1;
		}
		for (i = 0; i < len / (int)sizeof(*pinmux); i++)
			pinmux[i] = cpu_to_fdt32((fdt32_to_cpu(pinmux[i]) & ~0xffU) |
						 gpio);
		found++;
	}
	if (!found) {
		errno = ENOENT;
		return -1;
	}
	return 0;
}

/* Patch an explicitly exported scalar overlay parameter.  Keeping the
 * allow-list in the DTBO means arbitrary live-tree properties cannot be
 * changed through the command line. */
static int patch_parameter(void *blob, const char *assignment)
{
	char parameter[64], *end;
	const char *node_parameter;
	const fdt32_t *values;
	fdt32_t *property;
	unsigned long value;
	int depth = 0, len, node = -1, found = 0, i, count;
	size_t parameter_len = strcspn(assignment, "=");

	if (!assignment[parameter_len] || !parameter_len ||
	    parameter_len >= sizeof(parameter)) {
		errno = EINVAL;
		return -1;
	}
	memcpy(parameter, assignment, parameter_len);
	parameter[parameter_len] = '\0';
	errno = 0;
	value = strtoul(assignment + parameter_len + 1, &end, 0);
	if (errno || end == assignment + parameter_len + 1 || *end ||
	    assignment[parameter_len + 1] == '-' || value > UINT32_MAX) {
		errno = EINVAL;
		return -1;
	}

	while ((node = fdt_next_node(blob, node, &depth)) >= 0) {
		node_parameter = fdt_getprop(blob, node,
					     "espressif,param-name", &len);
		if (!node_parameter || strcmp(node_parameter, parameter))
			continue;
		values = fdt_getprop(blob, node, "espressif,param-values", &len);
		if (!values || len <= 0 || len % sizeof(*values)) {
			errno = EINVAL;
			return -1;
		}
		count = len / sizeof(*values);
		for (i = 0; i < count; i++)
			if (fdt32_to_cpu(values[i]) == value)
				break;
		if (i == count) {
			errno = ERANGE;
			return -1;
		}
		property = fdt_getprop_w(blob, node, parameter, &len);
		if (!property || len != sizeof(*property)) {
			errno = EINVAL;
			return -1;
		}
		*property = cpu_to_fdt32(value);
		found++;
	}
	if (!found) {
		errno = ENOENT;
		return -1;
	}
	return 0;
}

static int patch_assignment(void *blob, const char *assignment)
{
	if (!patch_parameter(blob, assignment))
		return 0;
	if (errno != ENOENT)
		return -1;
	return patch_route(blob, assignment);
}

/* Repeated keys would make an edit depend on argument order. Reject them in
 * both runtime requests and the offline configuration checker. */
static int patch_tokens(void *blob, char **save)
{
	char *token, *seen[128];
	unsigned int count = 0, i;
	while ((token = strtok_r(NULL, " \t", save))) {
		size_t length = strcspn(token, "=");
		if (!length || token[length] != '=' || count == 128) {
			errno = EINVAL;
			return -1;
		}
		for (i = 0; i < count; i++)
			if (strcspn(seen[i], "=") == length &&
			    !strncmp(seen[i], token, length)) {
				errno = EINVAL;
				return -1;
			}
		seen[count++] = token;
		if (patch_assignment(blob, token))
			return -1;
	}
	return 0;
}

static int prepare_spec_blob(const char *spec, void **blob, size_t *size)
{
	char copy[512], *save, *name;

	if (strlen(spec) >= sizeof(copy)) {
		errno = E2BIG;
		return -1;
	}
	strcpy(copy, spec);
	name = strtok_r(copy, " \t", &save);
	if (!name || !valid_name(name)) { errno = EINVAL; return -1; }
	if (load_blob(name, blob, size))
		return -1;
	/* Unmodified blobs are validated by the kernel. libfdt patching must
	 * establish allocation bounds before following header offsets. */
	if (save && *save && (*size < sizeof(struct fdt_header) ||
	    fdt_check_header(*blob) || fdt_totalsize(*blob) > *size)) {
		errno = EINVAL;
		free(*blob);
		return -1;
	}
	if (patch_tokens(*blob, &save)) {
		free(*blob);
		return -1;
	}
	return 0;
}

static int apply_blob(const void *blob, size_t size)
{
	int fd = open(OVERLAY_DEVICE, O_WRONLY), ret;
	if (fd < 0) return -1;
	ret = write(fd, blob, size) == (ssize_t)size ? 0 : -1;
	close(fd);
	return ret;
}

static int apply_spec(const char *spec)
{
	void *blob;
	size_t size;
	int ret;
	if (prepare_spec_blob(spec, &blob, &size)) return -1;
	ret = apply_blob(blob, size);
	free(blob);
	return ret;
}

static int read_current(char *config, size_t size)
{
	FILE *file = fopen(current_file(), "r");
	size_t got;

	config[0] = '\0';
	if (!file)
		return errno == ENOENT ? 0 : -1;
	got = fread(config, 1, size - 1, file);
	if (ferror(file) || (got == size - 1 && fgetc(file) != EOF)) {
		fclose(file);
		errno = EOVERFLOW;
		return -1;
	}
	if (memchr(config, '\0', got)) {
		fclose(file);
		errno = EINVAL;
		return -1;
	}
	config[got] = '\0';
	fclose(file);
	return 0;
}

static int write_current(const char *config)
{
	char temporary[512];
	int fd;
	size_t length = strlen(config);

	if (snprintf(temporary, sizeof(temporary), "%s.XXXXXX", current_file()) >=
	    (int)sizeof(temporary)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	fd = mkstemp(temporary);

	if (fd < 0)
		return -1;
	if (write(fd, config, length) != (ssize_t)length || fsync(fd) ||
	    close(fd) || rename(temporary, current_file())) {
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
	struct shown_route {
		const char *name;
		uint32_t gpio;
	} shown[64];
	void *blob;
	size_t size;
	int node = -1, depth = 0, len, found = 0, shown_count = 0;

	if (load_blob(name, &blob, &size))
		return -1;
	while ((node = fdt_next_node(blob, node, &depth)) >= 0) {
		const char *route = fdt_getprop(blob, node,
						"espressif,route-name", &len);
		const char *kind;
		const fdt32_t *pinmux;
		uint32_t gpio;
		int i;
		int count = fdt_stringlist_count(blob, node,
						 "espressif,route-names");

		if (count > 0) {
			pinmux = fdt_getprop(blob, node, "pinmux", &len);
			if (!pinmux || len != count * (int)sizeof(*pinmux)) {
				free(blob);
				errno = EINVAL;
				return -1;
			}
			for (i = 0; i < count; i++) {
				route = fdt_stringlist_get(blob, node,
						"espressif,route-names", i, NULL);
				kind = fdt_stringlist_get(blob, node,
						"espressif,route-kinds", i, NULL);
				if (!route || !kind)
					continue;
				gpio = fdt32_to_cpu(pinmux[i]) & 0xff;
				printf("%s=%u\n", route, gpio);
				found++;
			}
			continue;
		}

		if (!route)
			continue;
		kind = fdt_getprop(blob, node, "espressif,route-kind", &len);
		pinmux = fdt_getprop(blob, node, "pinmux", &len);
		if (kind && pinmux && len >= (int)sizeof(*pinmux) &&
		    !(len % sizeof(*pinmux))) {
			gpio = fdt32_to_cpu(*pinmux) & 0xff;
			for (i = 0; i < shown_count; i++) {
				if (strcmp(shown[i].name, route))
					continue;
				if (shown[i].gpio != gpio) {
					fprintf(stderr,
						"route %s has conflicting GPIO values\n",
						route);
					free(blob);
					errno = EINVAL;
					return -1;
				}
				break;
			}
			if (i < shown_count)
				continue;
			if ((size_t)shown_count == sizeof(shown) / sizeof(shown[0])) {
				free(blob);
				errno = E2BIG;
				return -1;
			}
			shown[shown_count].name = route;
			shown[shown_count++].gpio = gpio;
			printf("%s=%u\n", route, gpio);
			found++;
		}
	}
	free(blob);
	return found ? 0 : 1;
}

static int list_parameters(const char *name)
{
	void *blob;
	size_t size;
	int node = -1, depth = 0, len, found = 0;

	if (load_blob(name, &blob, &size))
		return -1;
	while ((node = fdt_next_node(blob, node, &depth)) >= 0) {
		const char *parameter = fdt_getprop(blob, node,
						    "espressif,param-name", &len);
		const fdt32_t *property, *values;
		int i, count;

		if (!parameter)
			continue;
		property = fdt_getprop(blob, node, parameter, &len);
		if (!property || len != sizeof(*property)) {
			free(blob);
			errno = EINVAL;
			return -1;
		}
		values = fdt_getprop(blob, node, "espressif,param-values", &len);
		if (!values || len <= 0 || len % sizeof(*values)) {
			free(blob);
			errno = EINVAL;
			return -1;
		}
		count = len / sizeof(*values);
		printf("%s=%u values=", parameter, fdt32_to_cpu(*property));
		for (i = 0; i < count; i++)
			printf("%s%u", i ? "," : "", fdt32_to_cpu(values[i]));
		putchar('\n');
		found++;
	}
	free(blob);
	return found ? 0 : 1;
}

/* Read-only interface for configuration frontends. Values come from the
 * packaged defaults plus recorded overrides, never from a diagnostic label. */
static int valid_field_string(const char *value, int length)
{
	int i;
	if (!value || length < 2 || length > 64 ||
	    strnlen(value, (size_t)length) != (size_t)length - 1)
		return 0;
	for (i = 0; i < length - 1; i++)
		if (!isalnum((unsigned char)value[i]) && value[i] != '-' &&
		    value[i] != '_' && value[i] != '.') return 0;
	return 1;
}

struct profile_field {
	const char *name, *kind;
	const fdt32_t *allowed;
	uint32_t value;
	int allowed_length;
};

static int record_field(struct profile_field *fields, int *count,
			const struct profile_field *field)
{
	int i;
	for (i = 0; i < *count; i++) {
		if (strcmp(fields[i].name, field->name)) continue;
		if (fields[i].value != field->value ||
		    !!fields[i].kind != !!field->kind ||
		    (field->kind && strcmp(fields[i].kind, field->kind)) ||
		    fields[i].allowed_length != field->allowed_length ||
		    (field->allowed_length && memcmp(fields[i].allowed,
				field->allowed, field->allowed_length))) {
			errno = EINVAL;
			return -1;
		}
		return 0;
	}
	if (*count == 128) { errno = E2BIG; return -1; }
	fields[(*count)++] = *field;
	return 0;
}

/* Header offsets must fit the allocation before libfdt walks the structure.
 * Validate exported metadata once before patching or emitting any TSV rows. */
static int check_profile_blob(const void *blob, size_t size, const char *name)
{
	struct profile_field fields[128];
	int field_count = 0, node = -1, len, count, i;
	const char *value;
	if (size < sizeof(struct fdt_header) || fdt_check_header(blob) ||
	    fdt_totalsize(blob) > size) goto invalid;
	value = fdt_getprop(blob, 0, "espressif,overlay-name", &len);
	if (!valid_field_string(value, len) || strcmp(name, value)) goto invalid;
	while ((node = fdt_next_node(blob, node, NULL)) >= 0) {
		const fdt32_t *pins;
		const char *route, *kind;
		int plural, pin_length, property, singular_length, kind_length;
		/* Reject broken property/string-table offsets, including metadata
		 * that an early successful lookup would otherwise hide. */
		for (property = fdt_first_property_offset(blob, node); property >= 0;
		     property = fdt_next_property_offset(blob, property))
			if (!fdt_getprop_by_offset(blob, property, &value, &len) || !value)
				goto invalid;
		if (property != -FDT_ERR_NOTFOUND) goto invalid;
		plural = fdt_stringlist_count(blob, node, "espressif,route-names");
		if (plural < 0 && plural != -FDT_ERR_NOTFOUND) goto invalid;
		route = fdt_getprop(blob, node, "espressif,route-name", &singular_length);
		if (route && !valid_field_string(route, singular_length)) goto invalid;
		if (plural >= 0 && (!plural || route)) goto invalid;
		count = plural > 0 ? plural : route ? 1 : 0;
		pins = fdt_getprop(blob, node, "pinmux", &pin_length);
		if (count && (!pins || pin_length <= 0 || pin_length % 4 ||
			     (plural > 0 && pin_length != count * 4))) goto invalid;
		if (plural > 0 && fdt_stringlist_count(blob, node,
				"espressif,route-kinds") != plural) goto invalid;
		for (i = 0; i < count; i++) {
			unsigned int index = plural > 0 ? (unsigned int)i : 0;
			struct profile_field field = { 0 };
			if (plural > 0) {
				route = fdt_stringlist_get(blob, node, "espressif,route-names", i, &len);
				if (!valid_field_string(route, len + 1)) goto invalid;
				kind = fdt_stringlist_get(blob, node, "espressif,route-kinds", i, &len);
				if (!valid_field_string(kind, len + 1)) goto invalid;
			} else {
				kind = fdt_getprop(blob, node, "espressif,route-kind", &kind_length);
				if (!valid_field_string(kind, kind_length)) goto invalid;
			}
			field.name = route; field.kind = kind;
			field.value = fdt32_to_cpu(pins[index]) & 0xff;
			if (!valid_gpio(field.value)) goto invalid;
			if (plural < 0)
				for (int j = 1; j < pin_length / 4; j++)
					if ((fdt32_to_cpu(pins[j]) & 0xff) != field.value) goto invalid;
			if (record_field(fields, &field_count, &field)) return -1;
		}
		value = fdt_getprop(blob, node, "espressif,param-name", &len);
		if (value) {
			struct profile_field field = { 0 };
			if (!valid_field_string(value, len)) goto invalid;
			field.name = value;
			pins = fdt_getprop(blob, node, value, &len);
			if (!pins || len != 4) goto invalid;
			field.value = fdt32_to_cpu(*pins);
			field.allowed = fdt_getprop(blob, node, "espressif,param-values", &len);
			if (!field.allowed || len <= 0 || len % 4) goto invalid;
			field.allowed_length = len;
			for (i = 0; i < len / 4; i++)
				if (fdt32_to_cpu(field.allowed[i]) == field.value) break;
			if (i == len / 4) goto invalid;
			if (record_field(fields, &field_count, &field)) return -1;
		}
		pins = fdt_getprop(blob, node, "espressif,gpio-claims", &len);
		if (pins) {
			if (len <= 0 || len % 4) goto invalid;
			for (i = 0; i < len / 4; i++)
				if (!valid_gpio(fdt32_to_cpu(pins[i]))) goto invalid;
		}
	}
	if (node != -FDT_ERR_NOTFOUND) goto invalid;
	return 0;
invalid:
	errno = EINVAL;
	return -1;
}

static int spec_for_name(const char *config, const char *name, char *out, size_t size)
{
	const char *line = config;
	size_t name_len = strlen(name);

	out[0] = '\0';
	while (*line) {
		size_t length = strcspn(line, "\n");
		if (length >= name_len && !strncmp(line, name, name_len) &&
		    (length == name_len || line[name_len] == ' ' || line[name_len] == '\t')) {
			if (length >= size || out[0]) {
				errno = EINVAL;
				return -1;
			}
			memcpy(out, line, length);
			out[length] = '\0';
		}
		line += length;
		if (*line) line++;
	}
	return out[0] != '\0';
}

static int patch_spec_blob(void *blob, const char *spec)
{
	char copy[512], *save;
	if (strlen(spec) >= sizeof(copy) || fdt_check_header(blob)) {
		errno = EINVAL;
		return -1;
	}
	strcpy(copy, spec);
	if (!strtok_r(copy, " \t", &save)) {
		errno = EINVAL;
		return -1;
	}
	return patch_tokens(blob, &save);
}

static void describe_value(const void *blob, int node, const char *property,
			   unsigned int index, int gpio)
{
	int len;
	const fdt32_t *value = blob ? fdt_getprop(blob, node, property, &len) : NULL;
	if (!value || len < (int)((index + 1) * sizeof(*value))) {
		putchar('-');
		return;
	}
	printf("%u", fdt32_to_cpu(value[index]) & (gpio ? 0xffU : UINT32_MAX));
}

static int describe_profile(const char *name)
{
	char current[CONFIG_LEN], saved[CONFIG_LEN], current_spec[512], saved_spec[512];
	struct s31_overlay_list active;
	const char *shown[128];
	void *defaults = NULL, *live = NULL, *desired = NULL;
	size_t size;
	int status, have_live, have_saved, node = -1, depth = 0, len, i, count;
	int shown_count = 0, ret = -1;

	if (!valid_name(name)) { errno = EINVAL; return -1; }
	if (load_blob(name, &defaults, &size))
		return -1;
	if (check_profile_blob(defaults, size, name)) goto out;
	if (read_current(current, sizeof(current))) goto out;
	if (read_persist(saved, sizeof(saved))) {
		if (errno != ENOENT) goto out;
		saved[0] = '\0';
	}
	have_live = spec_for_name(current, name, current_spec, sizeof(current_spec));
	have_saved = spec_for_name(saved, name, saved_spec, sizeof(saved_spec));
	if (have_live < 0 || have_saved < 0) goto out;
	status = manager_list(&active) ? -1 : has_active_overlay(&active, name);
	if (status == 1 && have_live) {
		live = malloc(size);
		if (!live) goto out;
		memcpy(live, defaults, size);
		if (patch_spec_blob(live, current_spec)) goto out;
	}
	if (have_saved) {
		desired = malloc(size);
		if (!desired) goto out;
		memcpy(desired, defaults, size);
		if (patch_spec_blob(desired, saved_spec)) goto out;
	}
	printf("profile\t%s\nactive\t%s\nsaved\t%d\ncurrent_known\t%d\n",
	       name, status < 0 ? "unknown" : status ? "1" : "0",
	       have_saved, live != NULL);
	while ((node = fdt_next_node(defaults, node, &depth)) >= 0) {
		const char *parameter = fdt_getprop(defaults, node, "espressif,param-name", &len);
		const char *route = fdt_getprop(defaults, node, "espressif,route-name", &len);
		const char *kind;
		const fdt32_t *values, *pins;
		int plural = fdt_stringlist_count(defaults, node, "espressif,route-names");
		count = plural > 0 ? plural : route ? 1 : 0;
		for (i = 0; i < count; i++) {
			int j;
			unsigned int index = plural > 0 ? (unsigned int)i : 0;
			if (plural > 0) {
				route = fdt_stringlist_get(defaults, node, "espressif,route-names", i, NULL);
				kind = fdt_stringlist_get(defaults, node, "espressif,route-kinds", i, NULL);
			} else kind = fdt_getprop(defaults, node, "espressif,route-kind", &len);
			if (!route || !kind) { errno = EINVAL; goto out; }
			for (j = 0; j < shown_count && strcmp(shown[j], route); j++);
			if (j < shown_count) continue;
			if (shown_count >= (int)(sizeof(shown) / sizeof(shown[0]))) {
				errno = E2BIG; goto out;
			}
			shown[shown_count++] = route;
			printf("route\t%s\t", route);
			describe_value(defaults, node, "pinmux", index, 1); putchar('\t');
			describe_value(live, node, "pinmux", index, 1); putchar('\t');
			describe_value(desired, node, "pinmux", index, 1);
			printf("\t%s\n", kind);
		}
		if (parameter) {
			int j;
			for (j = 0; j < shown_count && strcmp(shown[j], parameter); j++);
			if (j < shown_count) parameter = NULL;
		}
		if (parameter) {
			if (shown_count >= (int)(sizeof(shown) / sizeof(shown[0]))) {
				errno = E2BIG; goto out;
			}
			shown[shown_count++] = parameter;
			values = fdt_getprop(defaults, node, "espressif,param-values", &len);
			if (!values || len <= 0 || len % (int)sizeof(*values)) {
				errno = EINVAL; goto out;
			}
			count = len / (int)sizeof(*values);
			printf("parameter\t%s\t", parameter);
			describe_value(defaults, node, parameter, 0, 0); putchar('\t');
			describe_value(live, node, parameter, 0, 0); putchar('\t');
			describe_value(desired, node, parameter, 0, 0); putchar('\t');
			for (i = 0; i < count; i++)
				printf("%s%u", i ? "," : "", fdt32_to_cpu(values[i]));
			putchar('\n');
		}
		/* Explicit claims describe fixed wiring, unlike matrix routes. */
		pins = fdt_getprop(defaults, node, "espressif,gpio-claims", &len);
		if (pins && len > 0 && !(len % (int)sizeof(*pins)))
			for (i = 0; i < len / (int)sizeof(*pins); i++)
				printf("fixed_gpio\t%u\n", fdt32_to_cpu(pins[i]));
	}
	ret = 0;
out:
	free(defaults); free(live); free(desired);
	return ret;
}

static int check_configuration(const char *path)
{
	char config[CONFIG_LEN], copy[512], *line, *save, *name;
	void *blob;
	size_t size;
	if (setenv("S31_OVERLAY_PERSIST", path, 1) ||
	    read_persist(config, sizeof(config))) return -1;
	for (line = strtok_r(config, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (strlen(line) >= sizeof(copy)) { errno = E2BIG; return -1; }
		strcpy(copy, line);
		name = strtok(copy, " \t");
		if (!name || load_blob(name, &blob, &size)) return -1;
		if (check_profile_blob(blob, size, name) || patch_spec_blob(blob, line)) {
			free(blob); return -1;
		}
		free(blob);
	}
	return 0;
}

/* Equivalent settings need only be saved. Compare patched copies of the
 * packaged DTBO so key order and explicit default values do not cause a
 * driver reprobe. A missing, malformed or inactive current record cannot
 * establish equivalence and must take the normal apply path. */
static int active_spec_matches(const char *name, const void *requested,
			       size_t requested_size, const char *current)
{
	char spec[512];
	struct s31_overlay_list active;
	void *recorded;
	size_t size;
	int match = 0;
	if (spec_for_name(current, name, spec, sizeof(spec)) != 1)
		return 0;
	if (load_blob(name, &recorded, &size))
		return 0;
	if (size == requested_size && !check_profile_blob(recorded, size, name) &&
	    !patch_spec_blob(recorded, spec) && !memcmp(recorded, requested, size) &&
	    !manager_list(&active) && has_active_overlay(&active, name))
		match = 1;
	free(recorded);
	return match;
}

/* Invalidate only the changing profile before touching hardware. If the
 * final state write fails after an apply, an old record must not masquerade
 * as known current settings. Failure to invalidate aborts before the apply. */
static int invalidate_current_profile(const char *config, const char *name)
{
	char unknown[CONFIG_LEN];
	strcpy(unknown, config);
	if (update_config(unknown, sizeof(unknown), name, NULL)) return -1;
	return write_current(unknown);
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s list|status|restore|routes NAME|parameters NAME\n"
		"  %s describe NAME | check CONFIG_FILE\n"
		"  %s apply NAME [KEY=VALUE ...] [--volatile]\n"
		"  %s remove NAME|--all [--volatile]\n", program, program, program, program);
}

static int overlay_main(int argc, char **argv)
{
	char config[CONFIG_LEN] = "", persisted[CONFIG_LEN] = "", spec[512];
	struct s31_overlay_list active;
	int persist = 1, i, ret;

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "describe") && argc == 3) {
		ret = describe_profile(argv[2]);
		if (ret) perror("describe overlay");
		return !!ret;
	}
	if (!strcmp(argv[1], "check") && argc == 3) {
		ret = check_configuration(argv[2]);
		if (ret) perror("check overlay configuration");
		return !!ret;
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
	if (!strcmp(argv[1], "parameters") && argc == 3) {
		ret = list_parameters(argv[2]);
		if (ret < 0)
			perror("list parameters");
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
		if (!read_persist(persisted, sizeof(persisted)))
			printf("persisted:\n%s", persisted[0] ? persisted : "  (none)\n");
		else
			puts("persisted: (none)");
		return 0;
	}
	if (!strcmp(argv[1], "restore")) {
		int usb_was_active = 0;

		if (read_persist(config, sizeof(config)))
			return errno == ENODATA || errno == ENODEV || errno == ENOENT ?
				0 : 1;
		if (!manager_list(&active))
			usb_was_active = has_active_overlay(&active, "usb-device");
		if (manager_remove(NULL) && errno != ENOENT)
			return 1;
		if (usb_was_active && reprobe_usb()) {
			perror("reprobe USB host");
			return 1;
		}
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
				if (spec_is_usb_device(line) && reprobe_usb()) {
					perror("reprobe USB device");
					manager_remove("usb-device");
					reprobe_usb();
					return 1;
				}
			}
		}
		return write_current(config) ? 1 : 0;
	}
	if (!strcmp(argv[1], "apply")) {
		void *blob;
		size_t used, size;
		int unchanged;
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
		if (read_current(config, sizeof(config))) {
			perror("prepare active overlay set");
			return 1;
		}
		if (persist) {
			if ((read_persist(persisted, sizeof(persisted)) && errno != ENOENT) ||
			    update_config(persisted, sizeof(persisted), argv[2], spec)) {
				perror("prepare persistent overlay set");
				return 1;
			}
		}
		if (prepare_spec_blob(spec, &blob, &size)) {
			perror("prepare overlay");
			return 1;
		}
		unchanged = active_spec_matches(argv[2], blob, size, config);
		if (update_config(config, sizeof(config), argv[2], spec)) {
			perror("prepare active overlay set");
			free(blob);
			return 1;
		}
		if (!unchanged && invalidate_current_profile(config, argv[2])) {
			perror("invalidate current overlay record");
			free(blob);
			return 1;
		}
		if (!unchanged && apply_blob(blob, size)) {
			perror("apply overlay");
			free(blob);
			return 1;
		}
		free(blob);
		if (!unchanged && spec_is_usb_device(spec) && reprobe_usb()) {
			perror("reprobe USB device");
			manager_remove("usb-device");
			reprobe_usb();
			return 1;
		}
		if (write_current(config) || (persist && write_persist(persisted))) {
			perror(unchanged ? "overlay unchanged, but recording state failed" :
			       "overlay applied, but recording state failed");
			return 1;
		}
		return 0;
	}
	if (!strcmp(argv[1], "remove")) {
		const char *name;
		int usb_was_active = 0;
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
		if ((!name || !strcmp(name, "usb-device")) &&
		    !manager_list(&active))
			usb_was_active = has_active_overlay(&active, "usb-device");
		if (read_current(config, sizeof(config)) ||
		    (name ? update_config(config, sizeof(config), name, NULL) :
		     (config[0] = '\0', 0))) {
			perror("prepare active overlay set");
			return 1;
		}
		if (persist) {
			if ((read_persist(persisted, sizeof(persisted)) && errno != ENOENT) ||
			    (name ? update_config(persisted, sizeof(persisted), name, NULL) :
			     (persisted[0] = '\0', 0))) {
				perror("prepare persistent overlay set");
				return 1;
			}
		}
		if (manager_remove(name) && errno != ENOENT) {
			perror("remove overlay");
			return 1;
		}
		if (usb_was_active && reprobe_usb()) {
			perror("reprobe USB host");
			return 1;
		}
		if (write_current(config) || (persist && write_persist(persisted))) {
			perror("overlay removed, but recording state failed");
			return 1;
		}
		return 0;
	}
	usage(argv[0]);
	return 2;
}

int main(int argc, char **argv)
{
	int fd, ret;

	if (argc < 2 || (!strcmp(argv[1], "list") ||
			!strcmp(argv[1], "routes") ||
			!strcmp(argv[1], "check") ||
			!strcmp(argv[1], "parameters")))
		return overlay_main(argc, argv);
	fd = open(LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0 || flock(fd, LOCK_EX)) {
		perror("lock overlay state");
		if (fd >= 0)
			close(fd);
		return 1;
	}
	ret = overlay_main(argc, argv);
	close(fd);
	return ret;
}
