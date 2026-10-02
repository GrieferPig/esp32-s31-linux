/* SPDX-License-Identifier: BSD-2-Clause */
/* Value-only TX completion: never retain a native frame or EB pointer. */
static void s31_sm_report(uint32_t cookie, bool success, uint8_t rate, uint8_t attempts)
{
 struct s31_softmac_tx_status status = {
  .cookie = cookie, .success = success, .rate = rate, .attempts = attempts,
 };
 s31_radio_wifi_receive_aux(S31_WIFI_IF_TX_STATUS, (const uint8_t *)&status,
                            sizeof(status), 0, 0);
}
