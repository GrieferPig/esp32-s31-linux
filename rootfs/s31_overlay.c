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

static int read_persist(char *config, size_t config_size)
{
	FILE *file;
	char line[512], *equal, *name, *value;
	size_t used = 0;

	config[0] = '\0';
	file = fopen(persist_file(), "r");
	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		size_t length = strcspn(line, "\r\n");
		if (length == sizeof(line) - 1 && !feof(file)) {
			fclose(file);
			errno = EOVERFLOW;
			return -1;
		}
		line[length] = '\0';
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
		used += snprintf(config + used, config_size - used, "%s\n", value);
	}
	if (ferror(file)) {
		fclose(file);
		errno = EIO;
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
	if (errno || *end || !valid_gpio(gpio)) {
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
	if (errno || *end || value > UINT32_MAX) {
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
		if (patch_assignment(blob, token))
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
	if (ferror(file) || (got == size - 1 && fgetc(file) != EOF)) {
		fclose(file);
		errno = EOVERFLOW;
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

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s list|status|restore|routes NAME|parameters NAME\n"
		"  %s apply NAME [KEY=VALUE ...] [--volatile]\n"
		"  %s remove NAME|--all [--volatile]\n", program, program, program);
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
		if (read_current(config, sizeof(config)) ||
		    update_config(config, sizeof(config), argv[2], spec)) {
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
		if (apply_spec(spec)) {
			perror("apply overlay");
			return 1;
		}
		if (spec_is_usb_device(spec) && reprobe_usb()) {
			perror("reprobe USB device");
			manager_remove("usb-device");
			reprobe_usb();
			return 1;
		}
		if (write_current(config) || (persist && write_persist(persisted))) {
			perror("overlay applied, but recording state failed");
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
		if (manager_remove(name)) {
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
