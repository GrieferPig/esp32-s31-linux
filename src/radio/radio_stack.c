/* SPDX-License-Identifier: BSD-2-Clause */
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_eap_client.h"
#include <linux/esp32s31-radio-control.h>
#include "esp_bt.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_ipc.h"
#include "esp_phy_init.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_private/wifi_os_adapter.h"
#include "esp_private/wifi.h"
#include "private/esp_coexist_adapter.h"
#include "private/esp_coexist_internal.h"
#include "esp_private/esp_modem_clock.h"
#include "esp_private/esp_clk.h"
#include "esp_rom_sys.h"
#include "soc/rtc.h"
#include "soc/rsa_reg.h"

#if !CONFIG_LOG_TAG_LEVEL_IMPL_NONE || !CONFIG_LOG_DYNAMIC_LEVEL_CONTROL
#error "Native radio logging requires lock-free global runtime level control"
#endif

extern void s31_linux_printf(const char *fmt, ...);
extern uint64_t s31_linux_wall_time_seconds(void);
#include "s31_bt_lmp_reply.inc"

time_t __wrap_time(time_t *result)
{
	time_t now = (time_t)s31_linux_wall_time_seconds();

	if (result)
		*result = now;
	return now;
}
const uint32_t s31_radio_fw_abi_version = 2;
extern void s31_radio_wifi_control_complete(int result);
extern void s31_radio_wifi_ap_station(const uint8_t *mac, bool joined);
extern void s31_radio_wifi_receive_aux(uint8_t interface, const uint8_t *frame,
				     uint16_t length, uint8_t channel, int8_t signal);
/* Do not use IDF's inline xPortGetCoreID() here: on RISC-V it reads mhartid,
 * a machine-mode CSR.  The radio payload runs in Linux S-mode, so obtain the
 * worker CPU through the explicit Linux bridge instead. */
extern int32_t s31_linux_current_cpu(void);
extern void *s31_linux_current_cookie(void);
extern int32_t s31_linux_in_interrupt(void);
extern void s31_rtos_use_internal_stacks(void);
extern int s31_rtos_in_isr(void);
extern int s31_rtos_can_yield(void);
extern void s31_radio_wifi_rx_throttle(void);
/* These ROM-owned pointers live in retained SRAM.  A software reset from an
 * ESP-IDF image can leave them pointing at that image's flash/data mapping;
 * the ROM registration functions intentionally keep an existing adapter. */
extern coex_adapter_funcs_t *g_coa_funcs_p;
extern wifi_osi_funcs_t *g_osi_funcs_p;
extern void *g_wifi_global_lock;
#include "s31_wifi_rom_runtime.inc"
/* ESP-IDF invokes this from its SECONDARY system-init stage (priority 104),
 * before app_main() can initialize Wi-Fi.  The S-mode payload deliberately
 * does not run the generic IDF startup table, so preserve that ordering here.
 * WPA3/SAE uses the PSA key store for HMAC-SHA256 and otherwise fails while
 * deriving the password element. */
extern int32_t psa_crypto_init(void);
#define S31_RADIO_FEATURE_WIFI (1U << 0)
#define S31_RADIO_FEATURE_BLUETOOTH (1U << 1)
#define S31_PERIPH_WIFI_MODULE 5
/* ESP-IDF calls this from esp_rtc_init() before its system-init table.  The
 * U-Boot -> Linux path intentionally skips IDF startup, but the modem power
 * domain (in particular the BLE register aperture at 0x2010b000) still
 * depends on the PMU active-state programming performed here. */
extern void pmu_init(void);
/* Linux genpd owns HPCNNT force state after the one-time IDF PMU setup. */
extern void s31_linux_pmu_reclaim_after_radio_init(void)
	__attribute__((weak));

static int s31_radio_log_vprintf(const char *format, va_list args)
{
 char line[512];
 int length = vsnprintf(line, sizeof(line), format, args);
 if (length > 0) s31_linux_printf("%s", line);
 return length;
}

/* Wi-Fi and BTDM remain separate clients of one IDF coexistence core.  Keep
 * libnet80211's native esp_wifi_ipc_internal(): it packages each callback as a
 * Wi-Fi ioctl and executes it in the closed Wi-Fi task.  Running that callback
 * inline on the Linux radio worker changes both the owner task and the
 * happens-before relation with libcoexist's scheduler update. */
static unsigned int s31_radio_coex_users;

/* ESP-IDF's Bluedroid host reports A2DP state through vendor command 0xfc82.
 * The ESP32-S31 controller wrapper does not enable that optional VSC, but the
 * matching IDF coexistence closure exposes the same status-bit operation.
 * Keep the operation in the serialized radio world and let Linux provide only
 * the missing transport entry point. */
int s31_radio_coex_status(uint8_t type, uint8_t op, uint8_t status)
{
	if (type > COEX_SCHM_ST_TYPE_BT || op > 1)
		return -1;
	if (op)
		coex_schm_status_bit_set(type, status);
	else
		coex_schm_status_bit_clear(type, status);
	return 0;
}

extern int __real_coex_enable(void);
extern void __real_coex_disable(void);
extern int __real_coex_register_start_cb(int (*cb)(void));
extern int __real_coex_register_ble_cb(uint8_t type, void *callback);
extern int __real_coex_schm_process_restart(void);
extern int __real_coex_schm_register_callback(
	coex_schm_callback_type_t type, void *callback);
extern uint32_t coex_schm_status_get(uint32_t type);
extern void __real_coex_hw_timer_set(uint8_t idx, uint8_t src, uint8_t pti,
				     uint32_t latency, uint32_t periodic);
extern void __real_coex_hw_timer_enable(uint8_t idx);
extern void __real_coex_hw_timer_disable(uint8_t idx);
extern void *coexist_funcs;
extern int wifi_on_coex_start_process(void);
extern int wifi_on_coex_schm_phase_process(void);
extern void __real_esp_phy_enable(esp_phy_modem_t modem);
extern void __real_esp_phy_disable(esp_phy_modem_t modem);
extern esp_err_t __real_esp_ipc_call_blocking(uint32_t cpu_id,
					       esp_ipc_func_t func, void *arg);
/* Module payload text executes at the S-mode radio mapping, while the module
 * loader owns its generic read-only data relocation.  The IDF MPI helpers
 * address their four RSA blocks through the read-only MPI_BLOCK_BASES table;
 * in this execution mode that absolute-table relocation can resolve to low
 * memory.  The RSA aperture itself is already identity-mapped and used by
 * the unmodified HAL, so preserve the same four IDF mappings without making
 * the controller depend on the relocated table. */
static uintptr_t s31_mpi_block_base(uint32_t param)
{
	/* mpi_param_t is an int enum in ESP-IDF: X, Y, Z, M are 0..3.  Keep
	 * this local ABI scalar so the bridge does not need a second HAL header
	 * closure merely to replace the broken relocation table. */
	if (param == 0)
		return RSA_X_MEM;
	if (param == 1)
		return RSA_Y_MEM;
	if (param == 2)
		return RSA_Z_MEM;
	if (param == 3)
		return RSA_M_MEM;
	return 0;
}

void __wrap_mpi_hal_write_to_mem_block(uint32_t param, size_t offset,
					       const uint32_t *p, size_t n,
					       size_t num_words)
{
	volatile uint32_t *base = (volatile uint32_t *)(s31_mpi_block_base(param) +
							 offset);
	size_t copy_words = n < num_words ? n : num_words;
	size_t i;

	if (!base)
		return;
	for (i = 0; i < copy_words; i++)
		base[i] = p[i];
	for (; i < num_words; i++)
		base[i] = 0;
}

void __wrap_mpi_hal_write_at_offset(uint32_t param, int offset,
					    uint32_t value)
{
	volatile uint32_t *base = (volatile uint32_t *)(s31_mpi_block_base(param) +
							 offset);

	if (base)
		*base = value;
}

void __wrap_mpi_hal_write_rinv(uint32_t rinv)
{
	*(volatile uint32_t *)RSA_Z_MEM = rinv;
}

void __wrap_mpi_hal_read_result_hw_op(uint32_t *p, size_t n, size_t z_words)
{
	volatile uint32_t *base = (volatile uint32_t *)RSA_Z_MEM;
	size_t i;

	while (*(volatile uint32_t *)RSA_QUERY_IDLE_REG == 0) {
		/* Poll completion before clearing the accelerator interrupt. */
	}
	*(volatile uint32_t *)RSA_INT_CLR_REG = 1;
	for (i = 0; i < z_words; i++)
		p[i] = base[i];
	for (; i < n; i++)
		p[i] = 0;
}

/* ESP-IDF's dual-core controller setup asks esp_ipc to run the interrupt
 * allocator on the controller's pinned core.  The Linux bridge already runs
 * radio-init on that core, so relaying this same-core request through an IDF
 * ipc task adds a scheduling boundary but no hardware ordering.  More
 * importantly, an IDF ipc task assumes a native FreeRTOS scheduler on both
 * harts; the compatibility layer serializes payload entry behind one gate.
 * Execute only the provably same-core case inline.  Cross-core users retain
 * the original IDF implementation until their executor contract is bridged. */
esp_err_t __wrap_esp_ipc_call_blocking(uint32_t cpu_id,
				       esp_ipc_func_t func, void *arg)
{

	if (!func)
		return ESP_ERR_INVALID_ARG;
	if (cpu_id == (uint32_t)s31_linux_current_cpu()) {
		func(arg);

		return ESP_OK;
	}
	return __real_esp_ipc_call_blocking(cpu_id, func, arg);
}

static void s31_sm_coex_refresh(void);

static int (*s31_radio_coex_wifi_start_cb)(void);

static int s31_radio_coex_start(void)
{
 int (*callback)(void) = __atomic_load_n(&s31_radio_coex_wifi_start_cb,
                                        __ATOMIC_ACQUIRE);
 int rc;

 /* Persistent native task identity supports IDF's owner-aware Wi-Fi ioctl.
  * Keep the SDK dispatch instead of entering its PM handler from whichever
  * task enabled coexistence. No global vendor serialization is needed. */

 rc = callback ? callback() : 0;
 if (!rc) s31_sm_coex_refresh();

 return rc;
}

int __wrap_coex_register_start_cb(int (*cb)(void))
{
 __atomic_store_n(&s31_radio_coex_wifi_start_cb, cb, __ATOMIC_RELEASE);
 return __real_coex_register_start_cb(cb ? s31_radio_coex_start : NULL);
}

int __wrap_coex_enable(void)
{
	int rc;

	rc = __real_coex_enable();
	if (!rc)
		__atomic_add_fetch(&s31_radio_coex_users, 1, __ATOMIC_ACQ_REL);

	return rc;
}

void __wrap_coex_disable(void)
{
	unsigned int users;

	users = __atomic_load_n(&s31_radio_coex_users, __ATOMIC_ACQUIRE);
	for (;;) {
		if (!users)
			return;
		if (__atomic_compare_exchange_n(&s31_radio_coex_users, &users,
						users - 1, false,
						__ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE))
			 break;
	}
	__real_coex_disable();
}

static int s31_radio_clock_handoff(void)
{
	rtc_cpu_freq_config_t cpu_config;
	soc_rtc_slow_clk_src_t rtc_slow_src;
	modem_clock_lpclk_src_t wifi_lpclk_src;
	uint32_t slow_cal;
	uint32_t slow_hz;

	/* U-Boot and Linux already own the clock tree, so do not run esp_clk_init()
	 * and reprogram it.  Complete the metadata half of IDF's startup instead:
	 * ROM delay loops must know the already-selected CPU rate, and coexistence
	 * consumes the retained RTC-slow calibration through its OS adapter. */
	rtc_clk_cpu_freq_get_config(&cpu_config);
	if (!cpu_config.freq_mhz)
		return -1;
	esp_rom_set_cpu_ticks_per_us(cpu_config.freq_mhz);

	slow_hz = rtc_clk_slow_freq_get_hz();
	slow_cal = esp_clk_slowclk_cal_get();
	if (!slow_cal) {
		slow_cal = rtc_clk_cal(CLK_CAL_RTC_SLOW,
				       CONFIG_RTC_CLK_CAL_CYCLES);
		/* Linux may already have gated the calibration peripheral.  IDF uses
		 * this same nominal-period formula when startup calibration is disabled;
		 * it is also the safe fallback here because the slow-clock source itself
		 * remains firmware-owned and rtc_clk_slow_freq_get_hz() reads it back. */
		if (!slow_cal && slow_hz)
			slow_cal = (uint32_t)(((1ULL << 19) * 1000000ULL) /
					      slow_hz);
		if (!slow_cal)
			return -2;
		esp_clk_slowclk_cal_set(slow_cal);
	}

	/* esp_perip_clk_init() normally performs this selection before Wi-Fi is
	 * initialized.  Linux deliberately skips the generic IDF startup table,
	 * so carry over only its Wi-Fi low-power clock setup.  Without it the
	 * WIFIPWR sequencer has no source (MODEM_LPCON+0x0c == 0), while native
	 * IDF selects RC_SLOW on this board. */
	rtc_slow_src = rtc_clk_slow_src_get();
	wifi_lpclk_src = rtc_slow_src == SOC_RTC_SLOW_CLK_SRC_XTAL32K ?
		MODEM_CLOCK_LPCLK_SRC_XTAL32K : MODEM_CLOCK_LPCLK_SRC_RC_SLOW;
	modem_clock_select_lp_clock_source(S31_PERIPH_WIFI_MODULE,
					 wifi_lpclk_src, 0);

	return 0;
}

void s31_radio_wifi_clock_enable(void)
{
	modem_clock_module_enable(S31_PERIPH_WIFI_MODULE);
}

void s31_radio_wifi_clock_disable(void)
{
	modem_clock_module_disable(S31_PERIPH_WIFI_MODULE);
}

extern void s31_radio_report_wifi_init(int result);
extern void s31_radio_report_bt_init(int result);
extern void s31_radio_report_bt_enable(int result);
extern void s31_radio_report_bt_disable(int result);
extern void s31_radio_report_shutdown(int result);
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
	bool enterprise;
};
extern void s31_radio_wifi_scan_complete(const struct s31_wifi_ap *aps,
					 uint16_t count, int status);
extern void s31_radio_wifi_connected(const uint8_t *bssid, uint8_t channel,
				     int status);
extern void s31_radio_wifi_disconnected(uint16_t reason);
extern int s31_radio_wifi_receive(uint8_t *frame, uint16_t length);
extern int s31_radio_wifi_receive_zerocopy(uint8_t *frame, void *eb,
					    uint16_t length);
extern void s31_radio_wifi_intr_configure(uint32_t source,
					 uint32_t logical_intr, uint32_t priority);
extern void s31_radio_wifi_intr_set_isr(uint32_t logical_intr,
					void (*handler)(void *), void *arg);
extern void s31_radio_wifi_intr_mask(uint32_t mask, bool enable);

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

enum s31_wifi_pending_operation {
	S31_WIFI_PENDING_NONE,
	S31_WIFI_PENDING_SCAN,
	S31_WIFI_PENDING_CONNECT,
};

static int s31_wifi_prepared;
static bool s31_wifi_ap_active;
static volatile int s31_wifi_ap_ready;
static uint8_t *s31_eap_fields[S31_EAP_FIELDS];
static uint32_t s31_eap_lengths[S31_EAP_FIELDS];
static uint32_t s31_eap_received[S31_EAP_FIELDS];
static bool s31_eap_enabled;
static int s31_wifi_start_requested;
static int s31_wifi_start_complete;
/* SoftMAC start/stop acknowledgements belong to this runtime, not a retained
 * boolean from a previous open. The default loop orders START before STOP;
 * keep a timed-out stop pending until its actual event has drained. */
#define S31_WIFI_EVENT_STARTED (1U << 0)
#define S31_WIFI_EVENT_STOPPED (1U << 1)
static EventGroupHandle_t s31_wifi_lifecycle_events;
static bool s31_sm_start_waiting;
static bool s31_sm_stop_pending;

static enum s31_wifi_pending_operation s31_wifi_pending;

extern esp_err_t __real_esp_event_loop_create(const esp_event_loop_args_t *args,
					     esp_event_loop_handle_t *loop);

esp_err_t __wrap_esp_event_loop_create(const esp_event_loop_args_t *args,
				      esp_event_loop_handle_t *loop)
{
	/* The default radio event consumer completes synchronous Wi-Fi control.
	 * Its IDF priority20 can starve behind controller/service work on CPU0.
	 * Classify this dependency once at creation, with no per-event transitions
	 * or SDK-private task handle. Other event loops retain their own policy. */
	if (args && args->task_name && !strcmp(args->task_name, "sys_evt")) {
		esp_event_loop_args_t native = *args;

		native.task_priority = 23;
		return __real_esp_event_loop_create(&native, loop);
	}
	return __real_esp_event_loop_create(args, loop);
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

	(void)interface;
	(void)data;
}

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

static bool s31_wifi_is_from_isr(void)
{
	/* Match S31 esp_adapter.c exactly: _is_from_isr is implemented as
	 * !xPortCanYield(), despite its name.  On the CLIC port xPortCanYield()
	 * is false both in an ISR and while the interrupt threshold is raised by
	 * a task critical section.  Looking only at deferred-ISR nesting made the
	 * blob select blocking queue APIs while its critical section was active. */
	return !s31_rtos_can_yield();
}

/* Recycled by the Linux worker inside the blob pass once the net stack has
 * consumed the frame.  Kept in blob context (gate held) so it is serialized
 * with the MAC RX esf_buf allocator. */
void s31_radio_wifi_free_rx_buffer(void *eb)
{
	if (eb)
		esp_wifi_internal_free_rx_buffer(eb);
}

static int s31_wifi_rx(void *buffer, uint16_t length, void *eb)
{
			int rc = s31_radio_wifi_receive_zerocopy(buffer, eb, length);

	/* The Linux bridge copied the frame into its staging ring.  Return the
	 * closed driver's esf_buf before leaving the callback so RX progress does
	 * not depend on the worker reacquiring the blob gate. */
	if (eb)
		esp_wifi_internal_free_rx_buffer(eb);
	if (!rc)
		s31_radio_wifi_rx_throttle();
	return rc;
}

static int s31_wifi_ap_rx(void *buffer, uint16_t length, void *eb)
{
	s31_radio_wifi_receive_aux(S31_WIFI_IF_AP, buffer, length, 0, 0);
	if (eb)
		esp_wifi_internal_free_rx_buffer(eb);
	return 0;
}

static void s31_wifi_monitor_rx(void *buffer, wifi_promiscuous_pkt_type_t type)
{
	const wifi_promiscuous_pkt_t *packet = buffer;

	if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA)
		return;
	if (packet->rx_ctrl.sig_len < 4 || packet->rx_ctrl.sig_len > 4096 ||
	    packet->rx_ctrl.rx_state)
		return;
	s31_radio_wifi_receive_aux(S31_WIFI_IF_MONITOR, packet->payload,
		packet->rx_ctrl.sig_len, packet->rx_ctrl.channel, packet->rx_ctrl.rssi);
}

#include "s31_softmac.inc"

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
		/* Match the native ESP-IDF station setup used on this chip.  In
		 * particular, keep the closed driver out of its HE and modem-sleep
		 * paths until those timing services are modelled by the Linux shim. */
		rc = esp_wifi_set_storage(WIFI_STORAGE_RAM);
		if (!rc)
			rc = esp_wifi_set_country(&country);
		if (!rc)
			rc = esp_wifi_set_mode(WIFI_MODE_STA);
		if (!rc)
			rc = esp_wifi_set_protocol(WIFI_IF_STA,
				WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
				WIFI_PROTOCOL_11N);
		if (!rc)
			rc = esp_wifi_set_ps(WIFI_PS_NONE);
		if (!rc)
			rc = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW40);
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
	rc = esp_wifi_start();
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
	if (id == WIFI_EVENT_AP_STACONNECTED) {
		const wifi_event_ap_staconnected_t *station = event_data;

		s31_radio_wifi_ap_station(station->mac, true);
		return;
	}
	if (id == WIFI_EVENT_AP_STADISCONNECTED) {
		const wifi_event_ap_stadisconnected_t *station = event_data;

		s31_radio_wifi_ap_station(station->mac, false);
		return;
	}
	if (id == WIFI_EVENT_AP_START) {
		rc = esp_wifi_internal_reg_netstack_buf_cb(
			s31_wifi_netstack_ref, s31_wifi_netstack_free);
		if (!rc)
			rc = esp_wifi_internal_reg_rxcb(WIFI_IF_AP, s31_wifi_ap_rx);
		s31_wifi_ap_ready = rc ? -1 : 1;
		return;
	}
	if (id == WIFI_EVENT_AP_STOP) {
		s31_wifi_ap_ready = 0;
		return;
	}
	if (id == WIFI_EVENT_STA_STOP) {
		s31_wifi_start_complete = 0;
		s31_wifi_start_requested = 0;
		s31_sm_stop_pending = false;
		s31_radio_wifi_disconnected(3);
		xEventGroupSetBits(s31_wifi_lifecycle_events, S31_WIFI_EVENT_STOPPED);
		return;
	}
	if (id == WIFI_EVENT_STA_START) {
		enum s31_wifi_pending_operation pending = s31_wifi_pending;

		if (s31_sm_stop_pending) {

			return;
		}
		s31_wifi_pending = S31_WIFI_PENDING_NONE;
		s31_wifi_start_complete = 1;
		s31_wifi_start_requested = 1;

		rc = esp_wifi_internal_reg_netstack_buf_cb(
			s31_wifi_netstack_ref, s31_wifi_netstack_free);

		if (!rc && pending == S31_WIFI_PENDING_SCAN)
			rc = esp_wifi_scan_start(NULL, false);
		else if (!rc && pending == S31_WIFI_PENDING_CONNECT)
			rc = esp_wifi_connect();
		if (rc && pending == S31_WIFI_PENDING_SCAN)
			s31_radio_wifi_scan_complete(NULL, 0, rc);
		else if (rc && pending == S31_WIFI_PENDING_CONNECT)
			s31_radio_wifi_connected(NULL, 0, rc);
		if (s31_sm_start_waiting)
			xEventGroupSetBits(s31_wifi_lifecycle_events,
					   S31_WIFI_EVENT_STARTED);
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

		/* Association traffic fills the small capture ring before Linux can
		 * submit DHCP.  Start a fresh window at the protected data boundary. */

		/* Match wifi_default_action_sta_connected(): the S31 station data
		 * interface becomes ready only after association, so registering at
		 * STA_START can return success without attaching the RX data path. */
		rc = esp_wifi_internal_reg_rxcb(WIFI_IF_STA, s31_wifi_rx);
		if (!rc)
			rc = esp_wifi_set_tx_done_cb(s31_wifi_tx_done);

		s31_radio_wifi_connected(event->bssid, event->channel, rc);
	} else if (id == WIFI_EVENT_STA_DISCONNECTED) {
		wifi_event_sta_disconnected_t *event = event_data;

		s31_radio_wifi_disconnected(event->reason);
	}
}

int s31_vhci_try_send_packet(uint8_t *data, uint16_t length);

int s31_radio_vhci_try_send(uint8_t *frame, uint16_t length)
{
	if (!esp_vhci_host_check_send_available())
		return -1;

	return s31_vhci_try_send_packet(frame, length);
}

void s31_radio_wifi_scan_task(void *arg)
{
	int rc = 0;
	int start;

	(void)arg;
	rc = s31_wifi_prepare();
	if (!rc) {
		start = s31_wifi_start_operation(S31_WIFI_PENDING_SCAN);
		if (start > 0)
			rc = esp_wifi_scan_start(NULL, false);
		else if (start < 0)
			rc = -start;
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
	if (params->enterprise) {
		if (!s31_eap_enabled) {
			rc = ESP_ERR_INVALID_STATE;
			goto failed;
		}
		config.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
	} else if (params->has_password) {
		memcpy(config.sta.password, params->password,
		       params->password_length);
		/* Match the last known-good native S31 IDF path: a WPA-length
		 * plaintext password is normalized from OPEN to WPA2 internally. */
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

	if (!rc) {
		start = s31_wifi_start_operation(S31_WIFI_PENDING_CONNECT);
		if (start > 0)
			rc = esp_wifi_connect();
		else if (start < 0)
			rc = -start;
	}

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
 return esp_wifi_internal_tx(WIFI_IF_STA, frame, length);
}

int s31_radio_wifi_try_send_interface(uint8_t interface, uint8_t *frame, uint16_t length)
{
	if (interface == S31_WIFI_IF_TX_BATCH_BEGIN)
		return frame || length ? ESP_ERR_INVALID_ARG : s31_sm_tx_batch_begin();
	if (interface == S31_WIFI_IF_TX_BATCH_END)
		return frame || length ? ESP_ERR_INVALID_ARG : s31_sm_tx_batch_end();
	if (interface == S31_WIFI_IF_SOFTMAC_TX_VIEW)
		return s31_sm_send((const void *)frame, length);
	if (interface == S31_WIFI_IF_STA)
		return s31_radio_wifi_try_send(frame, length);
	if (interface != S31_WIFI_IF_AP || !s31_wifi_ap_active)
		return ESP_ERR_WIFI_IF;
	return esp_wifi_internal_tx(WIFI_IF_AP, frame, length);
}

static void s31_wifi_eap_clear(void)
{
	unsigned int i;

	esp_wifi_sta_enterprise_disable();
	esp_eap_client_clear_identity();
	esp_eap_client_clear_username();
	esp_eap_client_clear_password();
	esp_eap_client_clear_ca_cert();
	esp_eap_client_clear_certificate_and_key();
	for (i = 0; i < S31_EAP_FIELDS; i++) {
		if (s31_eap_fields[i]) {
			/* Volatile stores prevent deletion of the credential wipe. */
			volatile uint8_t *p = s31_eap_fields[i];
			uint32_t n = s31_eap_lengths[i] + 1;

			while (n--)
				*p++ = 0;
			heap_caps_free(s31_eap_fields[i]);
		}
		s31_eap_fields[i] = NULL;
		s31_eap_lengths[i] = s31_eap_received[i] = 0;
	}
	s31_eap_enabled = false;
}

static int s31_wifi_eap_control(const struct s31_wifi_control *request)
{
	uint32_t field = request->field;
	unsigned int i;
	int rc;

	if (request->operation == S31_WIFI_EAP_CLEAR) {
		s31_wifi_eap_clear();
		return 0;
	}
	if (request->operation == S31_WIFI_EAP_WRITE) {
		if (s31_eap_enabled || field >= S31_EAP_FIELDS ||
		    !request->total || request->total > S31_EAP_MAX_FIELD ||
		    !request->length || request->length > S31_EAP_CHUNK ||
		    request->offset > request->total ||
		    request->length > request->total - request->offset)
			return ESP_ERR_INVALID_ARG;
		if (!request->offset) {
			if (s31_eap_fields[field])
				return ESP_ERR_INVALID_STATE;
			s31_eap_fields[field] = heap_caps_calloc(1, request->total + 1, 0);
			if (!s31_eap_fields[field])
				return ESP_ERR_NO_MEM;
			s31_eap_lengths[field] = request->total;
		}
		if (!s31_eap_fields[field] || request->total != s31_eap_lengths[field] ||
		    request->offset != s31_eap_received[field])
			return ESP_ERR_INVALID_ARG;
		memcpy(s31_eap_fields[field] + request->offset, request->data, request->length);
		s31_eap_received[field] += request->length;
		return 0;
	}
	for (i = 0; i < S31_EAP_FIELDS; i++)
		if (s31_eap_lengths[i] != s31_eap_received[i])
			return ESP_ERR_INVALID_STATE;
	if (!s31_eap_lengths[S31_EAP_IDENTITY] || !s31_eap_lengths[S31_EAP_CA] ||
	    !s31_eap_lengths[S31_EAP_DOMAIN] ||
	    (!!s31_eap_lengths[S31_EAP_CERT] != !!s31_eap_lengths[S31_EAP_KEY]) ||
	    (!s31_eap_lengths[S31_EAP_CERT] &&
	     (!s31_eap_lengths[S31_EAP_USERNAME] || !s31_eap_lengths[S31_EAP_PASSWORD])))
		return ESP_ERR_INVALID_ARG;
	rc = esp_eap_client_set_identity(s31_eap_fields[S31_EAP_IDENTITY], s31_eap_lengths[S31_EAP_IDENTITY]);
	if (!rc)
		rc = esp_eap_client_set_ca_cert(s31_eap_fields[S31_EAP_CA], s31_eap_lengths[S31_EAP_CA] + 1);
	if (!rc)
		rc = esp_eap_client_set_domain_name((const char *)s31_eap_fields[S31_EAP_DOMAIN]);
	if (!rc)
		rc = esp_eap_client_set_disable_time_check(false);
	if (!rc && s31_eap_lengths[S31_EAP_CERT]) {
		rc = esp_eap_client_set_certificate_and_key(s31_eap_fields[S31_EAP_CERT],
			s31_eap_lengths[S31_EAP_CERT] + 1, s31_eap_fields[S31_EAP_KEY],
			s31_eap_lengths[S31_EAP_KEY] + 1, NULL, 0);
		if (!rc)
			rc = esp_eap_client_set_eap_methods(ESP_EAP_TYPE_TLS);
	} else if (!rc) {
		rc = esp_eap_client_set_username(s31_eap_fields[S31_EAP_USERNAME], s31_eap_lengths[S31_EAP_USERNAME]);
		if (!rc)
			rc = esp_eap_client_set_password(s31_eap_fields[S31_EAP_PASSWORD], s31_eap_lengths[S31_EAP_PASSWORD]);
		if (!rc)
			rc = esp_eap_client_set_eap_methods(ESP_EAP_TYPE_PEAP);
	}
	if (!rc)
		rc = esp_wifi_sta_enterprise_enable();
	s31_eap_enabled = !rc;
	return rc;
}

/* AP configuration is a cache-on task operation, never an IRQ callback.
 * Keep its large configuration object and cold setup in XIP, reserving SRAM
 * for the controller scheduler's admission deadline path. Vendor calls keep
 * their original ordering and restore their own temporary cache state. */
static int __attribute__((noinline)) s31_wifi_ap_start_control(
	const struct s31_wifi_control *request)
{
	wifi_config_t config = { 0 };
	static const char hex[] = "0123456789abcdef";
	unsigned int i;
	int rc;

	if (!request->ssid_length || request->ssid_length > 32 ||
	    request->password_length > 63 || request->channel < 1 ||
	    request->channel > 13) {
		rc = ESP_ERR_INVALID_ARG;
		return rc;
	}
	/* Configure before starting the AP: never briefly advertise an
	 * unprotected default BSS while installing the requested keys. */
	if (s31_wifi_start_requested) {
		rc = esp_wifi_stop();
		if (rc)
			return rc;
		s31_wifi_start_requested = 0;
		s31_wifi_start_complete = 0;
	}
	rc = esp_wifi_set_mode(WIFI_MODE_APSTA);
	if (rc)
		return rc;
	memcpy(config.ap.ssid, request->ssid, request->ssid_length);
	config.ap.ssid_len = request->ssid_length;
	config.ap.channel = request->channel;
	config.ap.ssid_hidden = request->hidden;
	config.ap.max_connection = request->max_connections;
	config.ap.beacon_interval = request->beacon_interval;
	config.ap.dtim_period = request->dtim_period;
	config.ap.authmode = WIFI_AUTH_OPEN;
	if (request->has_psk) {
		for (i = 0; i < 32; i++) {
			config.ap.password[2 * i] = hex[request->psk[i] >> 4];
			config.ap.password[2 * i + 1] = hex[request->psk[i] & 15];
		}
		config.ap.authmode = WIFI_AUTH_WPA2_PSK;
	} else if (request->password_length) {
		memcpy(config.ap.password, request->password, request->password_length);
		config.ap.authmode = WIFI_AUTH_WPA3_PSK;
		config.ap.pmf_cfg.required = true;
		config.ap.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
	}
	rc = esp_wifi_set_mac(WIFI_IF_AP, request->mac);
	if (!rc)
		rc = esp_wifi_set_config(WIFI_IF_AP, &config);
	s31_wifi_ap_ready = 0;
	if (!rc)
		rc = esp_wifi_start();
	if (!rc) {
		s31_wifi_start_requested = 1;
		for (i = 0; i < 200 && !s31_wifi_ap_ready; i++)
			vTaskDelay(pdMS_TO_TICKS(10) ?: 1);
		if (s31_wifi_ap_ready != 1)
			rc = ESP_ERR_TIMEOUT;
	}
	s31_wifi_ap_active = !rc;
	if (rc) {
		esp_wifi_stop();
		esp_wifi_set_mode(WIFI_MODE_STA);
		s31_wifi_start_requested = 0;
		s31_wifi_start_complete = 0;
	}
	return rc;
}

void s31_radio_wifi_control_task(void *arg)
{
	const struct s31_wifi_control *request = arg;
	int rc = s31_wifi_prepare();
	unsigned int i;

	if (rc)
		goto done;
	switch (request->operation) {
	case S31_WIFI_SOFTMAC_TX_VIEW_CAP:
		/* Retired per-frame locking contract; never run an old frontend. */
		rc = ESP_ERR_NOT_SUPPORTED;
		break;
	case S31_WIFI_SOFTMAC_TX_BATCH_CAP:
		rc = s31_rtos_softmac_tx_copy(NULL) == S31_SOFTMAC_TX_BATCH_ABI ?
			0 : ESP_ERR_NOT_SUPPORTED;
		break;
	case S31_WIFI_SOFTMAC_TX_HT_CAP:
	case S31_WIFI_SOFTMAC_TX_AGG_CAP:
	case S31_WIFI_SOFTMAC_TX_HE_CAP:
	case S31_WIFI_SOFTMAC_TX_HE9_CAP:
		rc = 0;
		break;
	case S31_WIFI_SOFTMAC_TX_PEER_ADD:
		rc = s31_sm_tx_peer_add(request);
		break;
	case S31_WIFI_SOFTMAC_TX_PEER_DEL:
	case S31_WIFI_SOFTMAC_TX_BA_ON:
	case S31_WIFI_SOFTMAC_TX_BA_OFF:
	case S31_WIFI_SOFTMAC_TX_BA_FLUSH:
		rc = s31_sm_tx_ba_control(request);
		break;
	case S31_WIFI_SOFTMAC_START:
		rc = s31_sm_start(request);
		break;
	case S31_WIFI_SOFTMAC_STOP:
		rc = s31_sm_stop();
		break;
	case S31_WIFI_SOFTMAC_RX_BA_ADD:
	case S31_WIFI_SOFTMAC_RX_BA_DEL:
		rc = s31_sm_rx_ba(request);
		break;
	case S31_WIFI_SOFTMAC_EDCA:
		rc = s31_sm_edca(request);
		break;
	case S31_WIFI_SOFTMAC_RX_KEY_ADD:
	case S31_WIFI_SOFTMAC_RX_KEY_DEL:
		rc = s31_sm_rx_key(request);
		break;
	case S31_WIFI_SOFTMAC_BSSID_FILTER:
		rc = s31_sm_bssid_filter(request);
		break;
	case S31_WIFI_SOFTMAC_HE_BSS:
		rc = s31_sm_he_bss(request);
		break;
	case S31_WIFI_SOFTMAC_COEX_BSS:
		rc = s31_sm_coex_bss(request);
		break;
	case S31_WIFI_EAP_WRITE:
	case S31_WIFI_EAP_COMMIT:
	case S31_WIFI_EAP_CLEAR:
		rc = s31_wifi_eap_control(request);
		break;
	case S31_WIFI_AP_START:
		rc = s31_wifi_ap_start_control(request);
		break;
	case S31_WIFI_AP_STOP:
		rc = esp_wifi_set_mode(WIFI_MODE_STA);
		if (!rc)
			s31_wifi_ap_active = false;
		break;
	case S31_WIFI_AP_DEAUTH: {
		uint16_t aid = 0;
		bool all = true;

		for (i = 0; i < 6; i++)
			all &= request->mac[i] == 0xff;
		if (!all)
			rc = esp_wifi_ap_get_sta_aid(request->mac, &aid);
		if (!rc)
			rc = esp_wifi_deauth_sta(aid);
		break;
	}
	case S31_WIFI_MONITOR_START:
		if (!s31_wifi_start_requested) {
			rc = esp_wifi_start();
			if (!rc)
				s31_wifi_start_requested = 1;
		}
		if (!rc)
			rc = esp_wifi_set_promiscuous_rx_cb(s31_wifi_monitor_rx);
		if (!rc)
			rc = esp_wifi_set_promiscuous(true);
		break;
	case S31_WIFI_MONITOR_STOP:
		rc = esp_wifi_set_promiscuous(false);
		if (!rc)
			rc = esp_wifi_set_promiscuous_rx_cb(NULL);
		break;
	case S31_WIFI_SET_CHANNEL:
		if (request->channel < 1 || request->channel > 13)
			rc = ESP_ERR_INVALID_ARG;
		else {
			if (!s31_wifi_start_requested) {
				rc = esp_wifi_start();
				if (!rc)
					s31_wifi_start_requested = 1;
			}
			if (!rc)
				rc = esp_wifi_set_channel(request->channel, WIFI_SECOND_CHAN_NONE);
		}
		break;
	default:
		rc = ESP_ERR_NOT_SUPPORTED;
		break;
	}
done:
	s31_radio_wifi_control_complete(rc);
}

/* Called only before controller/PHY init, while the normal XIP mapping and
 * cache are available. Keep configuration-only code out of the hot IRAM tail. */
static void __attribute__((noinline)) s31_radio_bt_configure_init(
    esp_bt_controller_config_t *config, uintptr_t features)
{

	_Static_assert(UC_BR_EDR_ACLU_TX_BUF_NB == 10 &&
		UC_BR_EDR_DYNAMIC_ACLU_TX_BUF_NB == 0 &&
		UC_BR_EDR_ACLU_RX_BUF_NB == 8,
		"Native controller requires the default IDF performance buffer depths");

}

void s31_radio_stack_task(void *arg)
{
	wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
	uintptr_t features = (uintptr_t)arg;
	bool enable_wifi = (features & S31_RADIO_FEATURE_WIFI) != 0;
	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
	bool enable_bt = (features & S31_RADIO_FEATURE_BLUETOOTH) != 0;
	int rc;

	s31_radio_bt_configure_init(&bt_cfg, features);

	pmu_init();
	rc = s31_radio_clock_handoff();
	if (rc != 0) {

		if (enable_wifi)
			s31_radio_report_wifi_init(rc);
		if (enable_bt)
			s31_radio_report_bt_init(rc);
		return;
	}
	if (s31_linux_pmu_reclaim_after_radio_init)
		s31_linux_pmu_reclaim_after_radio_init();
	/* ESP-IDF otherwise writes directly to UART0, bypassing Linux console log
	 * levels and corrupting interactive applications such as esp32-config.
	 * Route it through printk so diagnostics remain in dmesg while Linux owns
	 * whether they are shown on the serial console.  This also keeps the blob
	 * out of the ROM UART busy-poll path. */
	esp_log_set_vprintf(s31_radio_log_vprintf);
	esp_log_level_set("*", ESP_LOG_WARN);
	/* Keep the explicit native-IDF coexistence baseline.  The larger Linux
	 * direct-netdev values (32 static + 48 dynamic) reserve roughly 80 KiB of
	 * radio heap/DMA state, but did not eliminate the Type-2 LMAC stalls during
	 * A2DP traffic.  Matching the IDF 10 + 32 policy both frees that pressure
	 * and makes the throughput comparison meaningful. */
	wifi_cfg.static_rx_buf_num = 10;
	wifi_cfg.dynamic_rx_buf_num = 32;
	/* Keep the native 11n receive aggregation path.  The Linux esp_timer shim
	 * supplies the BlockAck reorder timeout and safely handles timer deletion
	 * from callbacks. */
	wifi_cfg.ampdu_rx_enable = 1;
	/* Keep IDF's TX aggregation path in both single-radio profiles.  Disabling
	 * it only for Wi-Fi+BT makes TCP ACK completions take seconds while the
	 * controller is enabled, even though the Linux enqueue path is healthy. */
	wifi_cfg.ampdu_tx_enable = 1;

	wifi_cfg.rx_ba_win = 6;
 if (features & S31_RADIO_FEATURE_RX_LEAN16) {
  wifi_cfg.static_rx_buf_num = 16;
  wifi_cfg.dynamic_rx_buf_num = 32;
  wifi_cfg.rx_ba_win = 32;
 }

	/* Re-register the OS adapter and rebuild the ROM callback dispatch table for
	 * the relocated Linux module.  Both pointers live in retained HP SRAM.  The
	 * dispatch table contains coex_core_* function addresses from the previous
	 * boot image, so retaining it would mix boot-firmware callbacks with the
	 * Linux-owned coexistence environment. */
	g_coa_funcs_p = NULL;
	coexist_funcs = NULL;
	if (enable_wifi) {
		rc = psa_crypto_init();

		if (rc != 0) {
			s31_radio_report_wifi_init(rc);
			return;
		}
		/* The previous runtime has joined all owners before heap reclamation. */
		s31_wifi_rom_runtime_begin();
		/* Keep the closed Wi-Fi library away from the M-mode CLIC window. */
		g_wifi_osi_funcs._set_intr = s31_wifi_set_intr;
		g_wifi_osi_funcs._set_isr = s31_wifi_set_isr;
		g_wifi_osi_funcs._ints_on = s31_wifi_ints_on;
		g_wifi_osi_funcs._ints_off = s31_wifi_ints_off;
		g_wifi_osi_funcs._is_from_isr = s31_wifi_is_from_isr;
		/* Linux owns flash/MTD; do not let the IDF blob open its NVS backend. */
		wifi_cfg.nvs_enable = 0;
	}
	/* Replace the loader/FreeRTOS callbacks retained by the COEX ROM. */
	rc = esp_coex_adapter_register(&g_coex_adapter_funcs);
	if (rc != 0) {

		if (enable_wifi)
			s31_radio_report_wifi_init(rc);
		if (enable_bt)
			s31_radio_report_bt_init(rc);
		return;
	}
	/*
	 * ESP-IDF normally performs these two calls together from its
	 * SECONDARY system-init hook.  Registering the adapter alone leaves
	 * the coexistence lock/environment uninitialised; esp_wifi_init() then
	 * faults as soon as it registers its scheduler callbacks.
	 */
	rc = coex_pre_init();

	if (rc != 0) {
		if (enable_wifi)
			s31_radio_report_wifi_init(rc);
		if (enable_bt)
			s31_radio_report_bt_init(rc);
		return;
	}
	if (enable_wifi) {
		/* ESP-IDF creates the default event loop before initialising Wi-Fi. */
		s31_wifi_lifecycle_events = xEventGroupCreate();
		rc = s31_wifi_lifecycle_events ? esp_event_loop_create_default() :
			ESP_ERR_NO_MEM;
		if (rc == 0)
			rc = esp_wifi_init(&wifi_cfg);

		if (rc == 0)
			rc = esp_event_handler_register(WIFI_EVENT,
							ESP_EVENT_ANY_ID,
							s31_wifi_event, NULL);
		s31_radio_report_wifi_init(rc);
		if (rc != 0)
			return;
	} else {
		s31_radio_report_wifi_init(0);
	}

	if (!enable_bt) {
		s31_radio_report_bt_init(0);
		return;
	}
	/* btdm_lp_timer_clk_init owns both the LP clock selection and its cached
	 * frequency. With controller sleep disabled, the current SDK config uses
	 * the internal RC source, matching the recorded stock-IDF benchmark.
	 * Do not set only its source here: that skips frequency initialization. */
	rc = esp_bt_controller_init(&bt_cfg);

	s31_radio_report_bt_init(rc);

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

	/* The second coex_enable() synchronously invokes Wi-Fi's registered start
	 * callback.  pm_on_coex_start() already restarts the Wi-Fi coexistence
	 * scheduler; restarting the closed coexist core again here corrupts that
	 * freshly selected phase and can starve Wi-Fi while BTDM is idle. */
	if (rc == 0) {
		rc = esp_vhci_host_register_callback(&s31_vhci_callbacks);

	}
	s31_radio_report_bt_enable(rc);

}

void s31_radio_bt_disable_task(void *arg)
{
	int rc;

	(void)arg;
	s31_rtos_use_internal_stacks();
	rc = esp_bt_controller_disable();

	s31_radio_report_bt_disable(rc);
}

void s31_radio_shutdown_task(void *arg)
{
	uintptr_t features = (uintptr_t)arg;
	int result = 0;
	int rc;

	if (features & S31_RADIO_FEATURE_BLUETOOTH) {
		rc = esp_bt_controller_disable();
		if (rc != 0 && rc != ESP_ERR_INVALID_STATE && !result)
			result = rc;

		rc = esp_bt_controller_deinit();
		if (rc != 0 && rc != ESP_ERR_INVALID_STATE && !result)
			result = rc;

	}
	if (features & S31_RADIO_FEATURE_WIFI) {
		esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
					     s31_wifi_event);
		esp_wifi_disconnect();
		rc = esp_wifi_stop();
		if (rc != 0 && rc != ESP_ERR_WIFI_NOT_INIT && !result)
			result = rc;

		if (s31_sm_cb_registered) {
			pp_unregister_tx_cb(S31_SM_CB_INDEX);
			s31_sm_cb_registered = false;
		}
		s31_sm_active = false;
		memset(s31_sm_flights, 0, sizeof(s31_sm_flights));
		rc = esp_wifi_deinit();
		if (rc != 0 && rc != ESP_ERR_WIFI_NOT_INIT && !result)
			result = rc;

		rc = esp_event_loop_delete_default();
		if (rc && rc != ESP_ERR_INVALID_STATE && !result)
			result = rc;
		if ((!rc || rc == ESP_ERR_INVALID_STATE) && s31_wifi_lifecycle_events) {
			vEventGroupDelete(s31_wifi_lifecycle_events);
			s31_wifi_lifecycle_events = NULL;
		}
		/* The next module instance may initialize Wi-Fi after BT has used the
		 * shared modem domain.  IDF resets WIFIMAC on the init path, but an
		 * explicit post-deinit reset also clears retained TX/RX state before
		 * the Linux module and its interrupt mappings disappear.  Without it,
		 * a Wi-Fi -> BT -> Wi-Fi lifecycle can associate while the second
		 * radio worker never observes the queued DHCP frames. */
		modem_clock_module_mac_reset(S31_PERIPH_WIFI_MODULE);

	}
	s31_radio_report_shutdown(result);
}
