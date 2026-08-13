/* SPDX-License-Identifier: BSD-2-Clause */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "esp_bt.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_private/wifi_os_adapter.h"
#include "esp_private/wifi.h"
#include "private/esp_coexist_adapter.h"
#include "private/esp_coexist_internal.h"

extern int esp_rom_printf(const char *fmt, ...);
extern void s31_rtos_use_internal_stacks(void);
extern void modem_clock_module_enable(int module);
extern void modem_clock_module_disable(int module);
/* These ROM-owned pointers live in retained SRAM.  A software reset from an
 * ESP-IDF image can leave them pointing at that image's flash/data mapping;
 * the ROM registration functions intentionally keep an existing adapter. */
extern coex_adapter_funcs_t *g_coa_funcs_p;
extern wifi_osi_funcs_t *g_osi_funcs_p;
extern void *s_wifi_queue;
extern void *xphyQueue;
extern void *pp_task_hdl;
/* ESP-IDF invokes this from its SECONDARY system-init stage (priority 104),
 * before app_main() can initialize Wi-Fi.  The S-mode payload deliberately
 * does not run the generic IDF startup table, so preserve that ordering here.
 * WPA3/SAE uses the PSA key store for HMAC-SHA256 and otherwise fails while
 * deriving the password element. */
extern int32_t psa_crypto_init(void);

#ifdef S31_LINUX_SMODE
#define S31_PERIPH_WIFI_MODULE 5

void s31_radio_wifi_clock_enable(void)
{
	modem_clock_module_enable(S31_PERIPH_WIFI_MODULE);
}

void s31_radio_wifi_clock_disable(void)
{
	modem_clock_module_disable(S31_PERIPH_WIFI_MODULE);
}

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
struct s31_wifi_connect_params {
	uint8_t ssid[32];
	uint8_t ssid_length;
	uint8_t bssid[6];
	uint8_t channel;
	uint8_t psk[32];
	uint8_t password[64];
	uint8_t password_length;
	bool has_bssid;
	bool has_psk;
	bool has_password;
};
extern void s31_radio_wifi_scan_complete(const struct s31_wifi_ap *aps,
					 uint16_t count, int status);
extern void s31_radio_wifi_connected(const uint8_t *bssid, uint8_t channel,
				     int status);
extern void s31_radio_wifi_disconnected(uint16_t reason);
extern int s31_radio_wifi_receive(uint8_t *frame, uint16_t length);
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

#ifndef S31_WIFI_ONLY
static const esp_vhci_host_callback_t s31_vhci_callbacks = {
	.notify_host_send_available = s31_vhci_send_available,
	.notify_host_recv = s31_vhci_receive,
};
#endif

enum s31_wifi_pending_operation {
	S31_WIFI_PENDING_NONE,
	S31_WIFI_PENDING_SCAN,
	S31_WIFI_PENDING_CONNECT,
};

static int s31_wifi_prepared;
static int s31_wifi_start_requested;
static int s31_wifi_start_complete;
static int s31_wifi_rx_registered;
static enum s31_wifi_pending_operation s31_wifi_pending;
static uint32_t s31_wifi_rx_count;
static uint32_t s31_wifi_tx_count;
static uint32_t s31_wifi_tx_done_count;
static void *s31_wifi_pp_queue;
static void *s31_wifi_pp_handle;
static void *s31_wifi_pp_task;
static uint32_t s31_wifi_pp_restore_count;

static void s31_wifi_report_pp_queue(const char *stage)
{
	uint32_t *wrapper = s_wifi_queue;

	if (wrapper)
		esp_rom_printf("[S31] pp queue %s: wrapper=%p handle=%p storage=%p xphy=%p\n",
			       stage, wrapper, (void *)wrapper[0],
			       (void *)wrapper[1], xphyQueue);
	else
		esp_rom_printf("[S31] pp queue %s: wrapper=NULL xphy=%p\n",
			       stage, xphyQueue);
}

/*
 * The S31 mask ROM keeps the PP task pointers in a global SRAM block.  Linux
 * has observed that block being cleared after esp_wifi_init() without either
 * _task_delete or _wifi_delete_queue being called.  The backing kthread and
 * queue remain live, so retain and restore their published ROM handles.  A
 * real teardown goes through the two adapter callbacks and is never repaired
 * by this guard.
 */
void s31_radio_wifi_guard_pp_state(void)
{
	if (!s31_wifi_pp_queue || !s31_wifi_pp_handle || !s31_wifi_pp_task)
		return;
	if (s_wifi_queue == s31_wifi_pp_queue &&
	    xphyQueue == s31_wifi_pp_handle &&
	    pp_task_hdl == s31_wifi_pp_task)
		return;
	if (++s31_wifi_pp_restore_count <= 8)
		esp_rom_printf("[S31] restoring PP state #%u tick=%u q=%p/%p "
			       "xphy=%p/%p task=%p/%p\n",
			       s31_wifi_pp_restore_count, xTaskGetTickCount(),
			       s_wifi_queue, s31_wifi_pp_queue,
			       xphyQueue, s31_wifi_pp_handle,
			       pp_task_hdl, s31_wifi_pp_task);
	s_wifi_queue = s31_wifi_pp_queue;
	xphyQueue = s31_wifi_pp_handle;
	pp_task_hdl = s31_wifi_pp_task;
}

/* esp_wifi_internal_tx() copies Linux frames, so the by-reference callbacks
 * are not expected to run.  The native ESP-IDF station glue still registers
 * them at STA_START and the closed driver uses that registration as part of
 * bringing up its netstack-facing data path. */
static void s31_wifi_netstack_ref(void *buffer)
{
	(void)buffer;
}

static void s31_wifi_netstack_free(void *buffer)
{
	(void)buffer;
}

static void s31_wifi_tx_done(uint8_t interface, uint8_t *data,
			     uint16_t *length, bool status)
{
	uint32_t count = ++s31_wifi_tx_done_count;

	(void)data;
	if (count <= 8 || !status)
		esp_rom_printf("[S31] Wi-Fi TX done #%u if=%u len=%u ok=%u\n",
			       count, interface, length ? *length : 0, status);
}

static void s31_wifi_set_intr(int32_t cpu_no, uint32_t source,
			      uint32_t logical_intr, int32_t priority)
{
	esp_rom_printf("[S31] Wi-Fi _set_intr cpu=%d source=%u logical=%u prio=%d\n",
		       cpu_no, source, logical_intr, priority);
	s31_radio_wifi_intr_configure(source, logical_intr, priority);
}

static void s31_wifi_set_isr(int32_t logical_intr, void *handler, void *arg)
{
	esp_rom_printf("[S31] Wi-Fi _set_isr logical=%d handler=%p\n",
		       logical_intr, handler);
	s31_radio_wifi_intr_set_isr(logical_intr,
				    (void (*)(void *))handler, arg);
}

static void s31_wifi_ints_on(uint32_t mask)
{
	esp_rom_printf("[S31] Wi-Fi _ints_on mask=%08x\n", mask);
	s31_radio_wifi_intr_mask(mask, true);
}

static void s31_wifi_ints_off(uint32_t mask)
{
	esp_rom_printf("[S31] Wi-Fi _ints_off mask=%08x\n", mask);
	s31_radio_wifi_intr_mask(mask, false);
}

static int s31_wifi_rx(void *buffer, uint16_t length, void *eb)
{
	int rc = s31_radio_wifi_receive(buffer, length);
	uint32_t count = ++s31_wifi_rx_count;

	if (count <= 8 || rc)
		esp_rom_printf("[S31] Wi-Fi RX #%u len=%u rc=%d\n",
			       count, length, rc);

	esp_wifi_internal_free_rx_buffer(eb);
	return rc;
}

static int s31_wifi_prepare(void)
{
	static const wifi_country_t country = {
		.cc = "CN",
		.schan = 1,
		.nchan = 13,
		.policy = WIFI_COUNTRY_POLICY_MANUAL,
	};
	int rc = 0;

	if (!s31_wifi_prepared) {
		s31_wifi_report_pp_queue("before-prepare");
		esp_rom_printf("[S31] scan prepare: set_storage\n");
		/* Match the native ESP-IDF station setup used on this chip.  In
		 * particular, keep the closed driver out of its HE and modem-sleep
		 * paths until those timing services are modelled by the Linux shim. */
		rc = esp_wifi_set_storage(WIFI_STORAGE_RAM);
		esp_rom_printf("[S31] scan prepare: set_storage rc=%d\n", rc);
		if (!rc)
			esp_rom_printf("[S31] scan prepare: set_country\n"),
			rc = esp_wifi_set_country(&country);
		esp_rom_printf("[S31] scan prepare: set_country rc=%d\n", rc);
		if (!rc)
			esp_rom_printf("[S31] scan prepare: set_mode\n"),
			rc = esp_wifi_set_mode(WIFI_MODE_STA);
		esp_rom_printf("[S31] scan prepare: set_mode rc=%d\n", rc);
		if (!rc)
			esp_rom_printf("[S31] scan prepare: set_protocol\n"),
			rc = esp_wifi_set_protocol(WIFI_IF_STA,
				WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
				WIFI_PROTOCOL_11N);
		esp_rom_printf("[S31] scan prepare: set_protocol rc=%d\n", rc);
		if (!rc)
			esp_rom_printf("[S31] scan prepare: set_ps\n"),
			rc = esp_wifi_set_ps(WIFI_PS_NONE);
		esp_rom_printf("[S31] scan prepare: set_ps rc=%d\n", rc);
		if (!rc)
			esp_rom_printf("[S31] scan prepare: set_bandwidth\n"),
			rc = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
		esp_rom_printf("[S31] scan prepare: set_bandwidth rc=%d\n", rc);
		if (!rc)
			s31_wifi_prepared = 1;
	}
	return rc;
}

/* Returns one when the caller may run now, zero when STA_START will run it. */
static int s31_wifi_start_operation(enum s31_wifi_pending_operation operation)
{
	int rc;

	if (s31_wifi_start_complete)
		return 1;
	if (s31_wifi_pending != S31_WIFI_PENDING_NONE)
		return -1;
	s31_wifi_pending = operation;
	if (s31_wifi_start_requested)
		return 0;
	esp_rom_printf("[S31] calling esp_wifi_start operation=%u\n", operation);
	rc = esp_wifi_start();
	esp_rom_printf("[S31] esp_wifi_start returned rc=%d\n", rc);
	if (rc) {
		s31_wifi_pending = S31_WIFI_PENDING_NONE;
		return -rc;
	}
	s31_wifi_start_requested = 1;
	return 0;
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
	if (id == WIFI_EVENT_STA_START) {
		enum s31_wifi_pending_operation pending = s31_wifi_pending;

		s31_wifi_pending = S31_WIFI_PENDING_NONE;
		s31_wifi_start_complete = 1;
		rc = esp_wifi_internal_reg_netstack_buf_cb(
			s31_wifi_netstack_ref, s31_wifi_netstack_free);
		esp_rom_printf("[S31] Wi-Fi STA_START rc=%d pending=%u\n",
			       rc, pending);
		if (!rc && pending == S31_WIFI_PENDING_SCAN)
			rc = esp_wifi_scan_start(NULL, false);
		else if (!rc && pending == S31_WIFI_PENDING_CONNECT)
			rc = esp_wifi_connect();
		if (rc && pending == S31_WIFI_PENDING_SCAN)
			s31_radio_wifi_scan_complete(NULL, 0, rc);
		else if (rc && pending == S31_WIFI_PENDING_CONNECT)
			s31_radio_wifi_connected(NULL, 0, rc);
		return;
	}
	if (id != WIFI_EVENT_SCAN_DONE)
		goto non_scan_event;
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
	return;

non_scan_event:
	if (id == WIFI_EVENT_STA_CONNECTED) {
		wifi_event_sta_connected_t *event = event_data;

		/* Match wifi_default_action_sta_connected(): the S31 station data
		 * interface becomes ready only after association, so registering at
		 * STA_START can return success without attaching the RX data path. */
		rc = esp_wifi_internal_reg_rxcb(WIFI_IF_STA, s31_wifi_rx);
		if (!rc)
			s31_wifi_rx_registered = 1;
		if (!rc)
			rc = esp_wifi_set_tx_done_cb(s31_wifi_tx_done);
		esp_rom_printf("[S31] Wi-Fi STA connected channel=%u data-cb=%d\n",
			       event->channel, rc);
		s31_radio_wifi_connected(event->bssid, event->channel, rc);
	} else if (id == WIFI_EVENT_STA_DISCONNECTED) {
		wifi_event_sta_disconnected_t *event = event_data;

		esp_rom_printf("[S31] Wi-Fi STA disconnected reason=%u\n",
			       event->reason);
		s31_radio_wifi_disconnected(event->reason);
	}
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
	int start;

	(void)arg;
	rc = s31_wifi_prepare();
	esp_rom_printf("[S31] scan task: prepare rc=%d\n", rc);
	if (!rc) {
		start = s31_wifi_start_operation(S31_WIFI_PENDING_SCAN);
		esp_rom_printf("[S31] scan task: start_operation=%d\n", start);
		if (start > 0)
			esp_rom_printf("[S31] scan task: scan_start\n"),
			rc = esp_wifi_scan_start(NULL, false);
		else if (start < 0)
			rc = -start;
		esp_rom_printf("[S31] scan task: scan_start rc=%d\n", rc);
	}
	if (rc)
		s31_radio_wifi_scan_complete(NULL, 0, rc);
}

void s31_radio_wifi_connect_task(void *arg)
{
	const struct s31_wifi_connect_params *params = arg;
	wifi_config_t config = { 0 };
	static const char hex[] = "0123456789abcdef";
	int rc;
	int i;
	int start;

	rc = s31_wifi_prepare();
	if (rc)
		goto failed;
	memcpy(config.sta.ssid, params->ssid, params->ssid_length);
	config.sta.channel = params->channel;
	config.sta.bssid_set = params->has_bssid;
	if (params->has_bssid)
		memcpy(config.sta.bssid, params->bssid, sizeof(config.sta.bssid));
	if (params->has_password) {
		memcpy(config.sta.password, params->password,
		       params->password_length);
	} else if (params->has_psk) {
		for (i = 0; i < 32; i++) {
			config.sta.password[i * 2] = hex[params->psk[i] >> 4];
			config.sta.password[i * 2 + 1] = hex[params->psk[i] & 0xf];
		}
		config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
		/* cfg80211 supplies a derived PMK, not the plaintext needed by SAE.
		 * On WPA2/WPA3 transition BSSes force the WPA2 path so the closed
		 * driver interprets the 64 hex digits above as a raw PSK. */
		config.sta.disable_wpa3_compatible_mode = 1;
	} else {
		config.sta.threshold.authmode = WIFI_AUTH_OPEN;
	}
	rc = esp_wifi_set_config(WIFI_IF_STA, &config);
	esp_rom_printf("[S31] Wi-Fi set_config rc=%d security=%s\n", rc,
		       params->has_password ? "WPA2/WPA3" :
		       params->has_psk ? "WPA2-PSK" : "open");
	if (!rc) {
		start = s31_wifi_start_operation(S31_WIFI_PENDING_CONNECT);
		if (start > 0)
			rc = esp_wifi_connect();
		else if (start < 0)
			rc = -start;
	}
	esp_rom_printf("[S31] Wi-Fi connect submit rc=%d\n", rc);
failed:
	if (rc)
		s31_radio_wifi_connected(NULL, 0, rc);
}

void s31_radio_wifi_disconnect_task(void *arg)
{
	(void)arg;
	if (esp_wifi_disconnect())
		s31_radio_wifi_disconnected(0);
}

int s31_radio_wifi_read_mac(uint8_t *mac)
{
	/* Reading the configured STA address does not require esp_wifi_start().
	 * Keep start on a compatibility-RTOS task: the closed driver accepts a
	 * worker-thread call but never completes its internal start transition.
	 */
	return esp_wifi_get_mac(WIFI_IF_STA, mac);
}

int s31_radio_wifi_try_send(uint8_t *frame, uint16_t length)
{
	int rc = esp_wifi_internal_tx(WIFI_IF_STA, frame, length);
	uint32_t count = ++s31_wifi_tx_count;

	if (count <= 8 || rc)
		esp_rom_printf("[S31] Wi-Fi TX #%u len=%u rc=%d\n",
			       count, length, rc);
	if (rc == ESP_ERR_NO_MEM || rc == ESP_ERR_WIFI_POST ||
	    rc == ESP_ERR_WIFI_WOULD_BLOCK)
		return -EAGAIN;
	return rc ? -EIO : 0;
}
#endif

void s31_radio_stack_task(void *arg)
{
	wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
#ifndef S31_WIFI_ONLY
	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#endif
	int rc;

	(void)arg;
	/* Keep the receive-side sizing identical to the native ESP-IDF S31
	 * station image used as the hardware reference.  The build-only loader
	 * intentionally has a smaller default because it never runs Wi-Fi. */
	wifi_cfg.static_rx_buf_num = 16;
	wifi_cfg.dynamic_rx_buf_num = 40;
	wifi_cfg.rx_ba_win = 32;
	rc = psa_crypto_init();
	esp_rom_printf("[S31] psa_crypto_init rc=%d\n", rc);
	if (rc != 0) {
	#ifdef S31_LINUX_SMODE
		s31_radio_report_wifi_init(rc);
	#endif
		return;
	}
	/* Keep the closed Wi-Fi library away from the M-mode CLIC window. */
	g_coa_funcs_p = NULL;
	g_osi_funcs_p = NULL;
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
	/* ESP-IDF creates the default event loop before initialising Wi-Fi. */
	rc = esp_event_loop_create_default();
	if (rc == 0)
		rc = esp_wifi_init(&wifi_cfg);
	esp_rom_printf("[S31] esp_wifi_init rc=%d\n", rc);
	#ifdef S31_LINUX_SMODE
	if (rc == 0)
		rc = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
						s31_wifi_event, NULL);
	#endif
	if (rc == 0)
		s31_wifi_report_pp_queue("after-init");
	if (rc == 0) {
		s31_wifi_pp_queue = s_wifi_queue;
		s31_wifi_pp_handle = xphyQueue;
		s31_wifi_pp_task = pp_task_hdl;
	}
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

#ifndef S31_WIFI_ONLY
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
#endif

}

/* Linux installs the deferred CLIC route only after the init task has
 * blocked/deleted and returned control to its worker.  Controller enable is
 * therefore a distinct second-stage task. */
void s31_radio_bt_enable_task(void *arg)
{
#ifdef S31_WIFI_ONLY
	(void)arg;
#else
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
#endif
}
