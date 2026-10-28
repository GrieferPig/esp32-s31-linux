/* SPDX-License-Identifier: GPL-2.0-only */
/* Board Bluetooth application; the BTstack library remains unmodified. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "btstack.h"
#include "btstack_run_loop_posix.h"
#include "btstack_signal.h"
#include "btstack_tlv_posix.h"
#include "ble/le_device_db_tlv.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "hci_transport_linux.h"

static hci_transport_config_linux_t transport_config = {
	.type = HCI_TRANSPORT_CONFIG_LINUX,
};
static btstack_packet_callback_registration_t hci_registration, sm_registration;
static btstack_tlv_posix_t tlv_context;
static const btstack_tlv_t *tlv;
static bool reset_pairings, stopping, failed, coex_enabled;
static bool streaming, applied_streaming;
static unsigned coex_phase;
static char name[30] = "S31 Radio";
static char tlv_path[100];
static uint8_t scan_response[31], sbc_configuration[4];
static uint8_t sbc_capabilities[] = {0x21, 0x15, 2, 53};
static uint8_t a2dp_sdp[150], avrcp_controller_sdp[200], avrcp_target_sdp[150];
static const hci_cmd_t coex_command = {0xfc82, "111"};
static bool transition_streaming;

static void coex_pump(void)
{
	if (!coex_enabled || stopping) return;
	if (!coex_phase) {
		if (streaming == applied_streaming) return;
		transition_streaming = streaming;
		coex_phase = 1;
	}
	if (!hci_can_send_command_packet_now()) return;
	if (coex_phase == 1 && !hci_send_cmd(&coex_command, 2,
					 transition_streaming ? 1 : 0, 0x10))
		coex_phase = 2;
	else if (coex_phase == 3 && !hci_send_cmd(&coex_command, 2, 0, 0x20))
		coex_phase = 4;
}

static void shutdown_stack(void)
{
	stopping = true;
	hci_power_control(HCI_POWER_OFF);
}

static void hardware_error(uint8_t error)
{
	fprintf(stderr, "Bluetooth controller error 0x%02x\n", error);
	failed = true;
	stopping = true;
	btstack_run_loop_trigger_exit();
}

static bool open_pairing_store(void)
{
	bd_addr_t address;
	int length;
	bool resetting = reset_pairings;
	reset_pairings = false;
	gap_local_bd_addr(address);
	length = snprintf(tlv_path, sizeof(tlv_path), "/var/lib/btstack/btstack_%s.tlv",
			  bd_addr_to_str_with_delimiter(address, '-'));
	if (length < 0 || (size_t)length >= sizeof(tlv_path)) return false;
	if (resetting && unlink(tlv_path) && errno != ENOENT) {
		puts("S31_PAIRING_RESET=failed");
		return false;
	}
	tlv = btstack_tlv_posix_init_instance(&tlv_context, tlv_path);
	if (!tlv || !tlv_context.file) {
		if (resetting) puts("S31_PAIRING_RESET=failed");
		return false;
	}
	btstack_tlv_set_instance(tlv, &tlv_context);
	hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv, &tlv_context));
	le_device_db_tlv_configure(tlv, &tlv_context);
	if (resetting) puts("S31_PAIRING_RESET=ok");
	return true;
}

static void packet_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size)
{
	bd_addr_t address;
	(void)channel;
	if (type != HCI_EVENT_PACKET || size < 2) return;
	switch (hci_event_packet_get_type(packet)) {
	case BTSTACK_EVENT_STATE:
		if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
			if (!open_pairing_store()) {
				failed = true;
				shutdown_stack();
				break;
			}
			gap_set_page_scan_activity(0x0800, 0x0012);
			gap_inquiry_set_scan_activity(0x1000, 0x0012);
			printf("Bluetooth ready as %s\n", name);
		} else if (btstack_event_state_get_state(packet) == HCI_STATE_OFF) {
			if (tlv) {
				btstack_tlv_posix_deinit(&tlv_context);
				tlv = NULL;
			}
			if (stopping) btstack_run_loop_trigger_exit();
		}
		break;
	case HCI_EVENT_PIN_CODE_REQUEST:
		hci_event_pin_code_request_get_bd_addr(packet, address);
		gap_pin_code_response(address, "0000");
		break;
	case HCI_EVENT_USER_CONFIRMATION_REQUEST:
		hci_event_user_confirmation_request_get_bd_addr(packet, address);
		gap_ssp_confirmation_response(address);
		break;
	case HCI_EVENT_COMMAND_COMPLETE:
		if (size >= 6 && hci_event_command_complete_get_command_opcode(packet) == 0xfc82) {
			if (packet[5]) {
				failed = true;
				shutdown_stack();
			} else if (coex_phase == 2) coex_phase = 3;
			else if (coex_phase == 4) {
				applied_streaming = transition_streaming;
				coex_phase = 0;
			}
		}
		break;
	default:
		break;
	}
	coex_pump();
}

static void sm_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size)
{
	(void)channel;
	if (type != HCI_EVENT_PACKET || size < 4) return;
	switch (hci_event_packet_get_type(packet)) {
	case SM_EVENT_JUST_WORKS_REQUEST:
		sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
		break;
	case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
		sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
		break;
	default:
		break;
	}
}

static void profile_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size)
{
	(void)channel;
	if (type != HCI_EVENT_PACKET || size < 3) return;
	if (hci_event_packet_get_type(packet) == HCI_EVENT_A2DP_META) {
		switch (packet[2]) {
		case A2DP_SUBEVENT_STREAM_STARTED: streaming = true; break;
		case A2DP_SUBEVENT_STREAM_SUSPENDED:
		case A2DP_SUBEVENT_STREAM_RELEASED:
		case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED: streaming = false; break;
		default: break;
		}
	}
	coex_pump();
}

static void receive_media(uint8_t seid, uint8_t *packet, uint16_t size)
{
	/* The board service terminates transport; it has no PCM playback device. */
	(void)seid; (void)packet; (void)size;
	coex_pump();
}

static bool setup_profiles(void)
{
	static const uint8_t ready[] = "ready";
	static const uint8_t advertising[] = {2, 1, 6, 5, 3, 0, 0x18, 0x10, 0xff};
	bd_addr_t any = {0};
	size_t length = strlen(name);
	l2cap_init();
	sdp_init();
	sm_init();
	sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
	sm_set_authentication_requirements(SM_AUTHREQ_BONDING);
	sm_registration.callback = sm_handler;
	sm_add_event_handler(&sm_registration);
	att_db_util_init();
	att_db_util_add_service_uuid16(ORG_BLUETOOTH_SERVICE_GENERIC_ACCESS);
	att_db_util_add_characteristic_uuid16(ORG_BLUETOOTH_CHARACTERISTIC_GAP_DEVICE_NAME,
		ATT_PROPERTY_READ, ATT_SECURITY_NONE, ATT_SECURITY_NONE, (uint8_t *)name, length);
	att_db_util_add_service_uuid16(ORG_BLUETOOTH_SERVICE_GENERIC_ATTRIBUTE);
	att_db_util_add_service_uuid16(0xff10);
	att_db_util_add_characteristic_uuid16(0xff11, ATT_PROPERTY_READ,
		ATT_SECURITY_NONE, ATT_SECURITY_NONE, (uint8_t *)ready, sizeof(ready) - 1);
	att_server_init(att_db_util_get_address(), NULL, NULL);
	scan_response[0] = length + 1;
	scan_response[1] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
	memcpy(scan_response + 2, name, length);
	gap_advertisements_set_params(0x00a0, 0x00a0, 0, 0, any, 7, 0);
	gap_advertisements_set_data(sizeof(advertising), (uint8_t *)advertising);
	gap_scan_response_set_data(length + 2, scan_response);
	gap_advertisements_enable(1);
	a2dp_sink_init();
	avrcp_init();
	avrcp_controller_init();
	avrcp_target_init();
	a2dp_sink_register_packet_handler(profile_handler);
	a2dp_sink_register_media_handler(receive_media);
	if (!a2dp_sink_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC,
		sbc_capabilities, sizeof(sbc_capabilities), sbc_configuration, sizeof(sbc_configuration))) return false;
	avrcp_register_packet_handler(profile_handler);
	avrcp_controller_register_packet_handler(profile_handler);
	avrcp_target_register_packet_handler(profile_handler);
	a2dp_sink_create_sdp_record(a2dp_sdp, sdp_create_service_record_handle(),
		AVDTP_SINK_FEATURE_MASK_HEADPHONE, NULL, NULL);
	avrcp_controller_create_sdp_record(avrcp_controller_sdp, sdp_create_service_record_handle(),
		1 << AVRCP_CONTROLLER_SUPPORTED_FEATURE_CATEGORY_PLAYER_OR_RECORDER, NULL, NULL);
	avrcp_target_create_sdp_record(avrcp_target_sdp, sdp_create_service_record_handle(),
		1 << AVRCP_TARGET_SUPPORTED_FEATURE_CATEGORY_MONITOR_OR_AMPLIFIER, NULL, NULL);
	sdp_register_service(a2dp_sdp);
	sdp_register_service(avrcp_controller_sdp);
	sdp_register_service(avrcp_target_sdp);
	gap_set_local_name(name);
	gap_discoverable_control(1);
	gap_set_class_of_device(0x240400);
	gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
	gap_set_allow_role_switch(true);
	return true;
}

int main(int argc, char **argv)
{
	const char *setting = getenv("S31_BTSTACK_NAME");
	int option;
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (setting) {
		size_t length = strlen(setting);
		if (!length || length >= sizeof(name)) return 2;
		for (size_t i = 0; i < length; i++)
			if ((unsigned char)setting[i] < 32 || setting[i] == 127) return 2;
		memcpy(name, setting, length + 1);
	}
	while ((option = getopt(argc, argv, "ru:")) != -1) {
		if (option == 'r') reset_pairings = true;
		else if (option == 'u') {
			char *end;
			unsigned long index;
			errno = 0;
			index = strtoul(optarg, &end, 10);
			if (errno || end == optarg || *end || index > 65534) return 2;
			transport_config.device_id = index;
		} else return 2;
	}
	if (optind != argc) return 2;
	setting = getenv("S31_BTSTACK_COEX");
	coex_enabled = setting && strcmp(setting, "0");
	btstack_memory_init();
	btstack_run_loop_init(btstack_run_loop_posix_get_instance());
	hci_init(hci_transport_linux_instance(), &transport_config);
	hci_set_hardware_error_callback(hardware_error);
	hci_registration.callback = packet_handler;
	hci_add_event_handler(&hci_registration);
	btstack_signal_register_callback(SIGINT, shutdown_stack);
	btstack_signal_register_callback(SIGTERM, shutdown_stack);
	if (!setup_profiles() || hci_power_control(HCI_POWER_ON)) return 1;
	btstack_run_loop_execute();
	hci_close();
	return failed ? 1 : 0;
}
