/* SPDX-License-Identifier: BSD-2-Clause */
/* Private ABI recovered from IDF a602e67b, ESP32-S31 only. Included by radio_stack.c.
 * PP callbacks/submission use persistent CPU0 native radio owners. Never
 * retain eb after the callback; PP owns allocation after ppTxPkt on ALL returns.
 */
_Static_assert(sizeof(struct s31_softmac_tx_header) == 12, "raw TX bridge ABI");
_Static_assert(sizeof(struct s31_softmac_tx_status) == 8, "TX status bridge ABI");
_Static_assert(sizeof(struct s31_softmac_rx_header) == 4, "raw RX bridge ABI");
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
extern int s31_rtos_mutex_tx_credit_bind(void *handle);
extern void s31_rtos_tx_flight_return(void);
extern int s31_rtos_tx_pool_bind(void *counter);
extern uint8_t *g_lmac_cnt_ptr;
extern int s31_rtos_softmac_tx_copy(void *request);
extern void ic_ebuf_recycle_tx(void *eb);
_Static_assert(sizeof(void *) == 4, "PP descriptor ABI is RV32");
_Static_assert(sizeof(struct s31_softmac_rx_view) == 8, "synchronous RX view ABI");
_Static_assert(sizeof(((wifi_promiscuous_pkt_t *)0)->rx_ctrl) == 64,
	       "S31 MAC-v3 RX metadata ABI");
extern void *ic_ebuf_alloc(const void *frame, unsigned int kind, unsigned int len);
extern void *ic_get_default_sched(void);
/* Pinned trc.o: 14 entries of 12 bytes; LGI MCS7..0 occupy entries 1..8. */
extern uint8_t rc11NSchedTbl[14][12];
extern uint8_t rc11AXSchedTbl[16][12];
extern uint32_t hal_now(void);
extern int s31_sm_pp_submit(void *eb, int post);
extern int pp_post(unsigned int signal, uint32_t parameter);
extern void *xphyQueue;
extern BaseType_t s31_rtos_pp_notify_send(void *queue, const void *item);
extern int s31_rtos_pp_notify_capable(void);
extern void *g_intr_lock_mux;
extern uint8_t *pp_sig_cnt_ptr;
extern uint8_t *g_pm_ptr;
extern uint8_t *our_instances_ptr;

/* The private pinned PP submission copy protects its two list appends and
 * redirects its final pp_post tail call here. The SDK returns 1 both on recycle
 * and on a lost notification after linking EB into PP's queue. Preserve the
 * distinction without touching the SDK's other submission callers. */
__attribute__((noinline)) int s31_sm_pp_post_result(unsigned int signal,
                                                  uint32_t parameter)
{
	return pp_post(signal, parameter) ? 2 | (signal << 8) : 0;
}
static __attribute__((noinline)) void s31_sm_pp_recover(unsigned int result)
{
	/* pp_post already counted this event. Reposting through pp_post would
	 * suppress it. Wait for actual capacity only after releasing the Wi-Fi
	 * mutex; keep the descriptor/key flight pinned even if shutdown cancels
	 * the native wait. Stop/flush then joins the actual PP/hardware owner. */
	uint32_t event[2] = { result >> 8, 0 };
	if (s31_rtos_pp_notify_send(xphyQueue, event) != pdTRUE) {
		/* The failed raw send enqueued no event. pp_post's coalescing
		 * admission remains counted, and pp_stop_sw_txq does not clear it.
		 * Undo only this missing TX notification under pp_post's own IRQ
		 * lock, or a BA-only drain could suppress all later notifications.
		 * Signals0..4 coalesce at one (unlike counted signals6..8).
		 * ROM pp_sig_cnt_ptr is initialized to the linked pp_sig_cnt by
		 * pp_attach; use that binding rather than a relocated data address.
		 * Descriptor/key ownership is unchanged until the real PP drain. */
		if (event[0] <= 4) {
			uint32_t irq = g_osi_funcs_p->_wifi_int_disable(g_intr_lock_mux);
			if (pp_sig_cnt_ptr[event[0]] == 1)
				pp_sig_cnt_ptr[event[0]] = 0;
			g_osi_funcs_p->_wifi_int_restore(g_intr_lock_mux, irq);
		}

	}
}
extern int ppRegisterTxCallback(void (*cb)(void *), unsigned int index, unsigned int stage);
extern int pp_unregister_tx_cb(unsigned int index);
extern void ic_disable_crypto(unsigned int interface, unsigned int skip);
extern void *g_wifi_global_lock;
/* esp_wifi_driver.h's private wifi_ipc_config_t, RV32 ABI. Sync ioctls retain
 * their config pointer. Keep our callback descriptors for the payload's whole
 * lifetime so cancellation cannot expose a retired caller's stack to PP. */
struct s31_sm_wifi_ipc_config {
	int (*fn)(void *);
	void *arg;
	uint32_t arg_size;
};
_Static_assert(sizeof(struct s31_sm_wifi_ipc_config) == 12, "Wi-Fi IPC ABI");
typedef struct s31_sm_wifi_ipc_config s31_sm_wifi_ipc_t;
extern int esp_wifi_ipc_internal(struct s31_sm_wifi_ipc_config *, bool sync);
extern void pp_stop_sw_txq(void);
extern int lmac_stop_hw_txq(void);
/* Pinned hal_ampdu.o RV32 ABI: hardware slot, interface, TA, TID, SSN, window.
 * Native ic_add_rx_ba swaps its first two arguments before this leaf helper. */
extern void hal_agreement_add_rx_ba(unsigned, unsigned, const uint8_t *, unsigned, unsigned, unsigned);
extern void hal_agreement_del_rx_ba(unsigned);
extern void ic_set_ac_param(unsigned, unsigned, unsigned, unsigned, unsigned);
static uint32_t s31_sm_ba_mask;
extern void hal_crypto_set_key_entry(int, const void *, int, const void *);
extern void hal_crypto_enable(unsigned, unsigned, unsigned, unsigned);
static uint32_t s31_sm_key_mask, s31_sm_key_hw_mask, s31_sm_tx_key_mask;
static uint8_t s31_sm_tx_key_token;
static uint8_t s31_sm_key_ta[5][6], s31_sm_key_id[5];
static volatile uint32_t *s31_sm_key_valid(void)
{
	return (volatile uint32_t *)0x20104814U;
}
static void s31_sm_key_remove(unsigned int slot)
{
	unsigned int i;
	volatile uint32_t *entry;
	entry = (volatile uint32_t *)(0x20105800U + slot * 40);
	*s31_sm_key_valid() &= ~(1U << slot);
	for (i = 0; i < 10; i++) entry[i] = 0;
	s31_sm_key_mask &= ~(1U << slot);
	s31_sm_key_hw_mask &= ~(1U << slot);
	s31_sm_tx_key_mask &= ~(1U << slot);
	if (slot == 4) s31_sm_tx_key_token = 0;
	memset(s31_sm_key_ta[slot], 0, 6);
	s31_sm_key_id[slot] = 0;
}
static bool s31_sm_active;
/* mac80211 owns MLME. The SDK still needs its connected PM metadata and
 * beacon timing to arbitrate RF ownership and signal the AP during BT slots.
 * Scheduler bits alone leave its PM path disconnected and can starve RX. */
static bool s31_sm_coex_saved;
static uint32_t s31_sm_coex_old_interval, s31_sm_coex_old_status;
static uint32_t s31_sm_coex_state, s31_sm_coex_period;
static uint16_t s31_sm_coex_aid;
static uint8_t s31_sm_coex_bssid[6];
static void s31_sm_coex_pm_stop(void);

static void s31_sm_coex_apply(unsigned int state, uint32_t period)
{
 unsigned int status = state == 2 ? 4 : state == 3 ? 2 : state;
 if (state != 2) s31_sm_coex_pm_stop();
 coex_schm_status_bit_clear(COEX_SCHM_ST_TYPE_WIFI, 0xffff);
 coex_schm_status_bit_set(COEX_SCHM_ST_TYPE_WIFI, status);
 coex_schm_interval_set(state == 2 ? period / 100 : state == 3 ? 240 : 100);
}

static void s31_sm_coex_restore(void)
{
 if (!s31_sm_coex_saved) return;
 s31_sm_coex_pm_stop();
 coex_schm_status_bit_clear(COEX_SCHM_ST_TYPE_WIFI, 0xffff);
 coex_schm_status_bit_set(COEX_SCHM_ST_TYPE_WIFI, s31_sm_coex_old_status);
 coex_schm_interval_set(s31_sm_coex_old_interval);
 __atomic_store_n(&s31_sm_coex_saved, false, __ATOMIC_RELEASE);
 s31_sm_coex_state = s31_sm_coex_period = 0;
 s31_sm_coex_aid = 0;
 memset(s31_sm_coex_bssid, 0, sizeof(s31_sm_coex_bssid));
}

static void s31_sm_coex_refresh(void)
{
 /* coex_core_enable releases its core lock before invoking the Wi-Fi start
  * callback. Reuse the Wi-Fi resource mutex and recheck after taking it.
  * This restores mac80211's scheduler truth after native disconnected-PM
  * initialization, without a second coexistence restart or another worker. */
 if (!__atomic_load_n(&s31_sm_coex_saved, __ATOMIC_ACQUIRE)) return;
 g_osi_funcs_p->_mutex_lock(g_wifi_global_lock);
 if (s31_sm_coex_saved)
  s31_sm_coex_apply(s31_sm_coex_state, s31_sm_coex_period);
 g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
}

static int s31_sm_coex_bss(const struct s31_wifi_control *r)
{
 uint32_t period = (uint32_t)r->beacon_interval << 10;
 if (!s31_sm_active) return ESP_ERR_INVALID_STATE;
 if (r->field > 3 || (r->field == 2 &&
     (!r->beacon_interval || !r->offset || r->offset > 2007 ||
      (r->mac[0] & 1) || !memcmp(r->mac, "\0\0\0\0\0\0", 6))))
  return ESP_ERR_INVALID_ARG;
 g_osi_funcs_p->_mutex_lock(g_wifi_global_lock);
 if (!r->field) {
  s31_sm_coex_restore();
 } else if (!s31_sm_coex_saved || r->field != s31_sm_coex_state ||
            (r->field == 2 && (period != s31_sm_coex_period ||
             r->offset != s31_sm_coex_aid || memcmp(r->mac, s31_sm_coex_bssid, 6)))) {
  if (!s31_sm_coex_saved) {
   s31_sm_coex_old_interval = coex_schm_interval_get();
   s31_sm_coex_old_status = coex_schm_status_get(COEX_SCHM_ST_TYPE_WIFI);
   __atomic_store_n(&s31_sm_coex_saved, true, __ATOMIC_RELEASE);
  }
  /* Retire the old AP's timers before publishing a replacement identity. */
  s31_sm_coex_pm_stop();
  s31_sm_coex_state = r->field;
  s31_sm_coex_period = period;
  s31_sm_coex_aid = r->field == 2 ? r->offset : 0;
  memcpy(s31_sm_coex_bssid, r->mac, sizeof(s31_sm_coex_bssid));
  s31_sm_coex_apply(r->field, period);
 }
 g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
 return 0;
}
static bool s31_sm_rx_ra_filter;
static bool s31_sm_rx_error_filter;
static bool s31_sm_rx_bssid_filter;
static bool s31_sm_bssid_policy_active;
static uint8_t s31_sm_bssid_policy_mac[6];
#include "s31_softmac_coex_pm.inc"
/* Apply only the existing associated STA policy. The control path disables
 * it synchronously for scans/OTHER_BSS/beacon-promiscuous/disassociation. */
static bool s31_sm_bssid_should_drop(const uint8_t *frame,unsigned len,uint16_t fc)
{
 if (!s31_sm_rx_bssid_filter || len<24 ||
     !__atomic_load_n(&s31_sm_bssid_policy_active,__ATOMIC_ACQUIRE)) return false;
 if ((((fc&0xfc)==0x80 || (fc&0xfc)==0x50) &&
      memcmp(frame+16,s31_sm_bssid_policy_mac,6)) ||
     ((fc&12)==8 && (frame[4]&1) &&
      memcmp(frame+10,s31_sm_bssid_policy_mac,6))) {

  return true;
 }
 return false;
}

/* Pinned hal_mac/he leaf ABI. Association setup only; Linux owns MLME. */
extern void hal_mac_set_bssid(unsigned, const uint8_t *);
extern void hal_he_bssid_init(unsigned);
extern void hal_he_set_bss_color(unsigned, unsigned, unsigned, unsigned);
extern void hal_he_set_mmss_and_aid(unsigned, int, int, unsigned);
static bool s31_sm_he_saved;
static uint32_t s31_sm_he_regs_saved[6];
static volatile uint32_t *s31_sm_he_reg(unsigned i)
{
 static const uint32_t addr[] = { 0x20104000, 0x20104004, 0x20104020,
                                0x20104038, 0x2010403c, 0x201040a0 };
 return (volatile uint32_t *)(uintptr_t)addr[i];
}
static void s31_sm_he_restore(void)
{
 unsigned i;
 if (!s31_sm_he_saved) return;
 for (i = 0; i < 6; i++) *s31_sm_he_reg(i) = s31_sm_he_regs_saved[i];
 s31_sm_he_saved = false;
}
static int s31_sm_he_bss(const struct s31_wifi_control *r)
{
 unsigned i;
 if (!s31_sm_active) return ESP_ERR_INVALID_STATE;
 if (r->length > 1 || r->field > 2007 || r->offset > 63 ||
     r->total > 1 || r->hidden > 1 || (r->length &&
     (!r->field || (r->mac[0] & 1)))) return ESP_ERR_INVALID_ARG;
 g_osi_funcs_p->_mutex_lock(g_wifi_global_lock);
 if (!r->length) s31_sm_he_restore();
 else {
  if (!s31_sm_he_saved) {
   for (i = 0; i < 6; i++) s31_sm_he_regs_saved[i] = *s31_sm_he_reg(i);
   s31_sm_he_saved = true;
  }
  hal_mac_set_bssid(0, r->mac);
  hal_he_bssid_init(0);
  hal_he_set_bss_color(0, r->total, r->offset, r->hidden);
  hal_he_set_mmss_and_aid(0, 0, r->field, 0);
 }
 g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);

 return 0;
}
/* Pinned hal_rx_enable/disable_bssid_check: iface0 at 0x201040d8,
 * enable ORs value 2, disable clears value 2. Preserve all other bits. */
static bool s31_sm_bssid_filter_saved;
static uint32_t s31_sm_bssid_filter_original;
static volatile uint32_t *s31_sm_bssid_filter_reg(void)
{
 return (volatile uint32_t *)0x201040d8U;
}
static void s31_sm_bssid_filter_restore(void)
{
 __atomic_store_n(&s31_sm_bssid_policy_active,false,__ATOMIC_RELEASE);
 if (!s31_sm_bssid_filter_saved) return;
 *s31_sm_bssid_filter_reg()=s31_sm_bssid_filter_original;
 s31_sm_bssid_filter_saved=false;
}
static int s31_sm_bssid_filter(const struct s31_wifi_control *r)
{
 unsigned i;
 if (!s31_sm_active) return ESP_ERR_INVALID_STATE;
 if (r->field>1 || (r->field &&
     ((r->mac[0]&1) || !(r->mac[0]|r->mac[1]|r->mac[2]|r->mac[3]|r->mac[4]|r->mac[5]))))
  return ESP_ERR_INVALID_ARG;
 g_osi_funcs_p->_mutex_lock(g_wifi_global_lock);
 __atomic_store_n(&s31_sm_bssid_policy_active,false,__ATOMIC_RELEASE);
 if (!s31_sm_bssid_filter_saved) {
  s31_sm_bssid_filter_original=*s31_sm_bssid_filter_reg();
  s31_sm_bssid_filter_saved=true;
 }
 if (r->field) {
  if (!s31_sm_he_saved) {
   for (i=0;i<6;i++) s31_sm_he_regs_saved[i]=*s31_sm_he_reg(i);
   s31_sm_he_saved=true;
  }
  hal_mac_set_bssid(0,r->mac);
  *s31_sm_bssid_filter_reg()|=2U;
  memcpy(s31_sm_bssid_policy_mac,r->mac,6);
  __atomic_store_n(&s31_sm_bssid_policy_active,true,__ATOMIC_RELEASE);
 } else *s31_sm_bssid_filter_reg()&=~2U;

 g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
 return 0;
}
#define S31_SM_CB_INDEX 15U /* Inside the observed callback table, before +0x410. */
struct s31_sm_flight { void *eb; uint32_t cookie; };
static struct s31_sm_flight s31_sm_flights[S31_SOFTMAC_PENDING];
/* Native submission and PP completion can preempt one another on CPU0.
 * RV32's ordinary uint64_t RMW can lose a different flight's update. Use
 * one word AMO per changed slot rather than serializing vendor execution. */
static uint32_t s31_sm_mac_ccmp_flights[2];
static inline __attribute__((always_inline)) void s31_sm_key_pin_update(unsigned int slot,
								    bool pin)
{
	uint32_t bit = 1U << (slot & 31);
	uint32_t *word = &s31_sm_mac_ccmp_flights[slot >> 5];
	if (pin) __atomic_fetch_or(word, bit, __ATOMIC_RELEASE);
	else __atomic_fetch_and(word, ~bit, __ATOMIC_RELEASE);
}
static inline __attribute__((always_inline)) bool s31_sm_key_pins_any(void)
{
	/* Key mutation holds the Wi-Fi mutex, excluding new submissions. A
	 * concurrent completion only clears bits, so two word loads suffice. */
	return __atomic_load_n(&s31_sm_mac_ccmp_flights[0], __ATOMIC_ACQUIRE) ||
	       __atomic_load_n(&s31_sm_mac_ccmp_flights[1], __ATOMIC_ACQUIRE);
}

static inline __attribute__((always_inline)) void s31_sm_key_pins_reset(void)
{
	/* Only after producer join and PP/hardware drain. */
	__atomic_store_n(&s31_sm_mac_ccmp_flights[0], 0, __ATOMIC_RELEASE);
	__atomic_store_n(&s31_sm_mac_ccmp_flights[1], 0, __ATOMIC_RELEASE);
}
static bool s31_sm_cb_registered;
static uint32_t s31_sm_epoch;
static uint8_t s31_sm_mac[6];
static uint32_t s31_sm_rx_crypto_saved;
static volatile uint32_t *s31_sm_rx_crypto_reg(void)
{
	/* Pinned hal_crypto_init/enable/disable: STA crypto control. */
	return (volatile uint32_t *)0x20104800U;
}
/* Linux legacy rates: 1, 2, 5.5, 11, 6, 9, 12, 18, 24, 36, 48, 54 Mbps.
 * S31 PP/RX uses the native PHY rate encoding for legacy OFDM. */
static const uint8_t s31_sm_phy_rates[] = { 0, 1, 2, 3, 11, 15, 10, 14, 9, 13, 8, 12 };
static int s31_sm_rate_index(unsigned int rate)
{
	unsigned int i;
	for (i = 0; i < sizeof(s31_sm_phy_rates); i++)
		if (s31_sm_phy_rates[i] == rate) return i;
	return -1;
}

static uint32_t sm_get32(const void *p, unsigned int offset)
{
	uint32_t value;
	memcpy(&value, (const uint8_t *)p + offset, sizeof(value));
	return value;
}
static void sm_put32(void *p, unsigned int offset, uint32_t value)
{
	memcpy((uint8_t *)p + offset, &value, sizeof(value));
}

/* The PP consumer can preempt our lower-priority CPU0 submitter. Its empty
 * queue transition resets tail to &head, so publishing *tail and the new tail
 * must be indivisible against that consumer and IRQ-side vendor access.
 * Only the intrusive append is IRQ-fenced. Allocation, rate selection and the
 * potentially blocking pp_post remain outside this local critical section. */
void s31_sm_pp_enqueue(void *eb, unsigned int waiting)
{
	extern uint8_t *pTxRx;
	uint32_t irq = g_osi_funcs_p->_wifi_int_disable(g_intr_lock_mux);
	const void *meta = (const void *)(uintptr_t)sm_get32(eb, 52);
	uint32_t control = sm_get32(meta, 16);
	uint8_t *queue;
	unsigned int tail;

	if (waiting) {
		queue = pTxRx + 8U * (110U + ((control >> 18) & 3U));
		tail = 8;
	} else {
		queue = pTxRx + 52U * ((control >> 20) & 15U);
		tail = 36;
	}
	sm_put32(eb, 48, 0);
	sm_put32((void *)(uintptr_t)sm_get32(queue, tail), 0, (uintptr_t)eb);
	sm_put32(queue, tail, (uintptr_t)eb + 48U);
	g_osi_funcs_p->_wifi_int_restore(g_intr_lock_mux, irq);
}
static void sm_put16(void *p, unsigned int offset, uint16_t value)
{
	memcpy((uint8_t *)p + offset, &value, sizeof(value));
}
#include "s31_softmac_tx_reports.inc"
static int s31_sm_tx_drain(void);
#include "s31_softmac_tx_agg.inc"

static void s31_sm_tx_done(void *eb)
{
	unsigned int i;
	const uint8_t *meta = (const void *)(uintptr_t)sm_get32(eb, 0x34);

	for (i = 0; i < S31_SOFTMAC_PENDING; i++) {
		if (s31_sm_flights[i].eb == eb) {
			uint32_t cookie = s31_sm_flights[i].cookie;
			s31_sm_key_pin_update(i, false);

			int rate = s31_sm_rate_index(meta[0x0c]);
			if (meta[0x0c] >= 16 && meta[0x0c] <=
			    ((sm_get32(meta, 0) & (1U << 31)) ? 25 : 23)) {
				rate = ((sm_get32(meta, 0) & (1U << 31)) ?
				S31_SOFTMAC_TX_HE_RATE : S31_SOFTMAC_TX_HT_RATE) | (meta[0x0c] - 16);

			}
			s31_sm_report(cookie, meta[0x13] == 1, rate < 0 ? 0 : rate,
				      1U + meta[6] + meta[7]);
			/* Publish slot credit only after every old-slot access. This
			 * stage-0 callback does not establish EB/buffer recycle. */
			__atomic_store_n(&s31_sm_flights[i].eb, NULL, __ATOMIC_RELEASE);
			s31_rtos_tx_flight_return();
			break;
		}
	}
}
static void s31_sm_rx_frame(const wifi_promiscuous_pkt_t *packet,
                             const uint8_t *frame, wifi_promiscuous_pkt_type_t type)
{
	unsigned int sig_len = packet->rx_ctrl.sig_len;

	unsigned int len = sig_len >= 4 ? sig_len - 4 : 0;
	unsigned int rate = packet->rx_ctrl.rate;
	int rate_index = -1;
	uint8_t encoding = 0, phy_flags = 0;
	uint16_t fc = 0;

	if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
	/* Same STA unicast-RA rejection as the Linux frontend. Keep all groups,
	 * beacons, and probe responses, including during a software scan. This
	 * feature is negotiated only when Linux's RX filter is enabled. */
	if (s31_sm_active && s31_sm_rx_ra_filter && len >= 24 &&
	    !(frame[4] & 1) && memcmp(frame + 4, s31_sm_mac, 6)) {

		return;
	}
	if (len >= 24) memcpy(&fc, frame, sizeof(fc));
 if (s31_sm_active && s31_sm_bssid_should_drop(frame,len,fc)) return;
	if (type == WIFI_PKT_DATA && (fc & 12) != 8) return;
	/* Probe only; do not mark plaintext/MIC validation from this trace. */

	/* The public callback payload contract uses sig_len, including FCS.
	 * Pinned S31 hardware reports dump_len = sig_len + 4 despite the
	 * dump_len header comment. Monitor captures show those extra bytes
	 * beyond the complete MPDU. Never use dump_len as a copy bound. */
	if (packet->rx_ctrl.cur_bb_format == RX_BB_FORMAT_11B &&
	    (rate <= 3 || (rate >= 5 && rate <= 7)))
		rate_index = rate > 3 ? rate - 4 : rate;
	else if (packet->rx_ctrl.cur_bb_format == RX_BB_FORMAT_11G) {
		rate_index = s31_sm_rate_index(rate);
		if (rate_index < 4) rate_index = -1;
	}
	else if (packet->rx_ctrl.cur_bb_format == RX_BB_FORMAT_HT) {
		rate_index = packet->rx_ctrl.he_siga1 & 0x7f;
		if (rate_index > 7) rate_index = -1; /* Single spatial stream. */
		encoding = 1;
		phy_flags = ((packet->rx_ctrl.he_siga1 >> 31) & 1) |
			((packet->rx_ctrl.he_siga1 >> 6) & 2);
	} else if (packet->rx_ctrl.cur_bb_format == RX_BB_FORMAT_HE_SU) {
		unsigned int gi = (packet->rx_ctrl.he_siga1 >> 21) & 3;
		rate_index = (packet->rx_ctrl.he_siga1 >> 3) & 15;
		if (rate_index > 9) rate_index = -1;
		encoding = 2;
		phy_flags = (gi < 2 ? 0 : gi - 1) << 2;
	}
	/* Error MPDUs may contain only a partial hardware DMA payload. */
	if (!s31_sm_active || packet->rx_ctrl.rx_state ||
	    len < 24 || len > S31_SOFTMAC_FRAME_MAX ||
	    rate_index < 0)
		return;
	/* Native CCMP strips the 8-byte MIC but retains the CCMP IV/PN.
  * For this pinned ABI raw dump_len=sig_len+4, authenticated plaintext
  * dump_len=sig_len-4. Reject unrecognized layouts; never bypass Linux PN. */
 if (s31_sm_key_mask && (fc & 0x4000) && (fc & 0x000c) == 8) {
  unsigned int hdr = 24 + ((fc & 0x0080) ? 2 : 0);
  unsigned int slot;
  if ((fc & 0x0300) == 0x0300) return; /* No four-address mode. */
  if ((fc & 0x0080) && (fc & 0x8000)) hdr += 4;
  if (len < hdr + 8 + 8 || !(frame[hdr + 3] & 0x20)) return;
  slot = frame[4] & 1 ? frame[hdr + 3] >> 6 : 4;
  if ((s31_sm_key_hw_mask & (1U << slot))) {
   if (packet->rx_ctrl.rxend_state || !packet->rx_ctrl.rxmatch0 ||
       packet->rx_ctrl.dump_len + 4U != sig_len ||
       s31_sm_key_id[slot] != frame[hdr + 3] >> 6 ||
       memcmp(frame + 10, s31_sm_key_ta[slot], 6) ||
       (!(frame[4] & 1) && memcmp(frame + 4, s31_sm_mac, 6))) return;
   len -= 8;
   phy_flags |= S31_SOFTMAC_RX_CCMP_VALID;
  }
 }
 /* Valid management frames only; data packets do not enter the PM adapter. */
 if ((fc & 0x00fc) == 0x0080 && len >= 36)
  s31_sm_coex_beacon(frame);
 {
  const struct s31_softmac_rx_view view = {
   .header = { .rate_index = rate_index, .encoding = encoding, .flags = phy_flags },
   .frame = frame,
  };
  s31_radio_wifi_receive_aux(S31_WIFI_IF_SOFTMAC_VIEW, (const void *)&view,
                            len, packet->rx_ctrl.channel, packet->rx_ctrl.rssi);
 }
}
static void s31_sm_rx(void *buffer, wifi_promiscuous_pkt_type_t type)
{
 const wifi_promiscuous_pkt_t *packet = buffer;
 if (type == WIFI_PKT_CTRL) {
  return; /* native hardware owns BA; never forward a duplicate completion */
 }
 s31_sm_rx_frame(packet, packet->payload, type);
}
/* Pinned pp_wdev_funcs+484 callback: EB->descriptor->64-byte RX metadata.
 * ROM retains EB and DMA buffer ownership. Consume only synchronously under
 * the existing radio ISR gate, queue Linux NAPI, and return without freeing.
 * This removes SnifferRxAmpdu's two allocations/copy/PP task round-trip. */
extern void wDev_SnifferRxAmpdu(void *);
static void (*s31_sm_sniffer_saved)(void *);
static void (**s31_sm_sniffer_slot(void))(void *)
{
 extern void *pp_wdev_funcs;
 return (void (**)(void *))((uint8_t *)pp_wdev_funcs + 484);
}
static void s31_sm_rx_direct(void *eb)
{
 const wifi_promiscuous_pkt_t *packet;
 uintptr_t desc, buf;
 if (!s31_sm_active) {
  if (s31_sm_sniffer_saved) s31_sm_sniffer_saved(eb);
  return;
 }
 if (!eb) return;
 desc = sm_get32(eb, 4);
 if (!desc || (desc & 3)) return;
 buf = sm_get32((void *)desc, 4);
 if (!buf || (buf & 3)) return;
 packet = (const void *)buf;
 s31_sm_rx((void *)packet, WIFI_PKT_DATA);
}
/* The S31 ROM normal RX path uses SnifferRxData (table+504), including
 * HT/HE data. Its first descriptor comes from wDevCtrl, and a0 is the last.
 * Fast path only for a single DMA buffer; scatter/CSI/error cases retain the
 * original implementation. The hardware metadata is 56 bytes plus an 8-byte
 * subframe header at get_sublen_offset(). Neither descriptor nor DMA is freed. */
extern void wDev_SnifferRxData(void *, unsigned, unsigned);
extern unsigned get_sublen_offset(void *, uint16_t *);
static void (*s31_sm_data_saved)(void *, unsigned, unsigned);
static void (**s31_sm_data_slot(void))(void *, unsigned, unsigned)
{
 extern void *pp_wdev_funcs;
 return (void (**)(void *, unsigned, unsigned))((uint8_t *)pp_wdev_funcs + 504);
}
static void *s31_sm_rx_head(void)
{
 extern void *wDevCtrl_ptr;
 return *(void **)wDevCtrl_ptr;
}
_Static_assert(WIFI_PKT_MGMT == 0 && WIFI_PKT_CTRL == 1 &&
               WIFI_PKT_DATA == 2 && WIFI_PKT_MISC == 3, "pinned RX callback ABI");
static void s31_sm_data_direct(void *last, unsigned count, unsigned state)
{
 void *head = s31_sm_rx_head();
 uint8_t *buf;
 unsigned bytes, off, need;
 uint16_t extra = 0, fc;
 typeof(((wifi_promiscuous_pkt_t *)0)->rx_ctrl) ctrl;
 if (!s31_sm_active) { goto fallback; }
 if (count > 1 || head != last || !head) { goto fallback; }
 if ((state & 0xf8) == 0x78) { goto fallback; }
 buf = (void *)(uintptr_t)sm_get32(head, 4);
 bytes = (sm_get32(head, 0) >> 14) & 0x3fff;
 if (!buf || bytes < 64) { goto fallback; }
 off = get_sublen_offset(buf, &extra) & 0xffff;
 if (off < 56 || off > bytes - 8 || bytes - off - 8 < 24) { goto fallback; }
 memcpy(&ctrl, buf, 56);
 memcpy((uint8_t *)&ctrl + 56, buf + off, 8);
 /* The frontend rejects every nonzero rx_state already. Reject it before
  * the original sniffer allocates/queues a partial payload; the ROM caller
  * still owns descriptor recycling, exactly as on the valid direct path. */
 if (s31_sm_rx_error_filter && ctrl.rx_state) {

  return;
 }
 memcpy(&fc, buf + off + 8, sizeof(fc));
 if (((fc & 12) != 8 && (fc & 12) != 0) || ctrl.sig_len < 28 || ctrl.sig_len > S31_SOFTMAC_FRAME_MAX+4)
  { goto fallback; }
 need = ctrl.sig_len - 4;
 if (s31_sm_key_hw_mask && !ctrl.rx_state && !ctrl.rxend_state &&
     ctrl.dump_len + 4U == ctrl.sig_len) need -= 8;
 if (need > bytes - off - 8) { goto fallback; }

 s31_sm_rx_frame((const void *)&ctrl, buf + off + 8, (fc & 12) ? WIFI_PKT_DATA : WIFI_PKT_MGMT);
 return;
fallback:
 if (s31_sm_data_saved) s31_sm_data_saved(last, count, state);
}
static int s31_sm_sniffer_install(void)
{
 void (**slot)(void *) = s31_sm_sniffer_slot();
 if (*slot != wDev_SnifferRxAmpdu || *s31_sm_data_slot() != wDev_SnifferRxData)
  return ESP_ERR_INVALID_STATE;

 s31_sm_data_saved = *s31_sm_data_slot();
 *s31_sm_data_slot() = s31_sm_data_direct;
 s31_sm_sniffer_saved = *slot;
 *slot = s31_sm_rx_direct;
 return 0;
}
static void s31_sm_sniffer_restore(void)
{

 if (s31_sm_data_saved) {
  *s31_sm_data_slot() = s31_sm_data_saved;
  s31_sm_data_saved = NULL;
 }
 if (s31_sm_sniffer_saved) {
  *s31_sm_sniffer_slot() = s31_sm_sniffer_saved;
  s31_sm_sniffer_saved = NULL;
 }
}
/* Only the persistent CPU0 submitter opens this bounded Wi-Fi resource
 * scope. It is an ordinary recursive native mutex lease, never a vendor
 * execution gate: radio IRQs and other tasks remain preemptible. */
/* The single kernel submitter owns begin/end and its bounded pass budget.
 * The native recursive mutex already owns task identity and validates unlock;
 * duplicating its owner and budget here adds no lifetime protection. */
static int s31_sm_tx_batch_begin(void)
{
    /* Never wait behind a control operation which itself is joining PP. */
    return xSemaphoreTakeRecursive(g_wifi_global_lock, 0) ?
        0 : S31_WIFI_TX_RETRY_LOCK;
}
static int s31_sm_tx_batch_end(void)
{
    g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
    return 0;
}
static int s31_sm_send(const struct s31_softmac_tx_view *view, uint16_t view_size)
{
	struct s31_softmac_tx_header header;
	const uint8_t *frame = view->prefix;
	uint8_t *eb, *meta, *desc;
	unsigned int i, len, hdrlen, flight_limit;
	uint16_t fc;
	bool qos;
	bool ht, he;
	bool ampdu;
	bool mac_ccmp;
	int rc;

	if (view_size != sizeof(*view) || view->length < 24 ||
	    view->length > S31_SOFTMAC_FRAME_MAX || !view->token)
		return ESP_ERR_INVALID_ARG;
	header = view->header;
	ht = header.reserved[0] == 1 || header.reserved[0] == 2;
	he = header.reserved[0] == 2;
	ampdu = header.reserved[1] == 1;
	mac_ccmp = header.reserved[2] != 0;
	if (header.reserved[0] > 2 || header.reserved[1] > 1 ||
	    (mac_ccmp && !(header.reserved[2] & S31_SOFTMAC_TX_MAC_CCMP)) ||
	    header.rate_index >= (he ? 10 : ht ? 8 : sizeof(s31_sm_phy_rates)))
		goto rejected;
	if (!s31_sm_active || header.epoch != s31_sm_epoch) {
		s31_sm_report(header.cookie, false, 0, 1);
		return 0;
	}
	len = view->length;
	memcpy(&fc, frame, sizeof(fc));
	if (ht && ((fc & 0x000c) != 8 || (frame[4] & 1))) goto rejected;
	qos = (fc & 0x000c) == 8 && (fc & 0x0080);
	/* Management or three-address data, including software CCMP ciphertext. */
	if ((fc & 0x000c) != 0 && (fc & 0x000c) != 8)
		goto rejected;
	if (((fc & 0x4000) && ((fc & 0x000c) != 8 || len < 40)) ||
	    (fc & 0x0400) || (frame[22] & 15) || (fc & 0x0300) == 0x0300 ||
	    (fc & 0x8000 && !(fc & 0x0080)))
		goto rejected;
	hdrlen = 24 + (qos ? 2 : 0) + ((qos && (fc & 0x8000)) ? 4 : 0);
	if (len < hdrlen || ((fc & 0x4000) && len < hdrlen + (mac_ccmp ? 8 : 16))) goto rejected;
	if (mac_ccmp && (!(fc & 0x4000) || (frame[4] & 1) ||
	    (fc & 0x0300) != 0x0100 || frame[hdrlen + 2] ||
	    (frame[hdrlen + 3] & 0x3f) != 0x20)) goto rejected;
	if (ampdu && (!ht || !qos || (fc & 0x0300) != 0x0100 ||
	    (frame[24] & 15) || !s31_sm_tx_peer || !s31_sm_tx_ba ||
	    header.rate_index != s31_sm_tx_mcs ||
	    memcmp(frame + 4, s31_sm_tx_ap, 6))) goto rejected;
	/* The bounded submit scope retains the Wi-Fi resource lock here. */
	/* HE is limited to the negotiated native BA path. Retiring the BSS or
	 * a different peer must never turn an already queued HT frame into HE. */
	if ((ampdu && he != s31_sm_tx_he) ||
	    (he && (!ampdu || !s31_sm_tx_he || !s31_sm_he_saved))) {
		goto rejected;
	}
	/* Key changes share this lock. Do not send plaintext after a removal,
	 * mismatch or stale submission, even if mac80211 already allocated PN. */
	if (mac_ccmp && (!(s31_sm_tx_key_mask & (1U << 4)) ||
	    header.reserved[2] != s31_sm_tx_key_token ||
	    !(*s31_sm_key_valid() & (1U << 4)) ||
	    memcmp(frame + 4, s31_sm_key_ta[4], 6) ||
	    (frame[hdrlen + 3] >> 6) != s31_sm_key_id[4])) {
		goto rejected;
	}
	flight_limit = S31_SOFTMAC_PENDING;
	for (i = 0; i < flight_limit; i++)
		if (!__atomic_load_n(&s31_sm_flights[i].eb, __ATOMIC_ACQUIRE))
			break;
	if (i == flight_limit) {
		/* No cookie/descriptor was submitted. Preserve the Linux staging
		 * slot until an in-flight completion makes room. */
		return S31_WIFI_TX_RETRY_FLIGHT;
	}
	/* Class 1 and metadata follow esp_wifi_80211_tx/output_raw_process. */
	/* Live pp_wdev_funcs+392 resolves to ROM esf_buf_alloc. Both its
	 * static path and dynamic delegate explicitly skip memcpy for NULL.
	 * The returned descriptor is private until ppTxPkt below. */
	eb = ic_ebuf_alloc(NULL, 1, len);
	if (!eb) {
		/* Internal DMA fragmentation/pressure is retryable. A terminal
		 * status here falsely consumes a frame which never reached PP. */
		return ESP_ERR_NO_MEM;
	}
	meta = (void *)(uintptr_t)sm_get32(eb, 0x34);
	desc = (void *)(uintptr_t)sm_get32(eb, 4);
	{
		struct s31_softmac_tx_copy copy = {
			.token = view->token,
			.destination = sm_get32(desc, 4),
			.length = len,
		};
		if (!s31_rtos_softmac_tx_copy(&copy)) {
			/* PP has never seen this descriptor. Only this failure path
			 * returns our private reservation through the real recycler. */
			ic_ebuf_recycle_tx(eb);
			goto rejected;
		}
		frame = (const void *)(uintptr_t)copy.destination;
	}
	sm_put16(eb, 0x14, hdrlen);
	sm_put16(eb, 0x16, len - hdrlen);
	sm_put32(desc, 0, (sm_get32(desc, 0) & ~0x2fffc000U) | 0xc0000000U |
		(mac_ccmp ? 0x20000000U : 0U) | (len << 14));
	/* Preserve mac80211 sequence: clear IDF's en_sys_seq flag. */
	sm_put32(meta, 0, (sm_get32(meta, 0) & ~1U) | 0x4000U |
				 ((frame[4] & 1) ? 0x402U : 0U));
	/* PP validates the high-nibble AC against the low-nibble TID. */
	{
		unsigned int tid = qos ? frame[24] & 15 : 7;
		unsigned int ac = tid > 7 ? 4 : (tid == 0 || tid == 3) ? 2 :
			(tid < 3) ? 3 : (tid < 6) ? 1 : 0;
		meta[4] = tid | (ac << 4);
	}
	meta[0x0c] = ht ? 16 + header.rate_index : s31_sm_phy_rates[header.rate_index];
	if (ht) {
		/* HE-SU selects the native HE length/HESIG path through bit31.
		 * Its format6/GI2 metadata encodes 1.6us. HT keeps its old ABI. */
		sm_put32(meta, 0, (sm_get32(meta, 0) & ~(1U << 22)) | (1U << 25));
		sm_put32(meta, 8, sm_get32(meta, 8) & ~(1U << 15));
		if (he) {
			sm_put32(meta, 0, sm_get32(meta, 0) | (1U << 31));
			meta[47] = (meta[47] & ~0x7bU) | 0x32U;
		} else {
			sm_put32(meta, 0, sm_get32(meta, 0) & ~(1U << 31));
			meta[47] &= ~0x80U;
		}
	}
	/* Pinned PP cipher 3 adds 12 air bytes (MIC 8 + FCS 4), but no input
	 * bytes. Linux has supplied the eight-byte CCMP header including PN. */
	/* ieee80211_encap_esfbuf also writes the hardware slot into meta[16].
	 * mac_tx_set_plcp1 copies this byte into PLCP1 bits17..24; RX key RA
	 * matching alone does not select a TX key. STA slot 4 has no AP/NAN bits. */
	sm_put32(meta, 0x10, (sm_get32(meta, 0x10) &
		~((3U << 18) | 0xf00U | (mac_ccmp ? 0xffU : 0U))) |
		(mac_ccmp ? 0x304U : 0U));
	sm_put32(meta, 0x14, 1U << S31_SM_CB_INDEX);
	sm_put32(meta, 0x18, hal_now());
	sm_put32(meta, 0x1c, (uintptr_t)(he ? rc11AXSchedTbl[10 - header.rate_index] :
		(ht ? rc11NSchedTbl[8 - header.rate_index] : ic_get_default_sched())));
	if (ampdu) s31_sm_tx_agg_prepare(eb, len, hdrlen);
	s31_sm_flights[i] = (struct s31_sm_flight) { .eb = eb, .cookie = header.cookie };
	s31_sm_key_pin_update(i, mac_ccmp);
	if (ampdu) s31_sm_tx_agg_flights |= UINT64_C(1) << i;
	else s31_sm_tx_agg_flights &= ~(UINT64_C(1) << i);

	rc = s31_sm_pp_submit(eb, 1);
	/* Only 1 establishes rejection/recycle. A tagged notification failure
	 * still belongs to PP, including its key pin and completion ownership. */
	if (rc == 1 && s31_sm_flights[i].eb == eb) {
		s31_sm_key_pin_update(i, false);
		s31_sm_report(header.cookie, false, 0, 1);
		__atomic_store_n(&s31_sm_flights[i].eb, NULL, __ATOMIC_RELEASE);
		s31_rtos_tx_flight_return();
	}
	if (rc & 2) {
		/* Recovery may sleep on PP queue capacity. Close the resource scope
		 * first, retaining each descriptor/key flight and its source lease.
		 * Tell Linux this accepted frame ended the current batch. */
		if (s31_sm_tx_batch_end())
			return ESP_ERR_INVALID_STATE;
		s31_sm_pp_recover((unsigned int)rc);
		return S31_WIFI_TX_BATCH_CLOSED;
	}
	return 0;
rejected:
	s31_sm_report(header.cookie, false, 0, 1);
	return 0;
}
static __attribute__((noinline)) int s31_sm_tx_drain_in_pp(void *unused)
{
	uint32_t cancelled[S31_SOFTMAC_PENDING];
	unsigned int i, n = 0;
	(void)unused;
	/* Linux closed and joined source submissions before this synchronous
	 * PP ioctl. Running on PP itself excludes its queue consumer without a
	 * global execution gate. Keep hardware key pins through actual recycle
	 * and hardware stop, including the three interface waiting lists and
	 * TX-done list which a single ppClearTxq(qid) does not drain. */
	g_osi_funcs_p->_mutex_lock(g_wifi_global_lock);
	for (i = 0; i < S31_SOFTMAC_PENDING; i++)
		if (s31_sm_flights[i].eb) {
			cancelled[n++] = s31_sm_flights[i].cookie;
			/* Direct recycling does not promise our completion callback.
			 * Prevent a late callback from reporting the same cookie twice;
			 * the separate key bitmap stays pinned until both drains return. */
			__atomic_store_n(&s31_sm_flights[i].eb, NULL, __ATOMIC_RELEASE);
		}
	ic_ampdu_op(0, 0, 0, 0);
	s31_sm_tx_ba = false;
	/* Match wifi_hw_stop's software-then-hardware order. The hardware helper
	 * checks live LMAC instances, avoiding stale last-EB dereferences on an
	 * idle queue. Both helpers are the actual linked SDK implementations. */
	pp_stop_sw_txq();
	lmac_stop_hw_txq();
	s31_sm_key_pins_reset();
	s31_sm_tx_agg_flights = 0;
	for (i = 0; i < n; i++)
		s31_sm_report(cancelled[i], false, 0, 1);
	if (n) s31_rtos_tx_flight_return();
	g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);

	return 0;
}
static int s31_sm_tx_drain(void)
{
	static struct s31_sm_wifi_ipc_config drain = { .fn = s31_sm_tx_drain_in_pp };
	return esp_wifi_ipc_internal(&drain, true);
}
static __attribute__((noinline)) int s31_sm_stopped_notifications_in_pp(void *unused)
{
	uint32_t irq;
	unsigned int i, count;
	(void)unused;
	/* Only after STA_STOP: wifi_hw_stop disabled the chip and cleared its
	 * interrupt handlers. This FIFO PP ioctl also joins previously queued
	 * completion notifications. Never clear a coalescing count while a TX
	 * producer or an older queued token could still consume that count. */
	if (s31_sm_active || s31_wifi_start_requested || s31_sm_stop_pending ||
	    s31_sm_key_pins_any() || s31_sm_tx_agg_flights)
		return ESP_ERR_INVALID_STATE;
	for (i = 0; i < S31_SOFTMAC_PENDING; i++)
		if (s31_sm_flights[i].eb) return ESP_ERR_INVALID_STATE;
	irq = g_osi_funcs_p->_wifi_int_disable(g_intr_lock_mux);
	count = pp_sig_cnt_ptr[16];
	/* Signal16 is coalesced, not a counted6..8 event. A missing queue send
	 * leaves one behind even after real SW/HW retirement and esp_wifi_stop. */
	if (count <= 1) pp_sig_cnt_ptr[16] = 0;
	g_osi_funcs_p->_wifi_int_restore(g_intr_lock_mux, irq);
	if (count > 1) return ESP_ERR_INVALID_STATE;

	return 0;
}
static __attribute__((noinline)) int s31_sm_stop_in_pp(void *unused)
{
	unsigned int i;
	int rc;
	s31_sm_tx_drain_in_pp(unused);
	g_osi_funcs_p->_mutex_lock(g_wifi_global_lock);
	if (s31_sm_tx_peer) {
		struct s31_wifi_control tx_stop = { .operation = S31_WIFI_SOFTMAC_TX_PEER_DEL };
		memcpy(tx_stop.mac, s31_sm_tx_ap, 6);
		rc = s31_sm_tx_ba_control(&tx_stop);
		if (rc) {
			g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
			return rc;
		}
	}
	for (i = 0; i < 4; i++) if (s31_sm_ba_mask & (1U << i)) hal_agreement_del_rx_ba(i);
	s31_sm_ba_mask = 0;
	s31_sm_he_restore();
	s31_sm_coex_restore();
	s31_sm_bssid_filter_restore();
	s31_sm_sniffer_restore();
	for (i = 0; i < 5; i++) if (s31_sm_key_mask & (1U << i)) s31_sm_key_remove(i);
	g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
	return 0;
}
static int s31_sm_stop(void)
{
	int rc;
	bool started = s31_wifi_start_requested || s31_sm_active;
	if (s31_sm_active) {
		static struct s31_sm_wifi_ipc_config drain = { .fn = s31_sm_stop_in_pp };
		/* Never wait for PP while holding the Wi-Fi resource mutex. */
		rc = esp_wifi_ipc_internal(&drain, true);
		if (rc) return rc;
	}

	if (s31_sm_active)
		*s31_sm_rx_crypto_reg() = s31_sm_rx_crypto_saved;
	s31_sm_active = false;
	s31_sm_rx_ra_filter = false;
 s31_sm_rx_error_filter = false;
 s31_sm_rx_bssid_filter = false;
	esp_wifi_set_promiscuous(false);
	esp_wifi_set_promiscuous_rx_cb(NULL);
	s31_sm_start_waiting = false;
	if (started) {
		xEventGroupClearBits(s31_wifi_lifecycle_events, S31_WIFI_EVENT_STOPPED);
		s31_sm_stop_pending = true;
	}
	rc = esp_wifi_stop();
	if (!rc && started && !(xEventGroupWaitBits(s31_wifi_lifecycle_events,
			S31_WIFI_EVENT_STOPPED, pdTRUE, pdFALSE,
			pdMS_TO_TICKS(2000)) & S31_WIFI_EVENT_STOPPED))
		rc = ESP_ERR_TIMEOUT;
	if (!rc && started) {
		static struct s31_sm_wifi_ipc_config stopped = {
			.fn = s31_sm_stopped_notifications_in_pp,
		};
		rc = esp_wifi_ipc_internal(&stopped, true);
	}
	if (!rc) {
		s31_rtos_tx_pool_bind(NULL);
		s31_wifi_start_requested = 0;
		s31_wifi_start_complete = 0;
		memset(s31_sm_flights, 0, sizeof(s31_sm_flights));
		s31_sm_key_pins_reset();
		s31_sm_tx_peer = NULL;
		s31_sm_tx_agg_flights = 0;
		s31_sm_tx_ba = false;
		s31_sm_tx_he = false;
		memset(s31_sm_tx_ap, 0, sizeof(s31_sm_tx_ap));
	}
	return rc;
}

static int s31_sm_start(const struct s31_wifi_control *request)
{
	wifi_promiscuous_filter_t filter = {
		.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MISC,
	};
	int rc;

	if (s31_sm_active || s31_wifi_start_requested || s31_sm_stop_pending)
		return ESP_ERR_INVALID_STATE;
	if (s31_rtos_softmac_tx_copy(NULL) != S31_SOFTMAC_TX_BATCH_ABI)
		return ESP_ERR_NOT_SUPPORTED;
	/* Refuse an older kernel adapter before enabling the new wait reason. */
	if (!s31_rtos_pp_notify_capable())
		return ESP_ERR_NOT_SUPPORTED;
	/* The native-only matched frontend consumes borrowed RX views before
	 * return. Reject an old start contract before touching hardware rather
	 * than retaining a second full-frame staging allocation/copy path. */
	if (!(request->field & S31_SOFTMAC_START_RX_VIEW))
		return ESP_ERR_NOT_SUPPORTED;
	if (request->channel < 1 || request->channel > 11)
		return ESP_ERR_INVALID_ARG;
	/* ROM esf_buf_recycle(type1): 12-byte record, recycle word at +4. */
	if (!s31_rtos_tx_pool_bind(g_lmac_cnt_ptr + 12 + 4))
		return ESP_ERR_NOT_SUPPORTED;
	if (!s31_rtos_mutex_tx_credit_bind(g_wifi_global_lock))
		return ESP_ERR_INVALID_STATE;
	rc = esp_wifi_set_mac(WIFI_IF_STA, request->mac);
	if (!rc) rc = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
		((request->field & 1) ? WIFI_PROTOCOL_11N : 0) |
		((request->field & 2) ? WIFI_PROTOCOL_11AX : 0));
	if (!rc) rc = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
	if (!rc) rc = esp_wifi_set_ps(WIFI_PS_NONE);

	if (!rc && !s31_sm_cb_registered) {
		rc = ppRegisterTxCallback(s31_sm_tx_done, S31_SM_CB_INDEX, 0);

		if (!rc) s31_sm_cb_registered = true;
	}
	if (!rc) {
		xEventGroupClearBits(s31_wifi_lifecycle_events, S31_WIFI_EVENT_STARTED);
		s31_wifi_start_complete = 0;
		s31_wifi_start_requested = 1;
		s31_sm_start_waiting = true;
		rc = esp_wifi_start();
		if (rc) {
			s31_wifi_start_requested = 0;
			s31_sm_start_waiting = false;
		}
	}

	if (!rc) {
		if (!(xEventGroupWaitBits(s31_wifi_lifecycle_events,
				S31_WIFI_EVENT_STARTED, pdTRUE, pdFALSE,
				pdMS_TO_TICKS(2000)) & S31_WIFI_EVENT_STARTED))
			rc = ESP_ERR_TIMEOUT;
		s31_sm_start_waiting = false;
	}
	if (!rc) rc = esp_wifi_set_channel(request->channel, WIFI_SECOND_CHAN_NONE);
	if (!rc) rc = esp_wifi_set_promiscuous_filter(&filter);
	if (!rc) rc = esp_wifi_set_promiscuous_rx_cb(s31_sm_rx);
	if (!rc) rc = esp_wifi_set_promiscuous(true);
	if (!rc) {
		/* Linux supplies ciphertext and owns all keys. Disable native STA
		 * crypto as well as the per-TX cipher selector (pinned private ABI). */
		g_osi_funcs_p->_mutex_lock(g_wifi_global_lock);
		ic_disable_crypto(WIFI_IF_STA, 0);
		/* Native "disable" writes 0x30000, leaving RX security enabled.
		 * Clear RX data/mgmt security enables so own unicast ciphertext is
		 * DMA'd in full instead of being truncated on hardware key miss.
		 * Restore the previous control word on stop; no native keys installed. */
		s31_sm_rx_crypto_saved = *s31_sm_rx_crypto_reg();
		*s31_sm_rx_crypto_reg() = s31_sm_rx_crypto_saved & ~0x30000U;
		if (request->total) rc = s31_sm_sniffer_install();
		g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
	}
	if (!rc) {
		s31_sm_rx_ra_filter = !!(request->field & S31_SOFTMAC_START_RX_RA_FILTER);

  s31_sm_rx_error_filter = !!(request->field & S31_SOFTMAC_START_RX_DROP_ERRORS);
  s31_sm_rx_bssid_filter = !!(request->field & S31_SOFTMAC_START_RX_BSSID_FILTER);

		s31_sm_epoch = request->offset;
		memcpy(s31_sm_mac, request->mac, sizeof(s31_sm_mac));
		s31_sm_active = true;
	}
	else if (s31_wifi_start_requested) s31_sm_stop();

	return rc;
}
static int s31_sm_rx_ba(const struct s31_wifi_control *request)
{
	unsigned int slot = request->field;
	if (!s31_sm_active || slot >= 4) return ESP_ERR_INVALID_STATE;
	if (request->operation == S31_WIFI_SOFTMAC_RX_BA_ADD &&
	    (request->total > 15 || request->offset > 4095 ||
	     !request->length || request->length > 64 || (request->mac[0] & 1)))
		return ESP_ERR_INVALID_ARG;
	g_osi_funcs_p->_mutex_lock(g_wifi_global_lock);
	if (request->operation == S31_WIFI_SOFTMAC_RX_BA_ADD) {
		hal_agreement_add_rx_ba(slot, 0, request->mac, request->total,
			request->offset, request->length);
		s31_sm_ba_mask |= 1U << slot;
	} else {
		hal_agreement_del_rx_ba(slot);
		s31_sm_ba_mask &= ~(1U << slot);
	}
	g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);

	return 0;
}
static int s31_sm_edca(const struct s31_wifi_control *request)
{
	if (!s31_sm_active || request->field > 3 || request->offset > 15 ||
	    request->total > 10 || request->length > 10 ||
	    request->total > request->length) return ESP_ERR_INVALID_ARG;
	g_osi_funcs_p->_mutex_lock(g_wifi_global_lock);
	ic_set_ac_param(request->field, request->offset, request->total,
		request->length, request->beacon_interval);
	g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
	return 0;
}
static int s31_sm_rx_key(const struct s31_wifi_control *request)
{
	unsigned int slot = request->field;
	uint32_t key[4];
	unsigned int i;
	/* Private net80211 cipher numbering, NOT public wifi_cipher_type_t.
     * IDF esp_nan_supplicant.c defines NAN_PASN_WIFI_ALG_CCMP=3. */
 uint8_t info[9] = {0, 3, 0};
	if (!s31_sm_active) return ESP_ERR_INVALID_STATE;
	if (slot > 4 || (request->operation != S31_WIFI_SOFTMAC_RX_KEY_ADD &&
	    request->operation != S31_WIFI_SOFTMAC_RX_KEY_DEL)) return ESP_ERR_INVALID_ARG;
	if (request->offset > 3 || request->length != 16 || request->total > 3 ||
	    ((request->total & S31_SOFTMAC_KEY_TX_CCMP) &&
	     (slot != 4 || !(request->total & S31_SOFTMAC_KEY_RX_CCMP) ||
	      !request->beacon_interval || request->beacon_interval > 255 ||
	      !(request->beacon_interval & S31_SOFTMAC_TX_MAC_CCMP))) || (request->mac[0] & 1))
		return ESP_ERR_INVALID_ARG;
	g_osi_funcs_p->_mutex_lock(g_wifi_global_lock);
	/* A submitted MAC frame may still read its key at hardware enable.
	 * Refuse a slot mutation until those flights drain; do not re-encrypt an
	 * old PN with a newly installed key. Queued frames use a separate token. */
	if (slot == 4 && s31_sm_key_pins_any()) {
		g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
		return ESP_ERR_INVALID_STATE;
	}
	if (request->operation == S31_WIFI_SOFTMAC_RX_KEY_ADD) {
		/* Refuse to overwrite keys installed by a different native owner. */
		if (*s31_sm_key_valid() & ~s31_sm_key_mask) {
			g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
			return ESP_ERR_INVALID_STATE;
		}
		info[2] = request->offset;
		memcpy(info + 3, request->mac, 6);
		memcpy(key, request->data, sizeof(key));
		hal_crypto_set_key_entry(slot, key, sizeof(key), info);
		for (i = 0; i < 4; i++) ((volatile uint32_t *)key)[i] = 0;
		s31_sm_key_mask |= 1U << slot;
		memcpy(s31_sm_key_ta[slot], request->mac, 6);
  s31_sm_key_id[slot] = request->offset;
  if (request->total & S31_SOFTMAC_KEY_RX_CCMP) s31_sm_key_hw_mask |= 1U << slot;
  else s31_sm_key_hw_mask &= ~(1U << slot);
  if (request->total & S31_SOFTMAC_KEY_TX_CCMP) s31_sm_tx_key_mask |= 1U << slot;
  else s31_sm_tx_key_mask &= ~(1U << slot);
		if (slot == 4) s31_sm_tx_key_token =
			(request->total & S31_SOFTMAC_KEY_TX_CCMP) ? request->beacon_interval : 0;
	} else {
		s31_sm_key_remove(slot);
	}
	if (s31_sm_key_mask) {
		hal_crypto_enable(0, 3, 0, 0);
		/* Existing RX-only mode must never double-encrypt Linux ciphertext.
		 * A group-key update must preserve an installed TX pairwise key. */
		if (!s31_sm_tx_key_mask) *s31_sm_rx_crypto_reg() &= ~0xffffU;
	} else {
		*s31_sm_rx_crypto_reg() = 0;
	}
	g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);

	return 0;
}
