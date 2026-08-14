/* SPDX-License-Identifier: BSD-2-Clause */
#include <stdint.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp32s31/rom/ets_sys.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define S31_NATIVE_CONNECTED BIT0
#define S31_NATIVE_GOT_IP    BIT1
#define S31_NATIVE_FAILED    BIT2

static const char *TAG = "native-wifi";
static EventGroupHandle_t s31_native_events;
static unsigned int s31_native_retries;

extern int esp_test_get_hw_rx_statistics(void *stats);

static uint32_t s31_native_fnv1a(const uint8_t *data, size_t len)
{
    uint32_t hash = 2166136261u;
    size_t i;

    for (i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static void s31_native_dump_reg_range(const char *stage, uint32_t start,
					 uint32_t end)
{
	volatile const uint32_t *word = (volatile const uint32_t *)start;
	uint32_t addr;

	for (addr = start; addr < end; addr += 4) {
		if (*word)
			esp_rom_printf("NATIVE %s REG %08lx=%08lx\r\n",
				       stage, (unsigned long)addr,
				       (unsigned long)*word);
		word++;
	}
}

static void s31_native_dump_state(const char *stage)
{
    volatile const uint32_t *crypto = (volatile const uint32_t *)0x20104800;
    uint16_t stats[48] = { 0 };
    uint32_t slot;
    uint32_t i;
    int rc;

    ESP_LOGI(TAG, "%s CRYPTO c0=%08lx c1=%08lx c2=%08lx c3=%08lx cfg=%08lx valid=%08lx",
             stage, (unsigned long)crypto[0], (unsigned long)crypto[1],
             (unsigned long)crypto[2], (unsigned long)crypto[3],
             (unsigned long)crypto[4], (unsigned long)crypto[5]);
    esp_rom_printf("NATIVE %s CRYPTO %08lx %08lx %08lx %08lx %08lx %08lx\r\n",
                   stage, (unsigned long)crypto[0], (unsigned long)crypto[1],
                   (unsigned long)crypto[2], (unsigned long)crypto[3],
                   (unsigned long)crypto[4], (unsigned long)crypto[5]);
    for (slot = 0; slot < 6; slot++) {
        volatile const uint32_t *entry;
        uint32_t hash;

        if (!(crypto[5] & BIT(slot)))
            continue;
        entry = (volatile const uint32_t *)(0x20105800 + slot * 40);
        hash = s31_native_fnv1a((const uint8_t *)(entry + 2), 16);
        ESP_LOGI(TAG, "%s KEYS slot=%lu hdr0=%08lx hdr1=%08lx keyfnv=%08lx",
                 stage, (unsigned long)slot, (unsigned long)entry[0],
                 (unsigned long)entry[1], (unsigned long)hash);
        esp_rom_printf("NATIVE %s KEY %lu %08lx %08lx %08lx\r\n", stage,
                       (unsigned long)slot, (unsigned long)entry[0],
                       (unsigned long)entry[1], (unsigned long)hash);
    }
    s31_native_dump_reg_range(stage, 0x20104600, 0x20104900);
    s31_native_dump_reg_range(stage, 0x20104c00, 0x20104d00);
    rc = esp_test_get_hw_rx_statistics(stats);
    ESP_LOGI(TAG, "%s RXSTAT rc=%d", stage, rc);
    for (i = 0; i < 37; i++)
        ESP_LOGI(TAG, "%s RXSTAT %lu:%u", stage, (unsigned long)i, stats[i]);
    ESP_LOGI(TAG, "%s RXSTAT w38=%08lx w40=%08lx", stage,
             (unsigned long)*(uint32_t *)((uint8_t *)stats + 76),
             (unsigned long)*(uint32_t *)((uint8_t *)stats + 80));
    esp_rom_printf("NATIVE %s RXSTAT rc=%d", stage, rc);
    for (i = 0; i < 37; i++)
        esp_rom_printf(" %lu:%u", (unsigned long)i, stats[i]);
    esp_rom_printf("\r\n");
}

static void s31_native_event(void *arg, esp_event_base_t base, int32_t id,
                             void *event_data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *event = event_data;

        ESP_LOGI(TAG, "connected bssid=" MACSTR " channel=%u aid=%u",
                 MAC2STR(event->bssid), event->channel, event->aid);
        esp_rom_printf("NATIVE connected %02x:%02x:%02x:%02x:%02x:%02x ch=%u aid=%u\r\n",
                       event->bssid[0], event->bssid[1], event->bssid[2],
                       event->bssid[3], event->bssid[4], event->bssid[5],
                       event->channel, event->aid);
        xEventGroupSetBits(s31_native_events, S31_NATIVE_CONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = event_data;

        ESP_LOGW(TAG, "disconnected reason=%u", event->reason);
        esp_rom_printf("NATIVE disconnected reason=%u\r\n", event->reason);
        if (++s31_native_retries < 8)
            esp_wifi_connect();
        else
            xEventGroupSetBits(s31_native_events, S31_NATIVE_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;

        ESP_LOGI(TAG, "got ip=" IPSTR " gateway=" IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
        esp_rom_printf("NATIVE got_ip %lu gateway %lu\r\n",
                       (unsigned long)event->ip_info.ip.addr,
                       (unsigned long)event->ip_info.gw.addr);
        xEventGroupSetBits(s31_native_events, S31_NATIVE_GOT_IP);
    }
}

void __attribute__((noreturn)) s31_native_wifi_probe(void)
{
    static const wifi_country_t country = {
        .cc = "CN",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t config = { 0 };
    EventBits_t bits;

    ESP_LOGI(TAG, "starting native FreeRTOS/IDF comparison probe");
    esp_rom_printf("NATIVE probe start\r\n");
    s31_native_events = xEventGroupCreate();
    ESP_ERROR_CHECK(s31_native_events ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() ? ESP_OK : ESP_FAIL);

    init.static_rx_buf_num = 16;
    init.dynamic_rx_buf_num = 40;
    init.rx_ba_win = 32;
    init.nvs_enable = 0;
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               s31_native_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               s31_native_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
                                         WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                                         WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20));
    memcpy(config.sta.ssid, "ChinaNet-38D07C", sizeof("ChinaNet-38D07C") - 1);
    memcpy(config.sta.password, "AH3s0564ZhF", sizeof("AH3s0564ZhF") - 1);
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    bits = xEventGroupWaitBits(s31_native_events,
                               S31_NATIVE_CONNECTED | S31_NATIVE_FAILED,
                               pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if (bits & S31_NATIVE_CONNECTED)
        s31_native_dump_state("CONNECTED");
    else {
        ESP_LOGE(TAG, "association failed bits=%08lx", (unsigned long)bits);
        esp_rom_printf("NATIVE association_failed bits=%08lx\r\n",
                       (unsigned long)bits);
    }

    bits = xEventGroupWaitBits(s31_native_events,
                               S31_NATIVE_GOT_IP | S31_NATIVE_FAILED,
                               pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if (bits & S31_NATIVE_GOT_IP)
        s31_native_dump_state("GOT_IP");
    else {
        ESP_LOGE(TAG, "DHCP failed bits=%08lx", (unsigned long)bits);
        esp_rom_printf("NATIVE dhcp_failed bits=%08lx\r\n",
                       (unsigned long)bits);
    }

    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
