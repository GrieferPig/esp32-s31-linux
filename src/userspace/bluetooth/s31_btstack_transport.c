/* SPDX-License-Identifier: GPL-2.0-only */
/* BTstack transport using the standard Linux HCI user-channel socket ABI. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "btstack.h"
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include "btstack_config.h"
#include "btstack_defines.h"
#include "btstack_run_loop.h"
#include "hci_transport.h"
#include "hci_transport_linux.h"

static int socket_fd = -1;
static unsigned device_index;
static btstack_data_source_t source;
static btstack_context_callback_registration_t sent_callback;
static void (*receive_packet)(uint8_t, uint8_t *, uint16_t);
static uint8_t incoming[HCI_INCOMING_PRE_BUFFER_SIZE + HCI_INCOMING_PACKET_BUFFER_SIZE + 1];
static uint8_t *pending;
static size_t pending_size;
static bool busy;

static void packet_sent(void *unused)
{
	uint8_t event[] = {HCI_EVENT_TRANSPORT_PACKET_SENT, 0};
	(void)unused;
	busy = false;
	receive_packet(HCI_EVENT_PACKET, event, sizeof(event));
}

static void fail_transport(void)
{
	uint8_t event[] = {HCI_EVENT_HARDWARE_ERROR, 1, 3};
	perror("Bluetooth HCI socket");
	btstack_run_loop_disable_data_source_callbacks(&source,
		DATA_SOURCE_CALLBACK_READ | DATA_SOURCE_CALLBACK_WRITE);
	receive_packet(HCI_EVENT_PACKET, event, sizeof(event));
}

static void flush_packet(void)
{
	ssize_t written;
	do written = write(socket_fd, pending, pending_size);
	while (written < 0 && errno == EINTR);
	if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		btstack_run_loop_enable_data_source_callbacks(&source, DATA_SOURCE_CALLBACK_WRITE);
		return;
	}
	if (written != (ssize_t)pending_size) {
		if (written >= 0) errno = EIO;
		fail_transport();
		return;
	}
	pending = NULL;
	btstack_run_loop_disable_data_source_callbacks(&source, DATA_SOURCE_CALLBACK_WRITE);
	sent_callback.callback = packet_sent;
	btstack_run_loop_execute_on_main_thread(&sent_callback);
}

static void process_socket(btstack_data_source_t *unused, btstack_data_source_callback_type_t type)
{
	(void)unused;
	if (type == DATA_SOURCE_CALLBACK_WRITE) {
		if (pending) flush_packet();
	} else if (type == DATA_SOURCE_CALLBACK_READ) {
		uint8_t *packet = incoming + HCI_INCOMING_PRE_BUFFER_SIZE;
		ssize_t received = read(socket_fd, packet, HCI_INCOMING_PACKET_BUFFER_SIZE + 1);
		if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return;
		if (received <= 1) {
			if (received >= 0) errno = EIO;
			fail_transport();
			return;
		}
		receive_packet(packet[0], packet + 1, received - 1);
	}
}

static void init_transport(const void *configuration)
{
	const hci_transport_config_linux_t *config = configuration;
	device_index = config->device_id;
}

static int open_transport(void)
{
	unsigned attempt;
	int ret;
	struct sockaddr_hci address = {
		.hci_family = AF_BLUETOOTH,
		.hci_channel = HCI_CHANNEL_USER,
	};
	address.hci_dev = device_index;
	socket_fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, BTPROTO_HCI);
	if (socket_fd < 0) return -1;
	/* Registration exposes sysfs before the kernel's initial HCI setup is
	 * complete. Wait only for that bounded startup interval; never take the
	 * controller away from an existing userspace host.
	 */
	for (attempt = 0; attempt < 100; attempt++) {
		ret = bind(socket_fd, (struct sockaddr *)&address, sizeof(address));
		if (!ret) break;
		if (errno != EBUSY) break;
		usleep(100000);
	}
	if (ret) {
		close(socket_fd);
		socket_fd = -1;
		return -1;
	}
	busy = false;
	pending = NULL;
	btstack_run_loop_set_data_source_fd(&source, socket_fd);
	btstack_run_loop_set_data_source_handler(&source, process_socket);
	btstack_run_loop_add_data_source(&source);
	btstack_run_loop_enable_data_source_callbacks(&source, DATA_SOURCE_CALLBACK_READ);
	return 0;
}

static int close_transport(void)
{
	if (socket_fd >= 0) {
		btstack_run_loop_remove_data_source(&source);
		close(socket_fd);
		socket_fd = -1;
	}
	pending = NULL;
	return 0;
}

static void register_handler(void (*handler)(uint8_t, uint8_t *, uint16_t))
{
	receive_packet = handler;
}

static int can_send(uint8_t type)
{
	(void)type;
	return socket_fd >= 0 && !busy;
}

static int send_packet(uint8_t type, uint8_t *packet, int size)
{
	if (!can_send(type) || size < 0 || size > HCI_OUTGOING_PACKET_BUFFER_SIZE) return -1;
	busy = true;
	pending = packet - 1;
	pending[0] = type;
	pending_size = size + 1;
	flush_packet();
	return 0;
}

const hci_transport_t *hci_transport_linux_instance(void)
{
	static const hci_transport_t transport = {
		.name = "Linux HCI user channel",
		.init = init_transport, .open = open_transport, .close = close_transport,
		.register_packet_handler = register_handler,
		.can_send_packet_now = can_send, .send_packet = send_packet,
	};
	return &transport;
}
