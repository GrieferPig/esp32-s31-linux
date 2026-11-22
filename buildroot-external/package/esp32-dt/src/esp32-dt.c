// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <libfdt.h>

#include "esp32s31-overlay.h"

#define MAX_FDT (64 * 1024)

static int read_tree(const char *path, void *tree)
{
	FILE *input = fopen(path, "rb");
	size_t len;
	int ret;

	if (!input) {
		perror(path);
		return 1;
	}
	len = fread(tree, 1, MAX_FDT, input);
	if (ferror(input)) {
		perror(path);
		fclose(input);
		return 1;
	}
	if (fgetc(input) != EOF || ferror(input)) {
		fprintf(stderr, "%s exceeds %u bytes or could not be read\n", path, MAX_FDT);
		fclose(input);
		return 1;
	}
	fclose(input);
	ret = len < sizeof(struct fdt_header) ? -FDT_ERR_TRUNCATED :
		fdt_check_header(tree);
	if (!ret && fdt_totalsize(tree) > len)
		ret = -FDT_ERR_TRUNCATED;
	if (ret) {
		fprintf(stderr, "%s: %s\n", path, fdt_strerror(ret));
		return 1;
	}
	return 0;
}

static int print_tree(const void *tree, const char *operation,
		      const char *path, const char *property)
{
	int node, offset, size;
	const char *name;
	const unsigned char *data;

	if (!strcmp(operation, "nodes")) {
		char path_buffer[1024];

		for (node = 0; node >= 0; node = fdt_next_node(tree, node, NULL)) {
			int ret = fdt_get_path(tree, node, path_buffer, sizeof(path_buffer));

			if (ret)
				return ret;
			puts(path_buffer);
		}
		return node == -FDT_ERR_NOTFOUND ? 0 : node;
	}
	node = fdt_path_offset(tree, path);
	if (node < 0)
		return node;
	if (!strcmp(operation, "properties")) {
		fdt_for_each_property_offset(offset, tree, node) {
			if (!fdt_getprop_by_offset(tree, offset, &name, &size))
				return size;
			printf("%s\n", name);
		}
		return offset == -FDT_ERR_NOTFOUND ? 0 : offset;
	}
	data = fdt_getprop(tree, node, property, &size);
	if (!data)
		return size;
	for (int i = 0; i < size; i++)
		printf("%02x", data[i]);
	putchar('\n');
	return 0;
}

static int hex_digit(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int encode(const char *type, int count, char **args, unsigned char *data)
{
	size_t len = 0;

	if (!strcmp(type, "empty"))
		return count == 0 ? 0 : -FDT_ERR_BADVALUE;
	if (count == 0)
		return -FDT_ERR_BADVALUE;
	for (int i = 0; i < count; i++) {
		if (!strcmp(type, "string")) {
			size_t size = strlen(args[i]) + 1;

			if (size > MAX_FDT - len)
				return -FDT_ERR_NOSPACE;
			memcpy(data + len, args[i], size);
			len += size;
		} else if (!strcmp(type, "u32")) {
			char *end;
			unsigned long long number;
			fdt32_t cell;

			errno = 0;
			number = strtoull(args[i], &end, 0);
			if (errno || end == args[i] || *end || args[i][0] == '-' ||
			    number > UINT32_MAX)
				return -FDT_ERR_BADVALUE;
			if (sizeof(cell) > MAX_FDT - len)
				return -FDT_ERR_NOSPACE;
			cell = cpu_to_fdt32(number);
			memcpy(data + len, &cell, sizeof(cell));
			len += sizeof(cell);
		} else if (!strcmp(type, "hex")) {
			size_t digits = strlen(args[i]);

			if (digits % 2 || digits / 2 > MAX_FDT - len)
				return -FDT_ERR_BADVALUE;
			for (size_t j = 0; j < digits; j += 2) {
				int high = hex_digit(args[i][j]);
				int low = hex_digit(args[i][j + 1]);

				if (low < 0 || high < 0)
					return -FDT_ERR_BADVALUE;
				data[len++] = (high << 4) | low;
			}
		} else {
			return -FDT_ERR_BADVALUE;
		}
	}
	return len;
}

static int add_fragment(void *tree, const char *name, const char *path)
{
	int fragment, ret;

	ret = fdt_create_empty_tree(tree, MAX_FDT);
	if (ret)
		return ret;
	ret = fdt_setprop_string(tree, 0, "espressif,overlay-name", name);
	if (ret)
		return ret;
	fragment = fdt_add_subnode(tree, 0, "fragment@0");
	if (fragment < 0)
		return fragment;
	ret = fdt_setprop_string(tree, fragment, "target-path", path);
	if (ret)
		return ret;
	return fdt_add_subnode(tree, fragment, "__overlay__");
}

static int edit_tree(const char *operation, int argc, char **argv, void *tree)
{
	const char *path = argv[3];
	const char *property = argv[4];
	unsigned char *data = malloc(MAX_FDT);
	int len, node, ret;

	if (!data)
		return -FDT_ERR_NOSPACE;
	len = encode(argv[5], argc - 6, argv + 6, data);
	if (len < 0) {
		free(data);
		return len;
	}
	if (!strcmp(operation, "overlay")) {
		node = add_fragment(tree, argv[2], path);
	} else {
		if (read_tree(argv[2], tree)) {
			free(data);
			return -FDT_ERR_BADVALUE;
		}
		ret = fdt_open_into(tree, tree, MAX_FDT);
		node = ret ? ret : fdt_path_offset(tree, path);
	}
	ret = node < 0 ? node : fdt_setprop(tree, node, property, data, len);
	free(data);
	return ret ? ret : fdt_pack(tree);
}

static int run_overlay(const char *operation, const char *arg, void *tree)
{
	struct s31_overlay_name name = {0};
	struct s31_overlay_list list;
	int fd, ret = -1;
	size_t len;

	if (!strcmp(operation, "apply") && read_tree(arg, tree))
		return 1;
	fd = open("/dev/esp32-overlay", O_RDWR | O_CLOEXEC);
	if (fd < 0 && errno == ENOENT)
		fd = open("/dev/s31-overlay", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open overlay device");
		return 1;
	}
	if (!strcmp(operation, "apply")) {
		len = fdt_totalsize(tree);
		ret = write(fd, tree, len);
		if (ret >= 0 && (size_t)ret != len) {
			errno = EIO;
			ret = -1;
		}
	} else if (!strcmp(operation, "remove")) {
		len = strlen(arg);
		if (len && len < sizeof(name.name)) {
			memcpy(name.name, arg, len);
			ret = ioctl(fd, S31_OVERLAY_IOC_REMOVE_NAME, &name);
		} else {
			errno = EINVAL;
		}
	} else {
		ret = ioctl(fd, S31_OVERLAY_IOC_LIST, &list);
		if (ret == 0) {
			for (uint32_t i = 0; i < list.count && i < S31_OVERLAY_MAX_ACTIVE; i++)
				printf("%.*s %d\n", (int)sizeof(list.items[i].name),
				       list.items[i].name, list.items[i].id);
		}
	}
	if (ret < 0)
		perror(operation);
	close(fd);
	return ret < 0;
}

int main(int argc, char **argv)
{
	static unsigned char tree[MAX_FDT] __attribute__((aligned(8)));
	int ret;
	const char *operation;

	if (argc < 2)
		goto usage;
	operation = argv[1];
	if ((!strcmp(operation, "active") && argc == 2) ||
	    ((!strcmp(operation, "apply") || !strcmp(operation, "remove")) && argc == 3))
		return run_overlay(operation, argc == 3 ? argv[2] : NULL, tree);
	if ((!strcmp(operation, "nodes") && argc == 3) ||
	    (!strcmp(operation, "properties") && argc == 4) ||
	    (!strcmp(operation, "get") && argc == 5)) {
		if (read_tree(argv[2], tree))
			return 1;
		ret = print_tree(tree, operation, argc > 3 ? argv[3] : NULL,
				 argc > 4 ? argv[4] : NULL);
	} else if (argc >= 6 && (!strcmp(operation, "set") ||
				 !strcmp(operation, "overlay"))) {
		if (argv[3][0] != '/' || !argv[4][0] || strchr(argv[4], '/'))
			goto usage;
		ret = edit_tree(operation, argc, argv, tree);
		if (!ret) {
			size_t len = fdt_totalsize(tree);

			if (fwrite(tree, 1, len, stdout) != len || fflush(stdout)) {
				perror("write device tree");
				return 1;
			}
		}
	} else {
		goto usage;
	}
	if (ret) {
		fprintf(stderr, "%s: %s\n", operation, fdt_strerror(ret));
		return 1;
	}
	return 0;

usage:
	fputs("Usage: esp32-dt nodes DTB | properties DTB PATH | get DTB PATH PROPERTY\n"
	      "       esp32-dt set DTB PATH PROPERTY TYPE [VALUE ...]\n"
	      "       esp32-dt overlay NAME PATH PROPERTY TYPE [VALUE ...]\n"
	      "Types: string, u32, hex, empty. Outputs a DTB/DTBO for set/overlay.\n",
	      stderr);
	return 2;
}
