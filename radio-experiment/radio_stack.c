/* SPDX-License-Identifier: BSD-2-Clause */
#include <stdint.h>
#include <string.h>
#include "esp_bt.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_private/wifi_os_adapter.h"
#include "private/esp_coexist_adapter.h"
#include "private/esp_coexist_internal.h"

extern int esp_rom_printf(const char *fmt, ...);
extern void s31_rtos_use_internal_stacks(void);
#ifdef S31_LINUX_SMODE
extern void s31_radio_heap_report(const char *stage);
extern void s31_radio_report_wifi_init(int result);
extern void s31_radio_report_bt_init(int result);
extern void s31_radio_report_bt_enable(int result);
extern void s31_radio_vhci_send_available(void);
extern int s31_radio_vhci_receive(uint8_t *frame, uint16_t length);
struct s31_wifi_ap {
	uint8_t bssid[6];
	uint8_t ssid[32];
	uint8_t ssid_length;
	uint8_t channel;
	int8_t signal;
	uint8_t authmode;
};
extern void s31_radio_wifi_scan_complete(const struct s31_wifi_ap *aps,
					 uint16_t count, int status);
extern void s31_radio_wifi_intr_configure(uint32_t source,
					 uint32_t logical_intr, uint32_t priority);
extern void s31_radio_wifi_intr_set_isr(uint32_t logical_intr,
					void (*handler)(void *), void *arg);
extern void s31_radio_wifi_intr_mask(uint32_t mask, bool enable);
#endif

#ifdef S31_LINUX_SMODE
static void s31_vhci_send_available(void)
{
	s31_radio_vhci_send_available();
}

static int s31_vhci_receive(uint8_t *data, uint16_t len)
{
	return s31_radio_vhci_receive(data, len);
}

static const esp_vhci_host_callback_t s31_vhci_callbacks = {
	.notify_host_send_available = s31_vhci_send_available,
	.notify_host_recv = s31_vhci_receive,
};

static int s31_wifi_started;

static void s31_wifi_set_intr(int32_t cpu_no, uint32_t source,
			      uint32_t logical_intr, int32_t priority)
{
	(void)cpu_no;
	s31_radio_wifi_intr_configure(source, logical_intr, priority);
}

static void s31_wifi_set_isr(int32_t logical_intr, void *handler, void *arg)
{
	s31_radio_wifi_intr_set_isr(logical_intr,
				    (void (*)(void *))handler, arg);
}

static void s31_wifi_ints_on(uint32_t mask)
{
	s31_radio_wifi_intr_mask(mask, true);
}

static void s31_wifi_ints_off(uint32_t mask)
{
	s31_radio_wifi_intr_mask(mask, false);
}

static void s31_wifi_event(void *arg, esp_event_base_t base, int32_t id,
			   void *event_data)
{
	wifi_ap_record_t *records = NULL;
	struct s31_wifi_ap *aps = NULL;
	uint16_t count = 32;
	int rc;
	int i;

	(void)arg;
	(void)base;
	(void)event_data;
	if (id != WIFI_EVENT_SCAN_DONE)
		return;
	records = heap_caps_malloc(sizeof(*records) * count, 0);
	aps = heap_caps_malloc(sizeof(*aps) * count, 0);
	if (!records || !aps) {
		heap_caps_free(records);
		heap_caps_free(aps);
		s31_radio_wifi_scan_complete(NULL, 0, -1);
		return;
	}
	rc = esp_wifi_scan_get_ap_records(&count, records);
	if (rc) {
		heap_caps_free(records);
		heap_caps_free(aps);
		s31_radio_wifi_scan_complete(NULL, 0, rc);
		return;
	}
	for (i = 0; i < count; i++) {
		size_t length = strnlen((const char *)records[i].ssid, 32);

		memcpy(aps[i].bssid, records[i].bssid, sizeof(aps[i].bssid));
		memcpy(aps[i].ssid, records[i].ssid, length);
		if (length < sizeof(aps[i].ssid))
			memset(aps[i].ssid + length, 0, sizeof(aps[i].ssid) - length);
		aps[i].ssid_length = length;
		aps[i].channel = records[i].primary;
		aps[i].signal = records[i].rssi;
		aps[i].authmode = records[i].authmode;
	}
	heap_caps_free(records);
	s31_radio_wifi_scan_complete(aps, count, 0);
	heap_caps_free(aps);
}

int s31_radio_vhci_try_send(uint8_t *frame, uint16_t length)
{
	if (!esp_vhci_host_check_send_available())
		return -1;
	esp_vhci_host_send_packet(frame, length);
	return 0;
}

void s31_radio_wifi_scan_task(void *arg)
{
	int rc = 0;

	(void)arg;
	if (!s31_wifi_started) {
		rc = esp_wifi_set_mode(WIFI_MODE_STA);
		if (!rc)
			rc = esp_wifi_start();
		if (!rc)
			s31_wifi_started = 1;
	}
	if (!rc)
		rc = esp_wifi_scan_start(NULL, false);
	if (rc)
		s31_radio_wifi_scan_complete(NULL, 0, rc);
}
#endif

void s31_radio_stack_task(void *arg)
{
	wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
	int rc;

	(void)arg;
	/* Keep the closed Wi-Fi library away from the M-mode CLIC window. */
	g_wifi_osi_funcs._set_intr = s31_wifi_set_intr;
	g_wifi_osi_funcs._set_isr = s31_wifi_set_isr;
	g_wifi_osi_funcs._ints_on = s31_wifi_ints_on;
	g_wifi_osi_funcs._ints_off = s31_wifi_ints_off;
	/* Linux owns flash/MTD; do not let the IDF blob open its NVS backend. */
	wifi_cfg.nvs_enable = 0;
	/* Replace the loader/FreeRTOS callbacks retained by the COEX ROM. */
	rc = esp_coex_adapter_register(&g_coex_adapter_funcs);
	if (rc != 0) {
		esp_rom_printf("[S31] esp_coex_adapter_register rc=%d\n", rc);
	#ifdef S31_LINUX_SMODE
		s31_radio_report_wifi_init(rc);
	#endif
		return;
	}
	/*
	 * ESP-IDF normally performs these two calls together from its
	 * SECONDARY system-init hook.  Registering the adapter alone leaves
	 * the coexistence lock/environment uninitialised; esp_wifi_init() then
	 * faults as soon as it registers its scheduler callbacks.
	 */
	rc = coex_pre_init();
	esp_rom_printf("[S31] coex_pre_init rc=%d\n", rc);
	if (rc != 0) {
	#ifdef S31_LINUX_SMODE
		s31_radio_report_wifi_init(rc);
	#endif
		return;
	}
	rc = esp_wifi_init(&wifi_cfg);
	esp_rom_printf("[S31] esp_wifi_init rc=%d\n", rc);
	#ifdef S31_LINUX_SMODE
	if (rc == 0)
		rc = esp_event_loop_create_default();
	if (rc == 0)
		rc = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
						s31_wifi_event, NULL);
	#endif
	#ifdef S31_LINUX_SMODE
	s31_radio_report_wifi_init(rc);
	#endif
	if (rc == 0) {
	#ifndef S31_LINUX_SMODE
		rc = esp_wifi_set_mode(WIFI_MODE_STA);
		esp_rom_printf("[S31] esp_wifi_set_mode rc=%d\n", rc);
		if (rc == 0) {
			rc = esp_wifi_start();
			esp_rom_printf("[S31] esp_wifi_start rc=%d\n", rc);
		}
	#endif
	}

	if (rc != 0)
		return;

	rc = esp_bt_controller_init(&bt_cfg);
	esp_rom_printf("[S31] esp_bt_controller_init rc=%d\n", rc);
	#ifdef S31_LINUX_SMODE
	s31_radio_report_bt_init(rc);
	#endif
	if (rc == 0) {
	#ifndef S31_LINUX_SMODE
		rc = esp_bt_controller_enable(BTDM_CONTROLLER_MODE_EFF);
		esp_rom_printf("[S31] esp_bt_controller_enable rc=%d\n", rc);
	#endif
	}

}

/* Linux installs the deferred CLIC route only after the init task has
 * blocked/deleted and returned control to its worker.  Controller enable is
 * therefore a distinct second-stage task. */
void s31_radio_bt_enable_task(void *arg)
{
	int rc;

	(void)arg;
	s31_rtos_use_internal_stacks();
	rc = esp_bt_controller_enable(BTDM_CONTROLLER_MODE_EFF);
	esp_rom_printf("[S31] esp_bt_controller_enable rc=%d\n", rc);
	#ifdef S31_LINUX_SMODE
	if (rc == 0) {
		rc = esp_vhci_host_register_callback(&s31_vhci_callbacks);
		esp_rom_printf("[S31] esp_vhci_host_register_callback rc=%d\n", rc);
	}
	s31_radio_report_bt_enable(rc);
	s31_radio_heap_report("after-bt-enable");
	#endif
}
