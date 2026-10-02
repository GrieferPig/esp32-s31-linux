/* SPDX-License-Identifier: BSD-2-Clause */
/* Pinned S31 IDF a602e67b private TRC/PP ABI. TID0 only, STA peer index0,
 * HT20 long GI or negotiated HE-SU20/GI1.6. Caller executes under the existing radio owner/blob gate.
 * No worker, packet cache, deliberate aggregation delay or native key/PN.
 */
extern void ic_set_trc(const void *);
extern void ic_set_he_min_subframe_len_tab(unsigned);
extern void ic_set_he_rts_threshold_bytes_tab(unsigned, unsigned);
extern void hal_he_set_packet_padding_us(unsigned);
extern uint8_t *rc_get_sta_trc(unsigned);
extern void ic_ampdu_op(unsigned, unsigned, unsigned, unsigned);
extern unsigned ppSearchTxQueue(unsigned, unsigned, unsigned, unsigned);
extern void ppClearTxq(unsigned);
extern void lmacStopTransmit(unsigned);

static uint8_t *s31_sm_tx_peer;
static uint8_t s31_sm_tx_ap[6], s31_sm_tx_mcs, s31_sm_tx_limit, s31_sm_tx_qid;
static bool s31_sm_tx_ba, s31_sm_tx_he;
/* A bitmap avoids padding another byte into every eight-byte flight slot. */
static uint64_t s31_sm_tx_agg_flights;


static bool s31_sm_tx_agg_inflight(void)
{
	unsigned i;
	for (i = 0; i < S31_SOFTMAC_PENDING; i++)
		if (s31_sm_flights[i].eb && (s31_sm_tx_agg_flights & (UINT64_C(1) << i))) return true;
	return false;
}

static int s31_sm_tx_peer_add(const struct s31_wifi_control *r)
{
	uint8_t args[36] = {0}, *peer;
	uint16_t peer_flags, byte_limit;
	unsigned qid;
	int rc = 0;
	if (!s31_sm_active || s31_sm_tx_peer) return ESP_ERR_INVALID_STATE;
	if (r->field > (r->hidden ? 9 : 7) || r->offset < 1 || r->offset > 2007 ||
	    r->total > 3 || r->length > 7 || r->hidden > 1 || (r->mac[0] & 1) ||
	    !(r->mac[0] | r->mac[1] | r->mac[2] | r->mac[3] | r->mac[4] | r->mac[5]) ||
	    (r->dtim_period != 2 && r->dtim_period != 4 &&
	     r->dtim_period != 8 && r->dtim_period != 16))
		return ESP_ERR_INVALID_ARG;
	g_osi_funcs_p->_mutex_lock(g_wifi_global_lock);
	/* Do not reinitialize a native association or reuse an occupied TRC. */
	if (r->hidden && !s31_sm_he_saved) { rc = ESP_ERR_INVALID_STATE; goto out; }
	if (rc_get_sta_trc(0)) { rc = ESP_ERR_INVALID_STATE; goto out; }
	args[0] = 1;
	sm_put32(args, 4, (uintptr_t)r->mac);
	args[10] = r->offset;
	args[12] = 2; /* Private TRC PHY: HT20 LGI=2, SGI=3; 4/5 select HT40.
	               * This is not the public wifi_phy_mode_t enumeration. */
	sm_put32(args, 16, 40);
	args[24] = 130; /* Highest 1-stream HT20 LGI rate, 65 Mbps in .5 Mbps. */
	if (r->hidden) {
		/* ic_set_sta keeps private PHY2 and supplies public mode7 in
		 * args27/peer142. Private PHY6 is LR, not HE. */
		args[24] = r->field == 9 ? 229 : r->field == 8 ? 206 : 172;
		/* Pinned rcGet11AXHighestRateIdx accepts these MCS9/8/7 base
		 * table entries. GI remains packet metadata, fixed to1.6us. */
		args[27] = 7;
		args[28] = 2; /* IDF STA default packet padding =16us. */
	}
	args[25] = 1;
	args[26] = r->total | (r->length << 2);
	ic_set_trc(args);
	peer = rc_get_sta_trc(0);
	if (!peer || peer[132] || peer[133] || memcmp(peer + 33, r->mac, 6)) {
		args[0] = 0; ic_set_trc(args);
		rc = ESP_ERR_NO_MEM; goto out;
	}
	/* Bit7 identifies the interface default TRCs created by trc_init.
	 * ppMapTxQueue bypasses peer/TID queues for those objects. A fresh
	 * rc_enable_trc connection is zeroed; reject an unexpected default. */
	memcpy(&peer_flags, peer + 12, sizeof(peer_flags));
	if (peer_flags & 0x80U) {
		args[0] = 0; ic_set_trc(args);
		rc = ESP_ERR_INVALID_STATE; goto out;
	}
	qid = ppSearchTxQueue(0, 0, 0, 2); /* BE: iface0, TRC0, TID0, AC2. */
	if (qid >= 16) {
		args[0] = 0; ic_set_trc(args);
		rc = ESP_ERR_NO_MEM; goto out;
	}
	/* Preserve fixed rates when rcGetSched attaches this peer. All three
	 * private fixed-rate selectors are explicit; retry table matches the mode. */
	memcpy(&peer_flags, peer + 12, sizeof(peer_flags));
	sm_put16(peer, 12, peer_flags | 7U);
	peer[8] = peer[9] = peer[10] = 16 + r->field;
	sm_put32(peer, 100, (uintptr_t)(r->hidden ? rc11AXSchedTbl[10 - r->field] : rc11NSchedTbl[8 - r->field]));
	sm_put32(peer, 104, sm_get32(peer, 100));
	if (r->hidden) {
		ic_set_he_min_subframe_len_tab(r->length);
		ic_set_he_rts_threshold_bytes_tab(1023, 2);
		hal_he_set_packet_padding_us(16);
	}
	/* Respect negotiated length and bound native assembly memory/work.
	 * PP also checks sequence distance against each head EB's MPDU cap. */
	memcpy(&byte_limit, peer + 124, sizeof(byte_limit));
	if (byte_limit > r->dtim_period * (S31_SOFTMAC_FRAME_MAX + 12U))
		byte_limit = r->dtim_period * (S31_SOFTMAC_FRAME_MAX + 12U);
	sm_put16(peer, 126, byte_limit);
	sm_put16(peer, 128, byte_limit);
	s31_sm_tx_he = r->hidden;
	s31_sm_tx_peer = peer;
	s31_sm_tx_mcs = r->field;
	s31_sm_tx_limit = r->dtim_period;
	s31_sm_tx_qid = qid;
	memcpy(s31_sm_tx_ap, r->mac, 6);
	s31_sm_tx_ba = false;
out:
	g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
	return rc;
}

static int s31_sm_tx_ba_control(const struct s31_wifi_control *r)
{
	uint8_t args[36] = {0};
	unsigned i;
	uint32_t cancelled[S31_SOFTMAC_PENDING];
	unsigned n = 0;
	if (!s31_sm_active || !s31_sm_tx_peer) return ESP_ERR_INVALID_STATE;
	if (r->total || memcmp(r->mac, s31_sm_tx_ap, 6)) return ESP_ERR_INVALID_ARG;
	if (r->operation == S31_WIFI_SOFTMAC_TX_BA_ON &&
	    (!r->length || r->length < s31_sm_tx_limit)) return ESP_ERR_INVALID_ARG;
	g_osi_funcs_p->_mutex_lock(g_wifi_global_lock);
	if (r->operation == S31_WIFI_SOFTMAC_TX_BA_ON) {
		ic_ampdu_op(0, 0, 0, 1);
		s31_sm_tx_ba = true;
	} else if (r->operation == S31_WIFI_SOFTMAC_TX_PEER_DEL) {
		/* Disable does not free the allocation. Never reset a peer still
		 * referenced by native EBs or reuse it for a different AP. */
		if (s31_sm_tx_agg_inflight()) {
			g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
			return ESP_ERR_NO_MEM;
		}
		ic_ampdu_op(0, 0, 0, 0);
		sm_put32(args, 4, (uintptr_t)s31_sm_tx_ap);
		ic_set_trc(args);
		s31_sm_tx_peer = NULL; s31_sm_tx_ba = false; s31_sm_tx_he = false;
		memset(s31_sm_tx_ap, 0, sizeof(s31_sm_tx_ap));
	} else if (r->operation == S31_WIFI_SOFTMAC_TX_BA_FLUSH) {
		/* Only the TID0 prototype attaches EBs to AC2. The other raw
		 * traffic keeps its null-peer AC0. Invalidate hardware first,
		 * then clear this private queue. Direct recycling has no callback:
		 * unpublish each flight before native reclamation and fail once. */
		s31_sm_tx_ba = false;
		ic_ampdu_op(0, 0, 0, 0);
		for (i = 0; i < S31_SOFTMAC_PENDING; i++)
			if (s31_sm_flights[i].eb && (s31_sm_tx_agg_flights & (UINT64_C(1) << i))) {
				cancelled[n++] = s31_sm_flights[i].cookie;
				s31_sm_flights[i].eb = NULL;
				s31_sm_tx_agg_flights &= ~(UINT64_C(1) << i);
				s31_sm_mac_ccmp_flights &= ~(UINT64_C(1) << i);
			}
		/* A drained queue can retain a recycled last-EB pointer in LMAC.
		 * Its stop helper dereferences EB metadata even when HW is invalid.
		 * With no native flights there is nothing to abort or recycle. */
		if (n) {
			lmacStopTransmit(2);
			ppClearTxq(s31_sm_tx_qid);
		}
		for (i = 0; i < n; i++)
			s31_sm_report(cancelled[i], false, (s31_sm_tx_he ? S31_SOFTMAC_TX_HE_RATE :
				S31_SOFTMAC_TX_HT_RATE) | s31_sm_tx_mcs, 1);
	} else if (r->operation == S31_WIFI_SOFTMAC_TX_BA_OFF) {
		/* Linux may already have expired a cookie, so it must also check
		 * native ownership before signalling STOP_CONT to mac80211. */
		if (s31_sm_tx_agg_inflight()) {
			g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
			return ESP_ERR_NO_MEM;
		}
		s31_sm_tx_ba = false;
		ic_ampdu_op(0, 0, 0, 0);
	} else {
		g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
		return ESP_ERR_INVALID_ARG;
	}
	g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
	return 0;
}

static void s31_sm_tx_agg_prepare(uint8_t *eb, unsigned len, unsigned hdrlen)
{
	uint8_t *desc = (void *)(uintptr_t)sm_get32(eb, 4);
	uint8_t *meta = (void *)(uintptr_t)sm_get32(eb, 0x34);
	uint32_t payload = sm_get32(desc, 4);
	uint16_t eb_flags;
	/* Class1 dynamic allocation provides the native eight-byte prefix
	 * headroom. PP fills length, sequence and spacing before assembly. */
	sm_put32(desc, 4, payload - 8U);
	/* PP marks only the final aggregate DMA as EOF in ppAssembleAMPDU.
	 * The raw path's pre-set EOF must not terminate an intermediate MPDU. */
	sm_put32(desc, 0, (sm_get32(desc, 0) & ~0x4fffc000U) | ((len + 8U) << 14));
	sm_put16(eb, 20, hdrlen + 8U);
	memcpy(&eb_flags, eb + 36, sizeof(eb_flags));
	sm_put16(eb, 36, eb_flags | 0x2000U);
	sm_put32(eb, 44, (uintptr_t)s31_sm_tx_peer);
	eb[27] = s31_sm_tx_limit;
	/* ROM ppProcessTxQ tests request bit18 (mask0x01040000). Bit22 is
	 * an assembled-aggregate result, set with bit19 by ppAssembleAMPDU.
	 * Clear raw prefix mode and do not claim assembly before PP does it.
	 * Preserve the cipher selector: NONE for Linux ciphertext, CCMP for
	 * MAC encryption of Linux's plaintext/IV. */
	sm_put32(meta, 0, (sm_get32(meta, 0) & ~(0x4000U | (1U << 22))) | (1U << 18));
}
