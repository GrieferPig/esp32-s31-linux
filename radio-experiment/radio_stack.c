/* SPDX-License-Identifier: BSD-2-Clause */
#include <stdint.h>
#include "esp_bt.h"
#include "esp_wifi.h"
#include "private/esp_coexist_adapter.h"
#include "private/esp_coexist_internal.h"

extern int esp_rom_printf(const char *fmt, ...);
extern void s31_rtos_use_internal_stacks(void);
#ifdef S31_LINUX_SMODE
extern void s31_radio_heap_report(const char *stage);
extern void s31_radio_report_wifi_init(int result);
extern void s31_radio_report_bt_init(int result);
extern void s31_radio_report_bt_enable(int result);
#endif

void s31_radio_stack_task(void *arg)
{
	wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
	int rc;

	(void)arg;
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
	s31_radio_report_bt_enable(rc);
	s31_radio_heap_report("after-bt-enable");
	#endif
}
