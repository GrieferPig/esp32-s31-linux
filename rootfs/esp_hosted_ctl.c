// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal Linux control binary for the ESP-Hosted-FG RPC protocol.
 *
 * The upstream Linux host_control utility targets the older
 * esp_hosted_config.proto ABI.  S31 uses the current esp_hosted_rpc.proto
 * generated sources, so this utility keeps the familiar command names while
 * speaking the exact protocol built into the FreeRTOS co-processor.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "esp_hosted_rpc.pb-c.h"
#include "s31_hosted_sram.h"

#define SERIAL_DEVICE		"/dev/esps0"
#define RPC_ENDPOINT		"RPCRsp"
#define RPC_ENDPOINT_LEN	6
#define RPC_TIMEOUT_MS		8000
#define CONNECT_TIMEOUT_MS	20000
#define CONNECT_ATTEMPTS	3
#define MAX_RPC_MESSAGE		65535

#define WIFI_MODE_STA		1
#define WIFI_IF_STA		0

#define S31_HOSTED_IOC_WIFI_SLOT_SET \
	_IOW('S', 0x33, struct s31_hosted_wifi_slot_request)
#define S31_HOSTED_IOC_WIFI_SLOT_GET \
	_IOWR('S', 0x34, struct s31_hosted_wifi_slot_request)
#define S31_HOSTED_IOC_WIFI_STATE_SET \
	_IOW('S', 0x35, struct s31_hosted_wifi_state_request)
#define S31_HOSTED_IOC_WIFI_STATE_GET \
	_IOR('S', 0x36, struct s31_hosted_wifi_state_request)

enum station_result {
	STATION_PENDING,
	STATION_CONNECTED,
	STATION_DISCONNECTED,
};

static int serial_fd = -1;
static uint32_t next_uid = 1;
static enum station_result station_result;

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s sta_connect <ssid> <password> [--dhcp]\n"
		"  %s sta_disconnect\n"
		"  %s get_wifi_mode\n"
		"  %s set_wifi_mode <0..3>\n"
		"  %s wifi_stop\n"
		"  %s wifi_slot_set <0..2> <ssid> <password>\n"
		"  %s wifi_slot_clear <0..2>\n"
		"  %s wifi_slot_get <0..2>\n"
		"  %s wifi_state_get\n"
		"  %s wifi_state_set <enabled> <auto_connect> <interval_sec> <active_slot>\n"
		"  %s get_fw_version\n",
		program, program, program, program, program, program, program,
		program, program, program, program);
}

static int remaining_ms(const struct timespec *deadline)
{
	struct timespec now;
	int64_t value;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	value = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000 +
		(deadline->tv_nsec - now.tv_nsec) / 1000000;
	if (value <= 0)
		return 0;
	return value > INT32_MAX ? INT32_MAX : (int)value;
}

static struct timespec deadline_after(int timeout_ms)
{
	struct timespec deadline;

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += timeout_ms / 1000;
	deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	return deadline;
}

static int parse_tlv(const uint8_t *frame, size_t frame_len,
		     const uint8_t **protobuf, size_t *protobuf_len)
{
	size_t endpoint_len;
	size_t offset;
	size_t data_len;

	if (frame_len < 12 || frame[0] != 1)
		return -1;
	endpoint_len = frame[1] | ((size_t)frame[2] << 8);
	offset = 3;
	if (endpoint_len != RPC_ENDPOINT_LEN ||
	    offset + endpoint_len + 3 > frame_len)
		return -1;
	if (memcmp(frame + offset, "RPCRsp", endpoint_len) &&
	    memcmp(frame + offset, "RPCEvt", endpoint_len))
		return -1;
	offset += endpoint_len;
	if (frame[offset++] != 2)
		return -1;
	data_len = frame[offset] | ((size_t)frame[offset + 1] << 8);
	offset += 2;
	if (!data_len || offset + data_len != frame_len)
		return -1;
	*protobuf = frame + offset;
	*protobuf_len = data_len;
	return 0;
}

static void print_bssid(const ProtobufCBinaryData *bssid)
{
	if (!bssid || bssid->len != 6) {
		printf("<unknown>");
		return;
	}
	printf("%02x:%02x:%02x:%02x:%02x:%02x",
	       bssid->data[0], bssid->data[1], bssid->data[2],
	       bssid->data[3], bssid->data[4], bssid->data[5]);
}

static void handle_event(const Rpc *rpc)
{
	if (rpc->msg_id == RPC_ID__Event_StaConnected &&
	    rpc->event_sta_connected &&
	    rpc->event_sta_connected->sta_connected) {
		WifiEventStaConnected *event =
			rpc->event_sta_connected->sta_connected;
		size_t ssid_len = event->ssid_len;

		if (ssid_len > event->ssid.len)
			ssid_len = event->ssid.len;
		printf("Station connected: SSID=%.*s BSSID=",
		       (int)ssid_len, (const char *)event->ssid.data);
		print_bssid(&event->bssid);
		printf(" channel=%u auth=%d\n",
		       event->channel, event->authmode);
		station_result = STATION_CONNECTED;
	} else if (rpc->msg_id == RPC_ID__Event_StaDisconnected &&
		   rpc->event_sta_disconnected &&
		   rpc->event_sta_disconnected->sta_disconnected) {
		WifiEventStaDisconnected *event =
			rpc->event_sta_disconnected->sta_disconnected;

		printf("Station disconnected: reason=%u rssi=%d\n",
		       event->reason, event->rssi);
		station_result = STATION_DISCONNECTED;
	}
}

static Rpc *read_rpc(int timeout_ms)
{
	uint8_t *frame;
	const uint8_t *protobuf;
	size_t protobuf_len;
	struct pollfd poll_fd = {
		.fd = serial_fd,
		.events = POLLIN,
	};
	ssize_t length;
	int ret;
	Rpc *rpc;

	do {
		ret = poll(&poll_fd, 1, timeout_ms);
	} while (ret < 0 && errno == EINTR);
	if (ret <= 0) {
		if (!ret)
			errno = ETIMEDOUT;
		return NULL;
	}

	frame = malloc(MAX_RPC_MESSAGE);
	if (!frame)
		return NULL;
	do {
		length = read(serial_fd, frame, MAX_RPC_MESSAGE);
	} while (length < 0 && errno == EINTR);
	if (length <= 0 ||
	    parse_tlv(frame, (size_t)length, &protobuf, &protobuf_len)) {
		if (length >= 0)
			errno = EPROTO;
		free(frame);
		return NULL;
	}

	rpc = rpc__unpack(NULL, protobuf_len, protobuf);
	free(frame);
	if (!rpc)
		errno = EPROTO;
	return rpc;
}

static int response_status(const Rpc *rpc)
{
	switch (rpc->msg_id) {
	case RPC_ID__Resp_SetWifiMode:
		return rpc->resp_set_wifi_mode ?
			rpc->resp_set_wifi_mode->resp : -EPROTO;
	case RPC_ID__Resp_GetWifiMode:
		return rpc->resp_get_wifi_mode ?
			rpc->resp_get_wifi_mode->resp : -EPROTO;
	case RPC_ID__Resp_WifiInit:
		return rpc->resp_wifi_init ?
			rpc->resp_wifi_init->resp : -EPROTO;
	case RPC_ID__Resp_WifiStart:
		return rpc->resp_wifi_start ?
			rpc->resp_wifi_start->resp : -EPROTO;
	case RPC_ID__Resp_WifiStop:
		return rpc->resp_wifi_stop ?
			rpc->resp_wifi_stop->resp : -EPROTO;
	case RPC_ID__Resp_WifiConnect:
		return rpc->resp_wifi_connect ?
			rpc->resp_wifi_connect->resp : -EPROTO;
	case RPC_ID__Resp_WifiDisconnect:
		return rpc->resp_wifi_disconnect ?
			rpc->resp_wifi_disconnect->resp : -EPROTO;
	case RPC_ID__Resp_WifiSetConfig:
		return rpc->resp_wifi_set_config ?
			rpc->resp_wifi_set_config->resp : -EPROTO;
	case RPC_ID__Resp_GetCoprocessorFwVersion:
		return rpc->resp_get_coprocessor_fwversion ?
			rpc->resp_get_coprocessor_fwversion->resp : -EPROTO;
	default:
		return -EPROTO;
	}
}

static Rpc *exchange(Rpc *request, RpcId response_id)
{
	uint8_t *protobuf = NULL;
	uint8_t *frame = NULL;
	size_t protobuf_len;
	size_t frame_len;
	struct timespec deadline;
	Rpc *rpc = NULL;
	ssize_t written;

	request->msg_type = RPC_TYPE__Req;
	request->uid = next_uid++;
	request->msg_id = response_id - RPC_ID__Resp_Base + RPC_ID__Req_Base;
	protobuf_len = rpc__get_packed_size(request);
	if (!protobuf_len || protobuf_len > UINT16_MAX) {
		errno = EMSGSIZE;
		return NULL;
	}
	protobuf = malloc(protobuf_len);
	frame_len = 12 + protobuf_len;
	frame = malloc(frame_len);
	if (!protobuf || !frame)
		goto out;

	rpc__pack(request, protobuf);
	frame[0] = 1;
	frame[1] = RPC_ENDPOINT_LEN;
	frame[2] = 0;
	memcpy(frame + 3, RPC_ENDPOINT, RPC_ENDPOINT_LEN);
	frame[9] = 2;
	frame[10] = protobuf_len & 0xff;
	frame[11] = protobuf_len >> 8;
	memcpy(frame + 12, protobuf, protobuf_len);

	do {
		written = write(serial_fd, frame, frame_len);
	} while (written < 0 && errno == EINTR);
	if (written != (ssize_t)frame_len) {
		if (written >= 0)
			errno = EIO;
		goto out;
	}

	deadline = deadline_after(RPC_TIMEOUT_MS);
	for (;;) {
		int timeout = remaining_ms(&deadline);

		if (!timeout) {
			errno = ETIMEDOUT;
			break;
		}
		rpc = read_rpc(timeout);
		if (!rpc)
			break;
		if (rpc->msg_type == RPC_TYPE__Event) {
			handle_event(rpc);
			rpc__free_unpacked(rpc, NULL);
			rpc = NULL;
			continue;
		}
		if (rpc->msg_type == RPC_TYPE__Resp &&
		    rpc->msg_id == response_id &&
		    rpc->uid == request->uid)
			break;
		rpc__free_unpacked(rpc, NULL);
		rpc = NULL;
	}

out:
	free(frame);
	free(protobuf);
	return rpc;
}

static int run_simple_request(Rpc *request, RpcId response_id,
			      const char *name, int tolerate_error)
{
	Rpc *response = exchange(request, response_id);
	int status;

	if (!response) {
		fprintf(stderr, "%s: RPC failed: %s\n", name, strerror(errno));
		return -1;
	}
	status = response_status(response);
	rpc__free_unpacked(response, NULL);
	if (status) {
		fprintf(stderr, "%s: ESP error 0x%x\n", name, status);
		return tolerate_error ? 0 : -1;
	}
	return 0;
}

static int wifi_init(void)
{
	WifiInitConfig config = WIFI_INIT_CONFIG__INIT;
	RpcReqWifiInit payload = RPC__REQ__WIFI_INIT__INIT;
	Rpc request = RPC__INIT;

	/* WIFI_INIT_CONFIG_DEFAULT() resolved against bootloader/sdkconfig. */
	config.static_rx_buf_num = 10;
	config.dynamic_rx_buf_num = 32;
	config.tx_buf_type = 1;
	config.dynamic_tx_buf_num = 32;
	config.rx_mgmt_buf_num = 5;
	config.ampdu_rx_enable = 1;
	config.ampdu_tx_enable = 1;
	config.nvs_enable = 1;
	config.rx_ba_win = 6;
	config.beacon_max_len = 752;
	config.mgmt_sbuf_num = 32;
	config.feature_caps = 1441;
	config.sta_disconnected_pm = 1;
	config.espnow_max_encrypt_num = 7;
	config.tx_hetb_queue_num = 3;
	config.magic = 0x1f2f3f4f;
	payload.cfg = &config;
	request.payload_case = RPC__PAYLOAD_REQ_WIFI_INIT;
	request.req_wifi_init = &payload;
	return run_simple_request(&request, RPC_ID__Resp_WifiInit,
				  "wifi_init", 0);
}

static int set_wifi_mode(int mode)
{
	RpcReqSetMode payload = RPC__REQ__SET_MODE__INIT;
	Rpc request = RPC__INIT;

	payload.mode = mode;
	request.payload_case = RPC__PAYLOAD_REQ_SET_WIFI_MODE;
	request.req_set_wifi_mode = &payload;
	return run_simple_request(&request, RPC_ID__Resp_SetWifiMode,
				  "set_wifi_mode", 0);
}

static int set_station_config(const char *ssid, const char *password)
{
	WifiStaConfig station = WIFI_STA_CONFIG__INIT;
	WifiConfig config = WIFI_CONFIG__INIT;
	RpcReqWifiSetConfig payload = RPC__REQ__WIFI_SET_CONFIG__INIT;
	Rpc request = RPC__INIT;

	if (strlen(ssid) > 32 || strlen(password) > 64) {
		fprintf(stderr, "SSID or password is too long\n");
		return -1;
	}
	station.ssid.data = (uint8_t *)ssid;
	station.ssid.len = strlen(ssid);
	station.password.data = (uint8_t *)password;
	station.password.len = strlen(password);
	config.u_case = WIFI_CONFIG__U_STA;
	config.sta = &station;
	payload.iface = WIFI_IF_STA;
	payload.cfg = &config;
	request.payload_case = RPC__PAYLOAD_REQ_WIFI_SET_CONFIG;
	request.req_wifi_set_config = &payload;
	return run_simple_request(&request, RPC_ID__Resp_WifiSetConfig,
				  "wifi_set_config", 0);
}

static int wifi_start(void)
{
	RpcReqWifiStart payload = RPC__REQ__WIFI_START__INIT;
	Rpc request = RPC__INIT;

	request.payload_case = RPC__PAYLOAD_REQ_WIFI_START;
	request.req_wifi_start = &payload;
	/* A second invocation may find Wi-Fi already started. */
	return run_simple_request(&request, RPC_ID__Resp_WifiStart,
				  "wifi_start", 1);
}

static int wait_for_station(void)
{
	struct timespec deadline = deadline_after(CONNECT_TIMEOUT_MS);

	while (station_result == STATION_PENDING) {
		int timeout = remaining_ms(&deadline);
		Rpc *rpc;

		if (!timeout)
			break;
		rpc = read_rpc(timeout);
		if (!rpc)
			break;
		if (rpc->msg_type == RPC_TYPE__Event)
			handle_event(rpc);
		rpc__free_unpacked(rpc, NULL);
	}
	if (station_result == STATION_CONNECTED)
		return 0;
	if (station_result == STATION_DISCONNECTED)
		fprintf(stderr, "Station connection failed\n");
	else
		fprintf(stderr, "Timed out waiting for station connection\n");
	return -1;
}

static int station_connect(const char *ssid, const char *password, int dhcp)
{
	RpcReqWifiConnect payload = RPC__REQ__WIFI_CONNECT__INIT;
	Rpc request = RPC__INIT;
	int attempt;

	if (wifi_init() || set_wifi_mode(WIFI_MODE_STA) ||
	    set_station_config(ssid, password) || wifi_start())
		return -1;

	request.payload_case = RPC__PAYLOAD_REQ_WIFI_CONNECT;
	request.req_wifi_connect = &payload;
	for (attempt = 1; attempt <= CONNECT_ATTEMPTS; attempt++) {
		station_result = STATION_PENDING;
		if (!run_simple_request(&request, RPC_ID__Resp_WifiConnect,
					"wifi_connect", 0) &&
		    !wait_for_station())
			break;
		if (attempt == CONNECT_ATTEMPTS)
			return -1;
		fprintf(stderr, "Retrying station connection (%d/%d)\n",
			attempt + 1, CONNECT_ATTEMPTS);
		sleep(1);
	}
	if (dhcp) {
		printf("Requesting DHCP lease on ethsta0\n");
		if (system("/sbin/udhcpc -i ethsta0 -n -q -t 5"))
			return -1;
	}
	return 0;
}

static int station_disconnect(void)
{
	RpcReqWifiDisconnect payload = RPC__REQ__WIFI_DISCONNECT__INIT;
	Rpc request = RPC__INIT;

	request.payload_case = RPC__PAYLOAD_REQ_WIFI_DISCONNECT;
	request.req_wifi_disconnect = &payload;
	return run_simple_request(&request, RPC_ID__Resp_WifiDisconnect,
				  "wifi_disconnect", 0);
}

static int wifi_stop(void)
{
	RpcReqWifiStop payload = RPC__REQ__WIFI_STOP__INIT;
	Rpc request = RPC__INIT;

	request.payload_case = RPC__PAYLOAD_REQ_WIFI_STOP;
	request.req_wifi_stop = &payload;
	return run_simple_request(&request, RPC_ID__Resp_WifiStop,
				  "wifi_stop", 0);
}

static int get_wifi_mode(void)
{
	RpcReqGetMode payload = RPC__REQ__GET_MODE__INIT;
	Rpc request = RPC__INIT;
	Rpc *response;
	int status;

	request.payload_case = RPC__PAYLOAD_REQ_GET_WIFI_MODE;
	request.req_get_wifi_mode = &payload;
	response = exchange(&request, RPC_ID__Resp_GetWifiMode);
	if (!response) {
		fprintf(stderr, "get_wifi_mode: %s\n", strerror(errno));
		return -1;
	}
	status = response_status(response);
	if (!status)
		printf("Wi-Fi mode: %d\n", response->resp_get_wifi_mode->mode);
	else
		fprintf(stderr, "get_wifi_mode: ESP error 0x%x\n", status);
	rpc__free_unpacked(response, NULL);
	return status ? -1 : 0;
}

static int get_fw_version(void)
{
	RpcReqGetCoprocessorFwVersion payload =
		RPC__REQ__GET_COPROCESSOR_FW_VERSION__INIT;
	Rpc request = RPC__INIT;
	Rpc *response;
	RpcRespGetCoprocessorFwVersion *version;
	int status;

	request.payload_case = RPC__PAYLOAD_REQ_GET_COPROCESSOR_FWVERSION;
	request.req_get_coprocessor_fwversion = &payload;
	response = exchange(&request, RPC_ID__Resp_GetCoprocessorFwVersion);
	if (!response) {
		fprintf(stderr, "get_fw_version: %s\n", strerror(errno));
		return -1;
	}
	status = response_status(response);
	version = response->resp_get_coprocessor_fwversion;
	if (!status && version) {
		printf("ESP-Hosted FW: %u.%u.%u revision=%d prerelease=%d "
		       "build=%d chip=%u target=%.*s\n",
		       version->major1, version->minor1, version->patch1,
		       version->revision, version->prerelease, version->build,
		       version->chip_id, (int)version->idf_target.len,
		       (const char *)version->idf_target.data);
	} else {
		fprintf(stderr, "get_fw_version: ESP error 0x%x\n", status);
	}
	rpc__free_unpacked(response, NULL);
	return status ? -1 : 0;
}

static int parse_index(const char *text, unsigned int max, unsigned int *value)
{
	char *end;
	unsigned long parsed = strtoul(text, &end, 0);
	if (!*text || *end || parsed >= max)
		return -1;
	*value = parsed;
	return 0;
}

static int wifi_slot_set(unsigned int slot, const char *ssid,
				 const char *password, int clear)
{
	struct s31_hosted_wifi_slot_request request = { 0 };
	if (!clear && (strlen(ssid) > S31_HOSTED_WIFI_SSID_MAX ||
			      strlen(password) > S31_HOSTED_WIFI_PASSWORD_MAX ||
			      !strlen(ssid))) {
		fprintf(stderr, "SSID/password length is invalid\n");
		return -1;
	}
	request.slot = slot;
	if (!clear) {
		request.config.valid = 1;
		request.config.ssid_len = strlen(ssid);
		request.config.password_len = strlen(password);
		memcpy(request.config.ssid, ssid, request.config.ssid_len);
		memcpy(request.config.password, password,
		       request.config.password_len);
	}
	if (ioctl(serial_fd, S31_HOSTED_IOC_WIFI_SLOT_SET, &request) < 0) {
		fprintf(stderr, "wifi_slot_%s: %s\n", clear ? "clear" : "set",
			strerror(errno));
		return -1;
	}
	printf("Wi-Fi slot %u %s\n", slot, clear ? "cleared" : "saved");
	return 0;
}

static int wifi_slot_get(unsigned int slot)
{
	struct s31_hosted_wifi_slot_request request = { .slot = slot };
	if (ioctl(serial_fd, S31_HOSTED_IOC_WIFI_SLOT_GET, &request) < 0) {
		fprintf(stderr, "wifi_slot_get: %s\n", strerror(errno));
		return -1;
	}
	if (!request.config.valid) {
		printf("Wi-Fi slot %u: empty\n", slot);
		return 0;
	}
	printf("Wi-Fi slot %u: SSID=%.*s password=%.*s priority=%u\n",
		slot, request.config.ssid_len, request.config.ssid,
		request.config.password_len, request.config.password,
		request.config.priority);
	return 0;
}

static int wifi_state_get(void)
{
	struct s31_hosted_wifi_state_request request = { 0 };
	if (ioctl(serial_fd, S31_HOSTED_IOC_WIFI_STATE_GET, &request) < 0) {
		fprintf(stderr, "wifi_state_get: %s\n", strerror(errno));
		return -1;
	}
	printf("Wi-Fi state: enabled=%u auto_connect=%u interval_sec=%u "
	       "active_slot=%u connected_slot=%s\n",
	       request.state.enabled, request.state.auto_connect,
	       request.state.scan_interval_sec, request.state.active_slot,
	       request.state.connected_slot == 0xff ? "none" : "set");
	return 0;
}

static int wifi_state_set(unsigned int enabled, unsigned int auto_connect,
				  unsigned int interval, unsigned int active_slot)
{
	struct s31_hosted_wifi_state_request request = { 0 };
	if (interval == 0 || interval > UINT16_MAX) {
		fprintf(stderr, "wifi_state_set: invalid interval\n");
		return -1;
	}
	request.state.enabled = !!enabled;
	request.state.auto_connect = !!auto_connect;
	request.state.scan_interval_sec = interval;
	request.state.active_slot = active_slot;
	request.state.connected_slot = 0xff;
	if (ioctl(serial_fd, S31_HOSTED_IOC_WIFI_STATE_SET, &request) < 0) {
		fprintf(stderr, "wifi_state_set: %s\n", strerror(errno));
		return -1;
	}
	return wifi_state_get();
}

int main(int argc, char **argv)
{
	int ret = -1;

	if (argc < 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	serial_fd = open(SERIAL_DEVICE, O_RDWR | O_CLOEXEC);
	if (serial_fd < 0) {
		fprintf(stderr, "open %s: %s\n",
			SERIAL_DEVICE, strerror(errno));
		return EXIT_FAILURE;
	}

	if (!strcmp(argv[1], "sta_connect") && (argc == 4 || argc == 5)) {
		int dhcp = argc == 5 && !strcmp(argv[4], "--dhcp");

		if (argc == 5 && !dhcp)
			usage(argv[0]);
		else
			ret = station_connect(argv[2], argv[3], dhcp);
	} else if (!strcmp(argv[1], "sta_disconnect") && argc == 2) {
		ret = station_disconnect();
	} else if (!strcmp(argv[1], "get_wifi_mode") && argc == 2) {
		ret = get_wifi_mode();
	} else if (!strcmp(argv[1], "set_wifi_mode") && argc == 3) {
		char *end;
		long mode = strtol(argv[2], &end, 0);

		if (*end || mode < 0 || mode > 3)
			usage(argv[0]);
		else
			ret = set_wifi_mode((int)mode);
	} else if (!strcmp(argv[1], "wifi_stop") && argc == 2) {
		ret = wifi_stop();
	} else if (!strcmp(argv[1], "wifi_slot_set") && argc == 5) {
		unsigned int slot;
		if (parse_index(argv[2], S31_HOSTED_WIFI_SLOT_COUNT, &slot))
			usage(argv[0]);
		else
			ret = wifi_slot_set(slot, argv[3], argv[4], 0);
	} else if (!strcmp(argv[1], "wifi_slot_clear") && argc == 3) {
		unsigned int slot;
		if (parse_index(argv[2], S31_HOSTED_WIFI_SLOT_COUNT, &slot))
			usage(argv[0]);
		else
			ret = wifi_slot_set(slot, NULL, NULL, 1);
	} else if (!strcmp(argv[1], "wifi_slot_get") && argc == 3) {
		unsigned int slot;
		if (parse_index(argv[2], S31_HOSTED_WIFI_SLOT_COUNT, &slot))
			usage(argv[0]);
		else
			ret = wifi_slot_get(slot);
	} else if (!strcmp(argv[1], "wifi_state_get") && argc == 2) {
		ret = wifi_state_get();
	} else if (!strcmp(argv[1], "wifi_state_set") && argc == 6) {
		char *end;
		unsigned long enabled = strtoul(argv[2], &end, 0);
		int valid = !*argv[2] || *end;
		unsigned long auto_connect = strtoul(argv[3], &end, 0);
		valid = valid || !*argv[3] || *end;
		unsigned long interval = strtoul(argv[4], &end, 0);
		valid = valid || !*argv[4] || *end;
		unsigned int active_slot;
		if (valid || enabled > 1 || auto_connect > 1 ||
		    parse_index(argv[5], S31_HOSTED_WIFI_SLOT_COUNT, &active_slot))
			usage(argv[0]);
		else
			ret = wifi_state_set(enabled, auto_connect, interval,
					    active_slot);
	} else if (!strcmp(argv[1], "get_fw_version") && argc == 2) {
		ret = get_fw_version();
	} else {
		usage(argv[0]);
	}

	close(serial_fd);
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
