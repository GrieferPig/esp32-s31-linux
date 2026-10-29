/* SPDX-License-Identifier: BSD-2-Clause */
/* New-runtime boundary only: no old tasks, work or IRQ callbacks survive the
 * previous private heap's retirement. The pinned Wi-Fi init creates its ROM
 * recursive mutex only when this slot is NULL, and deinit leaves it retained.
 * Never inspect/delete that stale handle: its backing bytes may be reclaimed
 * or reused already. Let the current adapter construct a new registered mutex.
 * Clear the adapter slot too; ROM registration otherwise retains old callbacks.
 */
static void s31_wifi_rom_runtime_begin(void)
{
    g_wifi_global_lock = NULL;
    g_osi_funcs_p = NULL;
}
