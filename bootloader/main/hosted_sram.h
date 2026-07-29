/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef int (*s31_hosted_frame_handler_t)(const uint8_t *frame,
					 size_t frame_length, void *arg);

esp_err_t s31_hosted_sram_start(void);
int s31_hosted_sram_send(uint8_t if_type, const void *payload, size_t length,
			 uint8_t hci_packet_type);
int s31_hosted_sram_send_meta(uint8_t if_type, uint8_t if_num,
			      const void *payload, size_t length, uint8_t flags,
			      uint16_t seq_num, uint8_t packet_type);
void s31_hosted_sram_set_frame_handler(s31_hosted_frame_handler_t handler,
				       void *arg);
int s31_hosted_sram_wifi_tx(const void *data, size_t length);
int s31_hosted_sram_ap_tx(const void *data, size_t length);
int s31_hosted_sram_hci_tx(const void *data, size_t length);
void s31_hosted_sram_set_features(uint32_t features);
void s31_hosted_sram_set_wifi_state(uint32_t state);
void s31_hosted_sram_set_sta_mac(const uint8_t mac[6]);
void s31_hosted_sram_set_bt_mac(const uint8_t mac[6]);
