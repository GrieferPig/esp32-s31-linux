/* SPDX-License-Identifier: BSD-2-Clause */
/* Included by s31_softmac.inc. Pinned cnx_sta_pm -> pm_start RV32 ABI:
 * interface,AID,BSSID,beacon TU,local RX timestamp,AP TSF low/high,
 * transmitted BSSID,MBSSID indicator/index,extra flag. mac80211 supplies
 * association identity; an accepted beacon supplies timing. Native MLME and
 * its keepalive entry point are not started. */
extern void pm_start(unsigned, unsigned, const uint8_t *, unsigned, uint32_t,
                     uint32_t, uint32_t, const uint8_t *, unsigned, unsigned,
                     unsigned);
extern void pm_stop(void);
extern uint8_t g_pm[];

static bool s31_sm_pm_owned;
static struct {
 uint8_t bssid[6];
 uint16_t aid, beacon_tu;
 uint32_t local_stamp, tsf_low, tsf_high;
} s31_sm_pm_beacon;

/* All policy transitions and this callback hold the existing Wi-Fi resource
 * mutex. Stop the SDK's timers before retiring their association identity. */
static void s31_sm_coex_pm_stop(void)
{
 if (!s31_sm_pm_owned) return;
 s31_sm_pm_owned = false;
 pm_stop();
}

static int s31_sm_coex_pm_start_in_owner(void *unused)
{
 (void)unused;
 g_osi_funcs_p->_mutex_lock(g_wifi_global_lock);
 if (!s31_sm_pm_owned && s31_sm_coex_saved && s31_sm_coex_state == 2 &&
     s31_sm_coex_aid == s31_sm_pm_beacon.aid &&
     s31_sm_coex_period == ((uint32_t)s31_sm_pm_beacon.beacon_tu << 10) &&
     !memcmp(s31_sm_coex_bssid, s31_sm_pm_beacon.bssid, 6)) {
  pm_start(0, s31_sm_pm_beacon.aid, s31_sm_pm_beacon.bssid,
           s31_sm_pm_beacon.beacon_tu, s31_sm_pm_beacon.local_stamp,
           s31_sm_pm_beacon.tsf_low, s31_sm_pm_beacon.tsf_high,
           s31_sm_pm_beacon.bssid, 0, 0, 0);
  s31_sm_pm_owned = g_pm[14] != 0;
 }
 g_osi_funcs_p->_mutex_unlock(g_wifi_global_lock);
 return 0;
}

static __attribute__((noinline)) void s31_sm_coex_beacon(const uint8_t *frame)
{
 /* The RX owner consumes one frame at a time. The synchronous SDK IPC joins
  * the callback before this owner can overwrite the snapshot. Both objects
  * are persistent, including if teardown cancels the initiating task. */
 static s31_sm_wifi_ipc_t request = { .fn = s31_sm_coex_pm_start_in_owner };
 if (s31_sm_pm_owned ||
     !__atomic_load_n(&s31_sm_coex_saved, __ATOMIC_ACQUIRE) ||
     __atomic_load_n(&s31_sm_coex_state, __ATOMIC_RELAXED) != 2 ||
     memcmp(frame + 16, s31_sm_coex_bssid, 6)) return;
 memcpy(s31_sm_pm_beacon.bssid, frame + 16, 6);
 s31_sm_pm_beacon.aid = s31_sm_coex_aid;
 s31_sm_pm_beacon.beacon_tu = (uint16_t)frame[32] | (uint16_t)frame[33] << 8;
 memcpy(&s31_sm_pm_beacon.tsf_low, frame + 24, 4);
 memcpy(&s31_sm_pm_beacon.tsf_high, frame + 28, 4);
 s31_sm_pm_beacon.local_stamp = (uint32_t)esp_timer_get_time();
 int rc = esp_wifi_ipc_internal(&request, true);
 if (rc) s31_linux_printf("[S31-SM] coex PM owner dispatch failed rc=%d\n", rc);
}
