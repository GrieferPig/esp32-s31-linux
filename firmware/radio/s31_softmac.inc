/* SPDX-License-Identifier: BSD-2-Clause */
/* Private ABI recovered from IDF a602e67b, ESP32-S31 only. Included by radio_stack.c.
 * PP stage-0 callbacks and submissions run under the Linux blob gate. Never
 * retain eb after the callback; PP owns allocation after ppTxPkt on ALL returns.
 */
_Static_assert(sizeof(struct s31_softmac_tx_header) == 12, "raw TX bridge ABI");
_Static_assert(sizeof(struct s31_softmac_tx_status) == 8, "TX status bridge ABI");
_Static_assert(sizeof(struct s31_softmac_rx_header) == 4, "raw RX bridge ABI");
#include "freertos/semphr.h"
#include "esp_timer.h"
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
extern int ppTxPkt(void *eb, int post);
extern int ppRegisterTxCallback(void (*cb)(void *), unsigned int index, unsigned int stage);
extern int pp_unregister_tx_cb(unsigned int index);
extern void ic_disable_crypto(unsigned int interface, unsigned int skip);
extern void *g_wifi_global_lock;
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
static unsigned int s31_sm_key_error_trace;
static unsigned int s31_sm_key_trace;
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
static bool s31_sm_rx_view_enabled;
static bool s31_sm_rx_ra_filter;
static bool s31_sm_rx_error_filter;
static bool s31_sm_rx_bssid_filter;
static bool s31_sm_bssid_policy_active;
static uint8_t s31_sm_bssid_policy_mac[6];
static unsigned s31_sm_rx_bssid_filtered;
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
  s31_sm_rx_bssid_filtered++;
  return true;
 }
 return false;
}

static unsigned s31_sm_rx_errors_filtered;
static unsigned s31_sm_rx_ra_filtered;
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
 s31_linux_printf("[S31-SM] HE BSS active=%u aid=%u color=%u enabled=%u\n",
                  r->length, r->field, r->offset, r->total);
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
 s31_linux_printf("[S31-SM] BSSID filter enabled=%u reg=%08x\n",r->field,*s31_sm_bssid_filter_reg());
 g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
 return 0;
}
#define S31_SM_CB_INDEX 15U /* Inside the observed callback table, before +0x410. */
struct s31_sm_flight { void *eb; uint32_t cookie; };
static struct s31_sm_flight s31_sm_flights[S31_SOFTMAC_PENDING];
static uint64_t s31_sm_mac_ccmp_flights;
static bool s31_sm_cb_registered;
static uint32_t s31_sm_epoch;
static uint8_t s31_sm_mac[6];
static uint32_t s31_sm_rx_crypto_saved;
static volatile uint32_t *s31_sm_rx_crypto_reg(void)
{
	/* Pinned hal_crypto_init/enable/disable: STA crypto control. */
	return (volatile uint32_t *)0x20104800U;
}
static unsigned int s31_sm_tx_trace, s31_sm_rx_trace, s31_sm_done_trace, s31_sm_protected_trace;
static unsigned int s31_sm_ht_done_trace;
static unsigned int s31_sm_phy_trace;
static uint8_t s31_sm_rx_wire[sizeof(struct s31_softmac_rx_header) + S31_SOFTMAC_FRAME_MAX];
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
static void sm_put16(void *p, unsigned int offset, uint16_t value)
{
	memcpy((uint8_t *)p + offset, &value, sizeof(value));
}
#include "s31_softmac_tx_reports.inc"
#include "s31_softmac_tx_agg.inc"
static void s31_sm_tx_done(void *eb)
{
	unsigned int i;
	const uint8_t *meta = (const void *)(uintptr_t)sm_get32(eb, 0x34);

	for (i = 0; i < S31_SOFTMAC_PENDING; i++) {
		if (s31_sm_flights[i].eb == eb) {
			uint32_t cookie = s31_sm_flights[i].cookie;
			s31_sm_flights[i].eb = NULL;
			s31_sm_mac_ccmp_flights &= ~(UINT64_C(1) << i);
			if (s31_sm_done_trace++ < 8)
				s31_linux_printf("[S31-SM] TX done cookie=%u status=%u rate=%u retries=%u/%u\n",
					cookie, meta[0x13], meta[0x0c], meta[6], meta[7]);
			int rate = s31_sm_rate_index(meta[0x0c]);
			if (meta[0x0c] >= 16 && meta[0x0c] <=
			    ((sm_get32(meta, 0) & (1U << 31)) ? 25 : 23)) {
				rate = ((sm_get32(meta, 0) & (1U << 31)) ?
				S31_SOFTMAC_TX_HE_RATE : S31_SOFTMAC_TX_HT_RATE) | (meta[0x0c] - 16);
				if (s31_sm_ht_done_trace++ < 4)
					s31_linux_printf("[S31-SM] TX MCS done cookie=%u status=%u PHY=%u retries=%u/%u HE=%u fmt=%u gi=%u\n",
						cookie, meta[0x13], meta[0x0c], meta[6], meta[7],
						!!(sm_get32(meta, 0) & (1U << 31)), (meta[47] >> 3) & 15, meta[47] & 3);
			}
			s31_sm_report(cookie, meta[0x13] == 1, rate < 0 ? 0 : rate,
				      1U + meta[6] + meta[7]);
			break;
		}
	}
}
static void s31_sm_rx_frame(const wifi_promiscuous_pkt_t *packet,
                             const uint8_t *frame, wifi_promiscuous_pkt_type_t type)
{
	unsigned int sig_len = packet->rx_ctrl.sig_len;
 if (s31_sm_key_mask && packet->rx_ctrl.rx_state && s31_sm_key_error_trace++ < 32)
  s31_linux_printf("[S31-SM] RX crypto error kind=%u state=%u end=%u fmt=%u sig=%u dump=%u match=%u/%u/%u/%u\n", type,
   packet->rx_ctrl.rx_state, packet->rx_ctrl.rxend_state, packet->rx_ctrl.cur_bb_format,
   sig_len, packet->rx_ctrl.dump_len, packet->rx_ctrl.rxmatch0, packet->rx_ctrl.rxmatch1,
   packet->rx_ctrl.rxmatch2, packet->rx_ctrl.rxmatch3);
	unsigned int len = sig_len >= 4 ? sig_len - 4 : 0;
	unsigned int rate = packet->rx_ctrl.rate;
	int rate_index = -1;
	uint8_t encoding = 0, phy_flags = 0;
	uint16_t fc = 0;
	if (s31_sm_active && packet->rx_ctrl.cur_bb_format >= 2 && s31_sm_phy_trace++ < 24)
		s31_linux_printf("[S31-SM] PHY type=%u fmt=%u state=%u sig=%u dump=%u rate=%u siga=%08x/%08x\n",
			type, packet->rx_ctrl.cur_bb_format, packet->rx_ctrl.rx_state,
			sig_len, packet->rx_ctrl.dump_len, rate,
			packet->rx_ctrl.he_siga1, packet->rx_ctrl.he_siga2);
	if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
	/* Same STA unicast-RA rejection as the Linux frontend. Keep all groups,
	 * beacons, and probe responses, including during a software scan. This
	 * feature is negotiated only when Linux's RX filter is enabled. */
	if (s31_sm_active && s31_sm_rx_ra_filter && len >= 24 &&
	    !(frame[4] & 1) && memcmp(frame + 4, s31_sm_mac, 6)) {
		s31_sm_rx_ra_filtered++;
		return;
	}
	if (len >= 24) memcpy(&fc, frame, sizeof(fc));
 if (s31_sm_active && s31_sm_bssid_should_drop(frame,len,fc)) return;
	if (type == WIFI_PKT_DATA && (fc & 12) != 8) return;
	/* Probe only; do not mark plaintext/MIC validation from this trace. */
	if (s31_sm_key_mask && len >= 40 && (fc & 0x4000) &&
	    !memcmp(frame + 4, s31_sm_mac, 6) &&
	    s31_sm_key_trace++ < 32) {
		unsigned int hdr = 24 + ((fc & 0x0080) ? 2 : 0);
		s31_linux_printf("[S31-SM] RX key probe fmt=%u state=%u end=%u sig=%u dump=%u match=%u/%u/%u/%u ccmp=%02x llc=%02x%02x\n",
			packet->rx_ctrl.cur_bb_format, packet->rx_ctrl.rx_state,
			packet->rx_ctrl.rxend_state, sig_len, packet->rx_ctrl.dump_len,
			packet->rx_ctrl.rxmatch0, packet->rx_ctrl.rxmatch1,
			packet->rx_ctrl.rxmatch2, packet->rx_ctrl.rxmatch3,
			frame[hdr + 3], frame[hdr + 8], frame[hdr + 9]);
	}
	if (s31_sm_active && len >= 24 && (fc & 0x4000) &&
	    !memcmp(frame + 4, s31_sm_mac, sizeof(s31_sm_mac)) &&
	    s31_sm_protected_trace++ < 16)
		s31_linux_printf("[S31-SM] RX protected own len=%u fc=%04x state=%u fmt=%u rate=%u\n",
			len, fc, packet->rx_ctrl.rx_state, packet->rx_ctrl.cur_bb_format, rate);

	if (s31_sm_active && s31_sm_rx_trace++ < 8)
		s31_linux_printf("[S31-SM] RX type=%u sig=%u dump=%u fmt=%u rate=%u state=%u\n",
			type, sig_len, packet->rx_ctrl.dump_len, packet->rx_ctrl.cur_bb_format,
			rate, packet->rx_ctrl.rx_state);
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
 if (s31_sm_rx_view_enabled) {
  const struct s31_softmac_rx_view view = {
   .header = { .rate_index = rate_index, .encoding = encoding, .flags = phy_flags },
   .frame = frame,
  };
  s31_radio_wifi_receive_aux(S31_WIFI_IF_SOFTMAC_VIEW, (const void *)&view,
                            len, packet->rx_ctrl.channel, packet->rx_ctrl.rssi);
  return;
 }
 memset(s31_sm_rx_wire, 0, sizeof(struct s31_softmac_rx_header));
	s31_sm_rx_wire[0] = rate_index;
	s31_sm_rx_wire[1] = encoding;
	s31_sm_rx_wire[2] = phy_flags;
	memcpy(s31_sm_rx_wire + sizeof(struct s31_softmac_rx_header), frame, len);
	s31_radio_wifi_receive_aux(S31_WIFI_IF_SOFTMAC, s31_sm_rx_wire,
				  len + sizeof(struct s31_softmac_rx_header),
				  packet->rx_ctrl.channel, packet->rx_ctrl.rssi);
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
static unsigned s31_sm_direct_count[8];
static void s31_sm_data_direct(void *last, unsigned count, unsigned state)
{
 void *head = s31_sm_rx_head();
 uint8_t *buf;
 unsigned bytes, off, need;
 uint16_t extra = 0, fc;
 typeof(((wifi_promiscuous_pkt_t *)0)->rx_ctrl) ctrl;
 if (!s31_sm_active) { s31_sm_direct_count[1]++; goto fallback; }
 if (count > 1 || head != last || !head) { s31_sm_direct_count[2]++; goto fallback; }
 if ((state & 0xf8) == 0x78) { s31_sm_direct_count[3]++; goto fallback; }
 buf = (void *)(uintptr_t)sm_get32(head, 4);
 bytes = (sm_get32(head, 0) >> 14) & 0x3fff;
 if (!buf || bytes < 64) { s31_sm_direct_count[4]++; goto fallback; }
 off = get_sublen_offset(buf, &extra) & 0xffff;
 if (off < 56 || off > bytes - 8 || bytes - off - 8 < 24) { s31_sm_direct_count[5]++; goto fallback; }
 memcpy(&ctrl, buf, 56);
 memcpy((uint8_t *)&ctrl + 56, buf + off, 8);
 /* The frontend rejects every nonzero rx_state already. Reject it before
  * the original sniffer allocates/queues a partial payload; the ROM caller
  * still owns descriptor recycling, exactly as on the valid direct path. */
 if (s31_sm_rx_error_filter && ctrl.rx_state) {
  s31_sm_rx_errors_filtered++;
  return;
 }
 memcpy(&fc, buf + off + 8, sizeof(fc));
 if (((fc & 12) != 8 && (fc & 12) != 0) || ctrl.sig_len < 28 || ctrl.sig_len > S31_SOFTMAC_FRAME_MAX+4)
  { s31_sm_direct_count[6]++; goto fallback; }
 need = ctrl.sig_len - 4;
 if (s31_sm_key_hw_mask && !ctrl.rx_state && !ctrl.rxend_state &&
     ctrl.dump_len + 4U == ctrl.sig_len) need -= 8;
 if (need > bytes - off - 8) { s31_sm_direct_count[7]++; goto fallback; }
 s31_sm_direct_count[0]++;
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
 memset(s31_sm_direct_count, 0, sizeof(s31_sm_direct_count));
 s31_sm_data_saved = *s31_sm_data_slot();
 *s31_sm_data_slot() = s31_sm_data_direct;
 s31_sm_sniffer_saved = *slot;
 *slot = s31_sm_rx_direct;
 return 0;
}
static void s31_sm_sniffer_restore(void)
{
 if (s31_sm_rx_bssid_filter) s31_linux_printf("[S31-SM] early BSSID filtered=%u\n",s31_sm_rx_bssid_filtered);
 if (s31_sm_rx_error_filter)
  s31_linux_printf("[S31-SM] early RX errors filtered=%u\n",s31_sm_rx_errors_filtered);
 if (s31_sm_data_saved)
  s31_linux_printf("[S31-SM] early RA filtered=%u\n", s31_sm_rx_ra_filtered);
 if (s31_sm_data_saved)
  s31_linux_printf("[S31-SM] direct fast=%u inactive=%u scatter=%u state=%u short=%u offset=%u type=%u bounds=%u\n",
   s31_sm_direct_count[0],s31_sm_direct_count[1],s31_sm_direct_count[2],s31_sm_direct_count[3],
   s31_sm_direct_count[4],s31_sm_direct_count[5],s31_sm_direct_count[6],s31_sm_direct_count[7]);
 if (s31_sm_data_saved) {
  *s31_sm_data_slot() = s31_sm_data_saved;
  s31_sm_data_saved = NULL;
 }
 if (s31_sm_sniffer_saved) {
  *s31_sm_sniffer_slot() = s31_sm_sniffer_saved;
  s31_sm_sniffer_saved = NULL;
 }
}
static int s31_sm_send(uint8_t *wire, uint16_t length)
{
	struct s31_softmac_tx_header header;
	uint8_t *frame = wire + sizeof(header), *eb, *meta, *desc;
	unsigned int i, len, hdrlen;
	uint16_t fc;
	bool qos;
	bool ht, he;
	bool ampdu;
	bool mac_ccmp;
	int rc;

	if (length < sizeof(header) + 24 || length > sizeof(header) + S31_SOFTMAC_FRAME_MAX)
		return ESP_ERR_INVALID_ARG;
	memcpy(&header, wire, sizeof(header));
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
	len = length - sizeof(header);
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
	/* We may run on PP itself. Never wait for a control task holding the
	 * global lock while it waits for PP to consume its queued command.
	 * No descriptor/status is consumed before this retryable failure. */
	if (!xSemaphoreTakeRecursive(g_wifi_global_lock, 0))
		return ESP_ERR_NO_MEM;
	/* HE is limited to the negotiated native BA path. Retiring the BSS or
	 * a different peer must never turn an already queued HT frame into HE. */
	if ((ampdu && he != s31_sm_tx_he) ||
	    (he && (!ampdu || !s31_sm_tx_he || !s31_sm_he_saved))) {
		g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
		goto rejected;
	}
	/* Key changes share this lock. Do not send plaintext after a removal,
	 * mismatch or stale submission, even if mac80211 already allocated PN. */
	if (mac_ccmp && (!(s31_sm_tx_key_mask & (1U << 4)) ||
	    header.reserved[2] != s31_sm_tx_key_token ||
	    !(*s31_sm_key_valid() & (1U << 4)) ||
	    memcmp(frame + 4, s31_sm_key_ta[4], 6) ||
	    (frame[hdrlen + 3] >> 6) != s31_sm_key_id[4])) {
		g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
		goto rejected;
	}
	for (i = 0; i < S31_SOFTMAC_PENDING; i++)
		if (!s31_sm_flights[i].eb)
			break;
	if (i == S31_SOFTMAC_PENDING) {
		g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
		goto rejected;
	}
	/* Class 1 and metadata follow esp_wifi_80211_tx/output_raw_process. */
	eb = ic_ebuf_alloc(frame, 1, len);
	if (!eb) {
		g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
		goto rejected;
	}
	meta = (void *)(uintptr_t)sm_get32(eb, 0x34);
	desc = (void *)(uintptr_t)sm_get32(eb, 4);
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
	if (mac_ccmp) s31_sm_mac_ccmp_flights |= UINT64_C(1) << i;
	else s31_sm_mac_ccmp_flights &= ~(UINT64_C(1) << i);
	if (ampdu) s31_sm_tx_agg_flights |= UINT64_C(1) << i;
	else s31_sm_tx_agg_flights &= ~(UINT64_C(1) << i);
	if (s31_sm_tx_trace++ < 8)
		s31_linux_printf("[S31-SM] TX submit cookie=%u len=%u fc=%04x\n", header.cookie, len, fc);
	rc = ppTxPkt(eb, 1);
	/* PP may recycle on rejection, or enqueue even when post fails. Never
	 * free eb here, and never return a retryable code after this call. */
	if (rc && s31_sm_flights[i].eb == eb) {
		s31_sm_flights[i].eb = NULL;
		s31_sm_mac_ccmp_flights &= ~(UINT64_C(1) << i);
		s31_sm_report(header.cookie, false, 0, 1);
	}
	g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
	return 0;
rejected:
	s31_sm_report(header.cookie, false, 0, 1);
	return 0;
}
static int s31_sm_stop(void)
{
	int rc;
	unsigned int i;
	if (s31_sm_active && s31_sm_tx_peer) {
		struct s31_wifi_control tx_stop = { .operation = S31_WIFI_SOFTMAC_TX_BA_FLUSH };
		memcpy(tx_stop.mac, s31_sm_tx_ap, 6);
		s31_sm_tx_ba_control(&tx_stop);
		tx_stop.operation = S31_WIFI_SOFTMAC_TX_PEER_DEL;
		s31_sm_tx_ba_control(&tx_stop);
	}
	if (s31_sm_active) {
		g_osi_funcs_p->_mutex_lock(g_wifi_global_lock);
		for (i = 0; i < 4; i++) if (s31_sm_ba_mask & (1U << i)) hal_agreement_del_rx_ba(i);
		s31_sm_ba_mask = 0;
		s31_sm_he_restore();
		s31_sm_bssid_filter_restore();
		s31_sm_sniffer_restore();
		for (i = 0; i < 5; i++) if (s31_sm_key_mask & (1U << i)) s31_sm_key_remove(i);
		g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
	}

	if (s31_sm_active)
		*s31_sm_rx_crypto_reg() = s31_sm_rx_crypto_saved;
	s31_sm_active = false;
	s31_sm_rx_view_enabled = false;
	s31_sm_rx_ra_filter = false;
 s31_sm_rx_error_filter = false;
 s31_sm_rx_bssid_filter = false;
	esp_wifi_set_promiscuous(false);
	esp_wifi_set_promiscuous_rx_cb(NULL);
	rc = esp_wifi_stop();
	if (!rc) {
		s31_wifi_start_requested = 0;
		s31_wifi_start_complete = 0;
		memset(s31_sm_flights, 0, sizeof(s31_sm_flights));
		s31_sm_mac_ccmp_flights = 0;
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
	unsigned int i;
	int rc;

	if (s31_sm_active || s31_wifi_start_requested)
		return ESP_ERR_INVALID_STATE;
	if (request->channel < 1 || request->channel > 11)
		return ESP_ERR_INVALID_ARG;
	rc = esp_wifi_set_mac(WIFI_IF_STA, request->mac);
	if (!rc) rc = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
		((request->field & 1) ? WIFI_PROTOCOL_11N : 0) |
		((request->field & 2) ? WIFI_PROTOCOL_11AX : 0));
	if (!rc) rc = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
	if (!rc) rc = esp_wifi_set_ps(WIFI_PS_NONE);
	s31_linux_printf("[S31-SM] config rc=%d\n", rc);
	if (!rc && !s31_sm_cb_registered) {
		rc = ppRegisterTxCallback(s31_sm_tx_done, S31_SM_CB_INDEX, 0);
		s31_linux_printf("[S31-SM] PP callback slot=%u rc=%d\n", S31_SM_CB_INDEX, rc);
		if (!rc) s31_sm_cb_registered = true;
	}
	if (!rc) rc = esp_wifi_start();
	s31_linux_printf("[S31-SM] esp_wifi_start rc=%d\n", rc);
	if (!rc) {
		s31_wifi_start_requested = 1;
		for (i = 0; i < 200 && !s31_wifi_start_complete; i++)
			vTaskDelay(pdMS_TO_TICKS(10) ?: 1);
		if (!s31_wifi_start_complete) rc = ESP_ERR_TIMEOUT;
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
		s31_sm_tx_trace = s31_sm_rx_trace = s31_sm_done_trace = s31_sm_protected_trace = 0;
		s31_sm_ht_done_trace = 0;
		s31_sm_phy_trace = 0;
		s31_sm_key_trace = s31_sm_key_error_trace = 0;
		s31_sm_rx_view_enabled = !!(request->field & S31_SOFTMAC_START_RX_VIEW);
		s31_sm_rx_ra_filter = !!(request->field & S31_SOFTMAC_START_RX_RA_FILTER);
		s31_sm_rx_ra_filtered = 0;
  s31_sm_rx_error_filter = !!(request->field & S31_SOFTMAC_START_RX_DROP_ERRORS);
  s31_sm_rx_bssid_filter = !!(request->field & S31_SOFTMAC_START_RX_BSSID_FILTER);
  s31_sm_rx_bssid_filtered = 0;
  s31_sm_rx_errors_filtered = 0;
		s31_sm_epoch = request->offset;
		memcpy(s31_sm_mac, request->mac, sizeof(s31_sm_mac));
		s31_sm_active = true;
	}
	else if (s31_wifi_start_requested) s31_sm_stop();
	s31_linux_printf("[S31-SM] start result=%d complete=%d active=%d epoch=%u\n",
			 rc, s31_wifi_start_complete, s31_sm_active, s31_sm_epoch);
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
	s31_linux_printf("[S31-SM] RX BA op=%u slot=%u tid=%u ssn=%u window=%u\n",
		request->operation, slot, request->total, request->offset, request->length);
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
	if (slot == 4 && s31_sm_mac_ccmp_flights) {
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
	s31_linux_printf("[S31-SM] RX key probe op=%u slot=%u keyid=%u mask=%x\n",
		request->operation, slot, request->offset, s31_sm_key_mask);
	return 0;
}
