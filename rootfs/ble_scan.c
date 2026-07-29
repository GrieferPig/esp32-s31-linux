// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal BLE discovery client for Linux's Bluetooth Management interface.
 * The kernel selects the controller's supported extended scan commands.
 */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BTPROTO_HCI		1
#define HCI_CHANNEL_CONTROL	3
#define HCI_DEV_NONE		0xffff
#define MGMT_OP_SET_POWERED	0x0005
#define MGMT_OP_START_DISCOVERY	0x0023
#define MGMT_OP_STOP_DISCOVERY	0x0024
#define MGMT_EV_CMD_COMPLETE	0x0001
#define MGMT_EV_CMD_STATUS	0x0002
#define MGMT_EV_DEVICE_FOUND	0x0012
#define MGMT_DISCOVERY_LE	0x06
#define MAX_DEVICES		128

struct sockaddr_hci {
	sa_family_t family;
	uint16_t dev;
	uint16_t channel;
};

struct seen_device {
	uint8_t address[6];
	uint8_t type;
};

static uint16_t get_le16(const uint8_t *data)
{
	return data[0] | ((uint16_t)data[1] << 8);
}

static void put_le16(uint8_t *data, uint16_t value)
{
	data[0] = value & 0xff;
	data[1] = value >> 8;
}

static int remaining_ms(const struct timespec *deadline)
{
	struct timespec now;
	int64_t value;

	clock_gettime(CLOCK_MONOTONIC, &now);
	value = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000 +
		(deadline->tv_nsec - now.tv_nsec) / 1000000;
	if (value <= 0)
		return 0;
	return value > INT32_MAX ? INT32_MAX : (int)value;
}

static struct timespec deadline_after(int seconds)
{
	struct timespec deadline;

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += seconds;
	return deadline;
}

static int mgmt_command(int fd, uint16_t opcode, uint16_t index,
			const void *parameters, uint16_t parameter_len)
{
	uint8_t command[6 + 255];
	uint8_t event[1024];
	struct timespec deadline = deadline_after(10);

	put_le16(command, opcode);
	put_le16(command + 2, index);
	put_le16(command + 4, parameter_len);
	memcpy(command + 6, parameters, parameter_len);
	if (write(fd, command, 6 + parameter_len) != 6 + parameter_len)
		return -1;

	while (remaining_ms(&deadline)) {
		struct pollfd poll_fd = { .fd = fd, .events = POLLIN };
		ssize_t length;
		int ret;

		do {
			ret = poll(&poll_fd, 1, remaining_ms(&deadline));
		} while (ret < 0 && errno == EINTR);
		if (ret <= 0) {
			if (!ret)
				errno = ETIMEDOUT;
			return -1;
		}
		do {
			length = read(fd, event, sizeof(event));
		} while (length < 0 && errno == EINTR);
		if (length < 9 || get_le16(event + 2) != index)
			continue;
		if (get_le16(event) == MGMT_EV_CMD_COMPLETE &&
		    get_le16(event + 6) == opcode) {
			if (event[8]) {
				fprintf(stderr,
					"Management command 0x%04x status 0x%02x\n",
					opcode, event[8]);
				errno = EIO;
				return -1;
			}
			return 0;
		}
		if (get_le16(event) == MGMT_EV_CMD_STATUS &&
		    get_le16(event + 6) == opcode) {
			if (event[8]) {
				fprintf(stderr,
					"Management command 0x%04x status 0x%02x\n",
					opcode, event[8]);
				errno = EIO;
				return -1;
			}
			return 0;
		}
	}
	errno = ETIMEDOUT;
	return -1;
}

static void advertisement_name(const uint8_t *data, size_t length,
			       char *name, size_t name_size)
{
	size_t offset = 0;

	name[0] = '\0';
	while (offset < length) {
		size_t field_len = data[offset];
		size_t copy_len;
		size_t i;

		if (!field_len || offset + field_len >= length)
			break;
		if ((data[offset + 1] == 0x08 ||
		     data[offset + 1] == 0x09) && field_len > 1) {
			copy_len = field_len - 1;
			if (copy_len >= name_size)
				copy_len = name_size - 1;
			for (i = 0; i < copy_len; i++) {
				uint8_t value = data[offset + 2 + i];

				name[i] = value >= 0x20 && value < 0x7f ?
					(char)value : '.';
			}
			name[copy_len] = '\0';
			if (data[offset + 1] == 0x09)
				return;
		}
		offset += field_len + 1;
	}
}

static int already_seen(struct seen_device *devices, size_t count,
			const uint8_t *address, uint8_t type)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (devices[i].type == type &&
		    !memcmp(devices[i].address, address, 6))
			return 1;
	}
	return 0;
}

static void handle_device(const uint8_t *event, size_t length,
			  struct seen_device *devices, size_t *device_count)
{
	const uint8_t *payload = event + 6;
	const uint8_t *address;
	const uint8_t *eir;
	uint16_t payload_len;
	uint16_t eir_len;
	uint8_t type;
	int8_t rssi;
	char name[64];

	if (length < 20)
		return;
	payload_len = get_le16(event + 4);
	if ((size_t)payload_len + 6 > length || payload_len < 14)
		return;
	address = payload;
	type = payload[6];
	rssi = (int8_t)payload[7];
	eir_len = get_le16(payload + 12);
	eir = payload + 14;
	if ((size_t)eir_len + 14 > payload_len ||
	    already_seen(devices, *device_count, address, type))
		return;
	if (*device_count < MAX_DEVICES) {
		memcpy(devices[*device_count].address, address, 6);
		devices[*device_count].type = type;
		(*device_count)++;
	}
	advertisement_name(eir, eir_len, name, sizeof(name));
	printf("%02x:%02x:%02x:%02x:%02x:%02x  %-6s RSSI %4d dBm",
	       address[5], address[4], address[3],
	       address[2], address[1], address[0],
	       type == 1 ? "public" : type == 2 ? "random" : "bredr", rssi);
	if (name[0])
		printf("  %s", name);
	putchar('\n');
}

int main(int argc, char **argv)
{
	struct sockaddr_hci address = {
		.family = AF_BLUETOOTH,
		.dev = HCI_DEV_NONE,
		.channel = HCI_CHANNEL_CONTROL,
	};
	struct seen_device devices[MAX_DEVICES];
	const uint8_t powered = 1;
	const uint8_t discovery_type = MGMT_DISCOVERY_LE;
	struct timespec deadline;
	size_t device_count = 0;
	int duration = 15;
	int fd;

	if (argc > 2) {
		fprintf(stderr, "Usage: %s [seconds]\n", argv[0]);
		return 2;
	}
	if (argc == 2) {
		char *end;
		long value = strtol(argv[1], &end, 10);

		if (*end || value < 1 || value > 300) {
			fprintf(stderr, "Scan duration must be 1..300 seconds\n");
			return 2;
		}
		duration = value;
	}

	fd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address))) {
		perror("open Bluetooth management socket");
		if (fd >= 0)
			close(fd);
		return 1;
	}
	if (mgmt_command(fd, MGMT_OP_SET_POWERED, 0, &powered, 1) ||
	    mgmt_command(fd, MGMT_OP_START_DISCOVERY, 0,
			 &discovery_type, 1)) {
		perror("start BLE discovery");
		close(fd);
		return 1;
	}

	printf("Scanning BLE devices on hci0 for %d seconds...\n", duration);
	deadline = deadline_after(duration);
	while (remaining_ms(&deadline)) {
		struct pollfd poll_fd = { .fd = fd, .events = POLLIN };
		uint8_t event[1024];
		ssize_t length;
		int ret;

		do {
			ret = poll(&poll_fd, 1, remaining_ms(&deadline));
		} while (ret < 0 && errno == EINTR);
		if (ret <= 0)
			break;
		do {
			length = read(fd, event, sizeof(event));
		} while (length < 0 && errno == EINTR);
		if (length >= 6 && get_le16(event) == MGMT_EV_DEVICE_FOUND &&
		    get_le16(event + 2) == 0)
			handle_device(event, (size_t)length,
				      devices, &device_count);
	}
	if (mgmt_command(fd, MGMT_OP_STOP_DISCOVERY, 0,
			 &discovery_type, 1))
		perror("stop BLE discovery");
	printf("Found %zu unique BLE device%s\n", device_count,
	       device_count == 1 ? "" : "s");
	close(fd);
	return 0;
}
