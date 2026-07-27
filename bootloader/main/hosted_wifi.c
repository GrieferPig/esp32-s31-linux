/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ESP32-S31 Hosted WiFi driver for FreeRTOS (hart0).
 *
 * Initializes Wi-Fi stack, connects to AP based on config received from
 * Linux, and forwards data frames between the Wi-Fi interface and the
 * shared SRAM transport.
 *
 * Usage:
 *   From main:  hosted_wifi_init();
 *   From transport rx (weak override): hosted_wifi_rx_handler(data, len) →
 *       forwards ethernet frames to esp_wifi_internal_tx().
 *   When Wi-Fi receives a frame → esp_wifi_internal_reg_rxcb() callback →
 *       s31_hosted_sram_wifi_tx().
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "hosted_sram.h"
#include "s31_hosted_sram.h"

#define HOSTED_WIFI_EVENT_BIT_CONNECTED  BIT0
#define HOSTED_WIFI_EVENT_BIT_GOT_IP     BIT1
#define HOSTED_WIFI_EVENT_BIT_DISCONNECTED BIT2

static const char *TAG = "hosted_wifi";
static bool s_wifi_initialized;
static bool s_wifi_connected;
static uint8_t s_sta_mac[6];
static EventGroupHandle_t s_wifi_event_group;

/* Forward Wi-Fi data to Linux transport. */
static esp_err_t hosted_wifi_sta_rx_cb(void *buffer, uint16_t len, void *eb)
{
	if (!s_wifi_connected) {
		esp_wifi_internal_free_rx_buffer(eb);
		return ESP_OK;
	}

	(void)s31_hosted_sram_wifi_tx(buffer, len);
	esp_wifi_internal_free_rx_buffer(eb);
	return ESP_OK;
}

/* Weak override: receive data from Linux, forward to Wi-Fi.
 * Called from hosted_sram.c process_h1_frame() for STA_IF frames.
 */
void hosted_wifi_rx_handler(const uint8_t *data, size_t len)
{
	if (!s_wifi_initialized || !s_wifi_connected)
		return;

	/* esp_wifi_internal_tx() needs a Wi-Fi buffer; use a simple internal
	 * TX for ethernet frames.  On S31 the lower-level call is
	 * esp_wifi_internal_tx_by_ref(WIFI_IF_STA, data, len, NULL).
	 * For now we drop untagged frames if Wi-Fi is not fully up.
	 */
	esp_wifi_internal_tx(WIFI_IF_STA, (void *)data, len);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
			       int32_t event_id, void *event_data)
{
	if (event_base == WIFI_EVENT) {
		switch (event_id) {
		case WIFI_EVENT_STA_START:
			esp_wifi_connect();
			break;
		case WIFI_EVENT_STA_CONNECTED:
			ESP_LOGI(TAG, "Wi-Fi connected");
			xEventGroupSetBits(s_wifi_event_group,
					   HOSTED_WIFI_EVENT_BIT_CONNECTED);
			s_wifi_connected = true;
			s31_hosted_sram_set_wifi_state(1);
			break;
		case WIFI_EVENT_STA_DISCONNECTED:
			ESP_LOGW(TAG, "Wi-Fi disconnected");
			s_wifi_connected = false;
			if (s_wifi_event_group) {
				xEventGroupClearBits(s_wifi_event_group,
						HOSTED_WIFI_EVENT_BIT_CONNECTED);
				xEventGroupSetBits(s_wifi_event_group,
						HOSTED_WIFI_EVENT_BIT_DISCONNECTED);
			}
			s31_hosted_sram_set_wifi_state(0);
			break;
		default:
			break;
		}
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ESP_LOGI(TAG, "Got IP address");
		xEventGroupSetBits(s_wifi_event_group,
				   HOSTED_WIFI_EVENT_BIT_GOT_IP);
	}
}

esp_err_t hosted_wifi_init(void)
{
	esp_err_t ret;

	if (s_wifi_initialized)
		return ESP_OK;

	s_wifi_event_group = xEventGroupCreate();
	if (!s_wifi_event_group)
		return ESP_ERR_NO_MEM;

	/* Initialize NVS for Wi-Fi PHY calibration data. */
	ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
	    ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		nvs_flash_erase();
		ret = nvs_flash_init();
	}
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
		goto fail_event;
	}

	/* Initialize TCP/IP stack and default Wi-Fi station interface. */
	ESP_ERROR_CHECK(esp_netif_init());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ret = esp_wifi_init(&cfg);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
		goto fail_event;
	}

	esp_wifi_set_storage(WIFI_STORAGE_RAM);

	ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
						  &wifi_event_handler, NULL,
						  NULL);
	ret |= esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
						   &wifi_event_handler, NULL,
						   NULL);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "event handler register failed");
		goto fail_wifi;
	}

	esp_wifi_set_mode(WIFI_MODE_STA);

	/* Register RX callback: forward Wi-Fi frames to Linux. */
	esp_wifi_internal_reg_rxcb(WIFI_IF_STA, hosted_wifi_sta_rx_cb);

	esp_wifi_start();

	esp_read_mac(s_sta_mac, ESP_MAC_WIFI_STA);
	s31_hosted_sram_set_sta_mac(s_sta_mac);
	ESP_LOGI(TAG, "STA MAC: %02x:%02x:%02x:%02x:%02x:%02x",
		 s_sta_mac[0], s_sta_mac[1], s_sta_mac[2],
		 s_sta_mac[3], s_sta_mac[4], s_sta_mac[5]);

	s_wifi_initialized = true;
	s31_hosted_sram_set_features(S31_HOSTED_FEAT_WIFI);
	return ESP_OK;

fail_wifi:
	esp_wifi_deinit();
fail_event:
	vEventGroupDelete(s_wifi_event_group);
	s_wifi_event_group = NULL;
	return ret;
}

bool hosted_wifi_is_connected(void)
{
	return s_wifi_connected;
}
