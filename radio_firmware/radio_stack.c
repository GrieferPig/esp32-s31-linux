/* SPDX-License-Identifier: BSD-2-Clause */
#include <stdint.h>
#include <string.h>
#include "esp_bt.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_private/wifi_os_adapter.h"
#include "esp_private/wifi.h"
#include "private/esp_coexist_adapter.h"
#include "private/esp_coexist_internal.h"
#include "btdm_lp.h"


extern uint64_t s31_linux_time_ns(void);
extern void s31_linux_printf(const char *fmt, ...);
extern void s31_rtos_use_internal_stacks(void);
extern int s31_rtos_in_isr(void);
extern int s31_rtos_can_yield(void);
extern void s31_radio_timing_tx_done(bool status, const uint8_t *data,
				     uint16_t length);
/* These ROM-owned pointers live in retained SRAM.  A software reset from an
 * ESP-IDF image can leave them pointing at that image's flash/data mapping;
 * the ROM registration functions intentionally keep an existing adapter. */
extern coex_adapter_funcs_t *g_coa_funcs_p;
extern wifi_osi_funcs_t *g_osi_funcs_p;
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
extern int s31_radio_wifi_receive_zerocopy(uint8_t *frame, void *eb,
					    uint16_t length);
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

#define S31_TX_DESC_SAMPLES 24
#define S31_TX_DESC_BYTES 72
#define S31_TX_FRAME_BYTES 48

struct s31_tx_desc_sample {
	uint32_t eb;
	uint32_t frame;
	uint32_t flags;
	uint32_t control;
	uint16_t length;
	uint8_t tid;
	uint8_t desc[S31_TX_DESC_BYTES];
	uint8_t header[S31_TX_FRAME_BYTES];
};

static struct s31_tx_desc_sample s31_tx_desc_samples[S31_TX_DESC_SAMPLES];
static uint32_t s31_tx_desc_sample_count;
static bool s31_tx_desc_samples_dumped;
static uint32_t s31_tx_desc_call_count;
static uint32_t s31_tx_desc_bad_eb_count;
static uint32_t s31_tx_desc_bad_desc_count;
static uint32_t s31_tx_desc_bad_frame_count;
static uint32_t s31_tx_desc_nondata_count;
static bool s31_tx_desc_capture_enabled;
static uint32_t s31_tx_desc_ipv4_submit_count;

#define S31_KEY_SAMPLES 16
#define S31_KEY_INFO_BYTES 12

struct s31_key_sample {
	uint32_t slot;
	uint32_t key_len;
	uint32_t key_hash;
	uint8_t key_info[S31_KEY_INFO_BYTES];
};

static struct s31_key_sample s31_key_samples[S31_KEY_SAMPLES];
static uint32_t s31_key_sample_count;
static bool s31_key_capture_enabled;

static uint32_t s31_lmac_txerr_count;
static uint32_t s31_lmac_txerr_seckid_count;
static uint32_t s31_lmac_txdone_count;
static uint32_t s31_lmac_txdone_bad_count;

extern void __real_ieee80211_set_tx_desc(void *ic, void *eb, uint32_t tid,
					 uint32_t flags, uint32_t control);
extern int __real_lmacTxFrame(void *eb, uint32_t queue);
extern void __real_hal_crypto_set_key_entry(int key_idx, const void *key,
					    int key_len, const void *key_info);
extern void __real_lmacProcessTxError(int err_type, int status, void *arg);
extern int __real_lmacTxDone(void *eb, int status);
extern int esp_test_get_hw_rx_statistics(uint16_t *stats);

static bool s31_radio_ptr_is_hpsram(const void *ptr, size_t length)
{
	uintptr_t start = (uintptr_t)ptr;
	uintptr_t end = start + length;

	return start >= 0x2f000000U && end >= start && end <= 0x2f800000U;
}

static uint32_t s31_fnv1a(const uint8_t *data, size_t len)
{
	uint32_t hash = 2166136261u;
	size_t i;

	for (i = 0; i < len; i++) {
		hash ^= data[i];
		hash *= 16777619u;
	}
	return hash;
}

void __wrap_ieee80211_set_tx_desc(void *ic, void *eb, uint32_t tid,
				  uint32_t flags, uint32_t control)
{
	struct s31_tx_desc_sample *sample;
	uint8_t *desc;
	uint8_t *frame = NULL;
	void *buffer;
	uint32_t index;
	uint32_t i;

	__real_ieee80211_set_tx_desc(ic, eb, tid, flags, control);
	s31_tx_desc_call_count++;
	if (!s31_radio_ptr_is_hpsram(eb, 56)) {
		s31_tx_desc_bad_eb_count++;
		return;
	}
	desc = *(uint8_t **)((uint8_t *)eb + 52);
	buffer = *(void **)((uint8_t *)eb + 4);
	if (s31_radio_ptr_is_hpsram(buffer, 8))
		frame = *(uint8_t **)((uint8_t *)buffer + 4);
	if (!s31_radio_ptr_is_hpsram(desc, S31_TX_DESC_BYTES)) {
		s31_tx_desc_bad_desc_count++;
		return;
	}
	if (!s31_radio_ptr_is_hpsram(frame, 8 + S31_TX_FRAME_BYTES)) {
		s31_tx_desc_bad_frame_count++;
		return;
	}
	frame += 8;
	if (!s31_tx_desc_capture_enabled || (frame[0] & 0x0c) != 0x08 ||
	    !(frame[1] & 0x40)) {
		s31_tx_desc_nondata_count++;
		return;
	}
	index = s31_tx_desc_sample_count;
	if (index >= S31_TX_DESC_SAMPLES)
		return;
	s31_tx_desc_sample_count = index + 1;
	sample = &s31_tx_desc_samples[index];
	sample->eb = (uintptr_t)eb;
	sample->frame = (uintptr_t)frame;
	sample->flags = flags;
	sample->control = control;
	sample->length = *(uint16_t *)((uint8_t *)eb + 22);
	sample->tid = tid;
	for (i = 0; i < S31_TX_DESC_BYTES; i++)
		sample->desc[i] = desc[i];
	for (i = 0; i < S31_TX_FRAME_BYTES; i++)
		sample->header[i] = frame[i];
}

int __wrap_lmacTxFrame(void *eb, uint32_t queue)
{
	struct s31_tx_desc_sample *sample;
	uint8_t *desc;
	uint8_t *frame = NULL;
	void *buffer;
	uint32_t index;
	uint32_t i;

	s31_tx_desc_call_count++;
	if (!s31_radio_ptr_is_hpsram(eb, 56)) {
		s31_tx_desc_bad_eb_count++;
		return __real_lmacTxFrame(eb, queue);
	}
	desc = *(uint8_t **)((uint8_t *)eb + 52);
	buffer = *(void **)((uint8_t *)eb + 4);
	if (s31_radio_ptr_is_hpsram(buffer, 8))
		frame = *(uint8_t **)((uint8_t *)buffer + 4);
	if (!s31_radio_ptr_is_hpsram(desc, S31_TX_DESC_BYTES)) {
		s31_tx_desc_bad_desc_count++;
		return __real_lmacTxFrame(eb, queue);
	}
	if (!s31_radio_ptr_is_hpsram(frame, 8 + S31_TX_FRAME_BYTES)) {
		s31_tx_desc_bad_frame_count++;
		return __real_lmacTxFrame(eb, queue);
	}
	frame += 8;
	if (!s31_tx_desc_capture_enabled || (frame[0] & 0x0c) != 0x08 ||
	    !(frame[1] & 0x40)) {
		s31_tx_desc_nondata_count++;
		return __real_lmacTxFrame(eb, queue);
	}
	index = s31_tx_desc_sample_count;
	if (index < S31_TX_DESC_SAMPLES) {
		s31_tx_desc_sample_count = index + 1;
		sample = &s31_tx_desc_samples[index];
		sample->eb = (uintptr_t)eb;
		sample->frame = (uintptr_t)frame;
		sample->flags = 0x4c4d4143U;
		sample->control = queue;
		sample->length = *(uint16_t *)((uint8_t *)eb + 22);
		sample->tid = queue;
		for (i = 0; i < S31_TX_DESC_BYTES; i++)
			sample->desc[i] = desc[i];
		for (i = 0; i < S31_TX_FRAME_BYTES; i++)
			sample->header[i] = frame[i];
	}
	return __real_lmacTxFrame(eb, queue);
}

void __wrap_hal_crypto_set_key_entry(int key_idx, const void *key,
				     int key_len, const void *key_info)
{
	struct s31_key_sample *sample;
	const uint8_t *info = key_info;
	uint32_t i;

	(void)key_len;
	__real_hal_crypto_set_key_entry(key_idx, key, key_len, key_info);
	if (!s31_key_capture_enabled ||
	    !s31_radio_ptr_is_hpsram(info, S31_KEY_INFO_BYTES) ||
	    s31_key_sample_count >= S31_KEY_SAMPLES)
		return;
	sample = &s31_key_samples[s31_key_sample_count++];
	sample->slot = key_idx;
	sample->key_len = key_len;
	sample->key_hash = s31_fnv1a(key, key_len);
	for (i = 0; i < S31_KEY_INFO_BYTES; i++)
		sample->key_info[i] = info[i];
}

void __wrap_lmacProcessTxError(int err_type, int status, void *arg)
{
	uint32_t count = ++s31_lmac_txerr_count;

	if (status == 192)
		s31_lmac_txerr_seckid_count++;
	if (count <= 32 || status == 192 || status == 0 || status == 1 ||
	    status == 2)
		s31_linux_printf("[S31] LMACTXERR #%u type=%d status=%d(0x%x) arg=%p\n",
			       count, err_type, status, status, arg);
	__real_lmacProcessTxError(err_type, status, arg);
}

int __wrap_lmacTxDone(void *eb, int status)
{
	uint8_t *desc = NULL;
	uint32_t w0 = 0;
	uint32_t w4 = 0;
	uint32_t count = ++s31_lmac_txdone_count;

	if (status != 0)
		s31_lmac_txdone_bad_count++;
	if (s31_radio_ptr_is_hpsram(eb, 56)) {
		desc = *(uint8_t **)((uint8_t *)eb + 52);
		if (s31_radio_ptr_is_hpsram(desc, 72)) {
			w0 = *(uint32_t *)(desc + 0);
			w4 = *(uint32_t *)(desc + 16);
		}
	}
	if (count <= 4)
		s31_linux_printf("[S31] LMACTXDONE #%u status=%d w0=%08x w4=%08x eb=%p\n",
			       count, status, w0, w4, eb);
	return __real_lmacTxDone(eb, status);
}

static void s31_wifi_dump_crypto_regs(void);
static void s31_wifi_dump_rx_stats(void);

static void s31_wifi_dump_crypto_samples(void)
{
	uint32_t i;
	uint32_t j;

	s31_linux_printf("[S31] LMACTXERR total=%u seckid=%u txdone=%u txdone-bad=%u keys=%u\n",
		       s31_lmac_txerr_count, s31_lmac_txerr_seckid_count,
		       s31_lmac_txdone_count, s31_lmac_txdone_bad_count,
		       s31_key_sample_count);
	for (i = 0; i < s31_key_sample_count; i++) {
		struct s31_key_sample *sample = &s31_key_samples[i];

		s31_linux_printf("[S31] KEY #%u slot=%u len=%u fnv=%08x info=",
			       i, sample->slot, sample->key_len, sample->key_hash);
		for (j = 0; j < S31_KEY_INFO_BYTES; j++)
			s31_linux_printf("%02x", sample->key_info[j]);
		s31_linux_printf("\n");
	}
	s31_wifi_dump_crypto_regs();
}

static void s31_wifi_dump_crypto_regs(void)
{
	volatile const uint32_t *base = (volatile const uint32_t *)0x20104800;
	uint32_t slot;

	s31_linux_printf("[S31] CRYPTO c0=%08x c1=%08x c2=%08x c3=%08x cfg=%08x valid=%08x\n",
		       base[0], base[1], base[2], base[3], base[4], base[5]);
	/* Dump only the key-entry metadata words (peer MAC + cipher word) and an
	 * FNV-1a fingerprint of the installed key bytes (never the key itself). */
	for (slot = 0; slot < 6; slot++) {
		volatile const uint32_t *entry =
			(volatile const uint32_t *)(0x20105800 + slot * 40);
		uint32_t hash;

		if (!(base[5] & (1u << slot)))
			continue;
		hash = s31_fnv1a((const uint8_t *)(entry + 2), 16);
		s31_linux_printf("[S31] KEYS slot=%u hdr0=%08x hdr1=%08x keyfnv=%08x\n",
			       slot, entry[0], entry[1], hash);
	}
	s31_wifi_dump_rx_stats();
}

static void s31_wifi_dump_rx_stats(void)
{
	uint16_t stats[48] = { 0 };
	uint32_t i;
	int rc = esp_test_get_hw_rx_statistics(stats);

	s31_linux_printf("[S31] RXSTAT rc=%d", rc);
	for (i = 0; i < 37; i++)
		s31_linux_printf(" %u:%u", i, stats[i]);
	s31_linux_printf(" w38=%08x w40=%08x\n",
		       *(uint32_t *)((uint8_t *)stats + 76),
		       *(uint32_t *)((uint8_t *)stats + 80));
}

static void s31_wifi_dump_tx_desc_samples(void)
{
	uint32_t count = s31_tx_desc_sample_count;
	uint32_t i;
	uint32_t j;

	if (s31_tx_desc_samples_dumped)
		return;
	if (count > S31_TX_DESC_SAMPLES)
		count = S31_TX_DESC_SAMPLES;
	s31_linux_printf("[S31] TXDESC status calls=%u samples=%u bad-eb=%u bad-desc=%u bad-frame=%u nondata=%u\n",
		       s31_tx_desc_call_count, count, s31_tx_desc_bad_eb_count,
		       s31_tx_desc_bad_desc_count, s31_tx_desc_bad_frame_count,
		       s31_tx_desc_nondata_count);
	if (!count)
		return;
	s31_tx_desc_samples_dumped = true;
	for (i = 0; i < count; i++) {
		struct s31_tx_desc_sample *sample = &s31_tx_desc_samples[i];

		s31_linux_printf("[S31] TXDESC #%u eb=%08x frame=%08x len=%u tid=%u flags=%08x ctl=%08x desc=",
			       i, sample->eb, sample->frame, sample->length,
			       sample->tid, sample->flags, sample->control);
		for (j = 0; j < S31_TX_DESC_BYTES; j++)
			s31_linux_printf("%02x", sample->desc[j]);
		s31_linux_printf(" hdr=");
		for (j = 0; j < S31_TX_FRAME_BYTES; j++)
			s31_linux_printf("%02x", sample->header[j]);
		s31_linux_printf("\n");
	}
}

/* esp_wifi_internal_tx() copies Linux frames, so the by-reference callbacks
 * are not expected to run.  The native ESP-IDF station glue still registers
 * them at STA_START and the closed driver uses that registration as part of
 * bringing up its netstack-facing data path. */
static void s31_wifi_netstack_ref(void *buffer)
{
	static uint32_t count;
	if (++count <= 16)
		s31_linux_printf("[S31] netstack ref #%u buffer=%p\n", count, buffer);
}

static void s31_wifi_netstack_free(void *buffer)
{
	static uint32_t count;
	if (++count <= 16)
		s31_linux_printf("[S31] netstack free #%u buffer=%p\n", count, buffer);
}

static void s31_wifi_tx_done(uint8_t interface, uint8_t *data,
			     uint16_t *length, bool status)
{
	uint32_t count = ++s31_wifi_tx_done_count;

	s31_radio_timing_tx_done(status, data, length ? *length : 0);
	(void)data;
	/* s31_linux_printf() busy-polls the ROM UART, so a per-frame print here
	 * serializes the whole gate hold behind 115200-baud output and was
	 * measured as the long Wi-Fi queue-receive hold.  Keep only the first
	 * few completions for bring-up diagnostics. */
	if (!status && count <= 16)
		s31_linux_printf("[S31] Wi-Fi TX done #%u if=%u len=%u ok=%u\n",
			       count, interface, length ? *length : 0, status);
}

static void s31_wifi_set_intr(int32_t cpu_no, uint32_t source,
			      uint32_t logical_intr, int32_t priority)
{
	(void)cpu_no;
	s31_radio_wifi_intr_configure(source, logical_intr, priority);
}

static void s31_wifi_set_isr(int32_t logical_intr, void *handler, void *arg)
{
	s31_linux_printf("[S31] Wi-Fi set_isr logical=%d handler=%p arg=%p\n",
		       logical_intr, handler, arg);
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
	uint32_t count = ++s31_wifi_rx_count;

	if (count <= 8 || rc)
		s31_linux_printf("[S31] Wi-Fi RX #%u len=%u rc=%d\n",
			       count, length, rc);

	/* eb is NOT freed here anymore: it travels with the zero-copy RX
	 * descriptor and is recycled by s31_radio_wifi_free_pending().  If the
	 * ring was full (rc == -ENOSPC), recycle it right away so the frame is
	 * dropped without leaking the buffer. */
	if (rc)
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
			rc = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
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
	if (id == WIFI_EVENT_STA_START) {
		enum s31_wifi_pending_operation pending = s31_wifi_pending;

		s31_wifi_pending = S31_WIFI_PENDING_NONE;
		s31_wifi_start_complete = 1;
		s31_key_sample_count = 0;
		s31_key_capture_enabled = true;
		rc = esp_wifi_internal_reg_netstack_buf_cb(
			s31_wifi_netstack_ref, s31_wifi_netstack_free);
		s31_linux_printf("[S31] Wi-Fi STA_START rc=%d pending=%u\n",
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

		/* Association traffic fills the small capture ring before Linux can
		 * submit DHCP.  Start a fresh window at the protected data boundary. */
		s31_tx_desc_sample_count = 0;
		s31_tx_desc_samples_dumped = false;
		s31_tx_desc_call_count = 0;
		s31_tx_desc_bad_eb_count = 0;
		s31_tx_desc_bad_desc_count = 0;
		s31_tx_desc_bad_frame_count = 0;
		s31_tx_desc_nondata_count = 0;
		s31_tx_desc_ipv4_submit_count = 0;
		s31_tx_desc_capture_enabled = true;
		s31_lmac_txerr_count = 0;
		s31_lmac_txerr_seckid_count = 0;
		s31_lmac_txdone_count = 0;
		s31_lmac_txdone_bad_count = 0;
		/* Match wifi_default_action_sta_connected(): the S31 station data
		 * interface becomes ready only after association, so registering at
		 * STA_START can return success without attaching the RX data path. */
		rc = esp_wifi_internal_reg_rxcb(WIFI_IF_STA, s31_wifi_rx);
		if (!rc)
			s31_wifi_rx_registered = 1;
		if (!rc)
			rc = esp_wifi_set_tx_done_cb(s31_wifi_tx_done);
		s31_linux_printf("[S31] RX slots ref=%p sta=%p free=%p expected=%p\n",
			       *(void * volatile *)0x2f07ff68,
			       *(void * volatile *)0x2f07ff6c,
			       *(void * volatile *)0x2f07ff78, s31_wifi_rx);
		s31_linux_printf("[S31] Wi-Fi STA connected channel=%u data-cb=%d\n",
			       event->channel, rc);
		s31_radio_wifi_connected(event->bssid, event->channel, rc);
	} else if (id == WIFI_EVENT_STA_DISCONNECTED) {
		wifi_event_sta_disconnected_t *event = event_data;

		s31_linux_printf("[S31] Wi-Fi STA disconnected reason=%u\n",
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
	if (params->has_password) {
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
	s31_linux_printf("[S31] Wi-Fi set_config rc=%d security=%s\n", rc,
		       params->has_password ? "WPA2/WPA3" :
		       params->has_psk ? "WPA2-PSK" : "open");
	if (!rc) {
		start = s31_wifi_start_operation(S31_WIFI_PENDING_CONNECT);
		if (start > 0)
			rc = esp_wifi_connect();
		else if (start < 0)
			rc = -start;
	}
	s31_linux_printf("[S31] Wi-Fi connect submit rc=%d\n", rc);
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
	bool ipv4 = length >= 14 && frame[12] == 0x08 && frame[13] == 0x00;
	int rc;
	uint32_t count = ++s31_wifi_tx_count;

	/* esp_wifi_internal_tx() only queues the Ethernet frame.  By the second
	 * DHCP retry, the first one has traversed the asynchronous Wi-Fi task and
	 * lmacTxFrame(), so dump that completed capture before queuing another. */
	if (ipv4)
		s31_tx_desc_ipv4_submit_count++;
	rc = esp_wifi_internal_tx(WIFI_IF_STA, frame, length);

	if (count <= 16 || rc) {
		if (length >= 14)
			s31_linux_printf("[S31] Wi-Fi TX #%u len=%u rc=%d dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x type=%02x%02x\n",
				       count, length, rc,
				       frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
				       frame[6], frame[7], frame[8], frame[9], frame[10], frame[11],
				       frame[12], frame[13]);
		else
				s31_linux_printf("[S31] Wi-Fi TX #%u len=%u rc=%d\n",
				       count, length, rc);
	}
	return rc;
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
	/* Keep blob logging out of the ROM UART busy-poll path: every ESP_LOG
	 * line serializes the gate hold behind 115200-baud output and was the
	 * measured cause of the multi-hundred-ms Wi-Fi queue-receive hold. */
	esp_log_level_set("*", ESP_LOG_WARN);
	/* ESP-IDF "max WiFi throughput" reference sizing, allocated in internal
	 * SRAM instead of PSRAM (AGENTS.md forbids blob buffers in PSRAM).  The
	 * reclaimed heap-low + heap2 chunks give the pool room; the zero-copy RX
	 * descriptor ring removed the 25.6 KiB data ring that previously forced
	 * the no-PSRAM minimums. */
	wifi_cfg.static_rx_buf_num = 32;
	wifi_cfg.dynamic_rx_buf_num = 32;
	/* TX buffer type/number follows sdkconfig.radio.defaults.  Static TX
	 * avoids per-frame alloc/free churn but 16 buffers was too small for the
	 * BT+WiFi ACK stream (esp_wifi_internal_tx rc=257); keep dynamic TX for
	 * now while the TX completion stall is debugged. */
	wifi_cfg.rx_ba_win = 32;
	/* TX_BA_WIN is a compile-time Kconfig, set via CONFIG_ESP_WIFI_TX_BA_WIN.
	 * Keep the TX completion path healthy: shrinking TX buffers to 8 stalled
	 * the download (rc=257) under ACK bursts. */
	rc = psa_crypto_init();
	s31_linux_printf("[S31] psa_crypto_init rc=%d\n", rc);
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
	g_wifi_osi_funcs._is_from_isr = s31_wifi_is_from_isr;
	/* Linux owns flash/MTD; do not let the IDF blob open its NVS backend. */
	wifi_cfg.nvs_enable = 0;
	/* Replace the loader/FreeRTOS callbacks retained by the COEX ROM. */
	rc = esp_coex_adapter_register(&g_coex_adapter_funcs);
	if (rc != 0) {
		s31_linux_printf("[S31] esp_coex_adapter_register rc=%d\n", rc);
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
	s31_linux_printf("[S31] coex_pre_init rc=%d\n", rc);
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
	s31_linux_printf("[S31] esp_wifi_init rc=%d\n", rc);
	#ifdef S31_LINUX_SMODE
	if (rc == 0)
		rc = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
						s31_wifi_event, NULL);
	#endif
	#ifdef S31_LINUX_SMODE
	s31_radio_report_wifi_init(rc);
	#endif
	if (rc == 0) {
	#ifndef S31_LINUX_SMODE
		rc = esp_wifi_set_mode(WIFI_MODE_STA);
		s31_linux_printf("[S31] esp_wifi_set_mode rc=%d\n", rc);
		if (rc == 0) {
			rc = esp_wifi_start();
			s31_linux_printf("[S31] esp_wifi_start rc=%d\n", rc);
		}
	#endif
	}

	if (rc != 0)
		return;

#ifndef S31_WIFI_ONLY
	/* The controller still needs an accurate low-power time base when modem
	 * sleep is disabled.  IDF's S31 Kconfig only exposes the LP-clock choice
	 * when CONFIG_BT_CTRL_SLEEP_ENABLE=y; otherwise btdm_lp_timer_clk_init()
	 * falls back to the system 136 kHz RC clock.  IDF explicitly documents
	 * that source as unable to reliably maintain ACL links.  Select the
	 * divided main XTAL while the controller is still IDLE, before init. */
	/* S31's current IDF BLE port fixes cfg->ble.rtc_freq at 32 kHz.  Feed
	 * that controller an equally exact 32 kHz clock from the 40 MHz main
	 * XTAL (divider 1250), rather than the inaccurate RC source or the
	 * common port's 100 kHz main-XTAL default. */
	btdm_lp_set_lpclk_freq(32000);
	btdm_lp_set_lpclk_src(MODEM_CLOCK_LPCLK_SRC_MAIN_XTAL);
	rc = esp_bt_controller_init(&bt_cfg);
	s31_linux_printf("[S31] esp_bt_controller_init rc=%d\n", rc);
	#ifdef S31_LINUX_SMODE
	s31_radio_report_bt_init(rc);
	#endif
	if (rc == 0) {
	#ifndef S31_LINUX_SMODE
		rc = esp_bt_controller_enable(BTDM_CONTROLLER_MODE_EFF);
		s31_linux_printf("[S31] esp_bt_controller_enable rc=%d\n", rc);
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
	s31_linux_printf("[S31] esp_bt_controller_enable rc=%d\n", rc);
	#ifdef S31_LINUX_SMODE
	if (rc == 0) {
		rc = esp_vhci_host_register_callback(&s31_vhci_callbacks);
		s31_linux_printf("[S31] esp_vhci_host_register_callback rc=%d\n", rc);
	}
	s31_radio_report_bt_enable(rc);
	s31_radio_heap_report("after-bt-enable");
	#endif
#endif
}
