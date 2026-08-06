/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/hp_system_reg.h"
#include "soc/interrupts.h"
#include "soc/rtc.h"
#include "soc/soc.h"

#include "hosted_sram.h"
#include "s31_hosted_sram.h"

static const char *TAG = "hosted_sram";
static volatile struct s31_hosted_control *const s_ctrl =
	(void *)S31_HOSTED_SRAM_BASE;
static volatile struct s31_hosted_slot *const s_h0_to_h1 =
	(void *)(S31_HOSTED_SRAM_BASE + S31_HOSTED_H0_TO_H1_OFFSET);
static volatile struct s31_hosted_slot *const s_h1_to_h0 =
	(void *)(S31_HOSTED_SRAM_BASE + S31_HOSTED_H1_TO_H0_OFFSET);
static portMUX_TYPE s_tx_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_rx_task;
static intr_handle_t s_h1_irq;
static uint16_t s_tx_sequence;
static s31_hosted_frame_handler_t s_frame_handler;
static void *s_frame_handler_arg;
static bool s_clock_test_active;
static uint32_t s_clock_test_cookie;
static uint32_t s_clock_test_duration;
static int64_t s_clock_test_deadline_us;

#define S31_PM_MIN_FREQ_MHZ 53

static inline void shared_wmb(void)
{
	__asm__ volatile("fence rw, rw" ::: "memory");
}

static inline void shared_rmb(void)
{
	__asm__ volatile("fence r, rw" ::: "memory");
}

static inline void shared_invalidate(const volatile void *address, size_t size)
{
	/*
	 * Internal HP SRAM is directly addressed, and both HP harts share the
	 * unified L1 data cache.  The CACHE_SYNC engine is for cached external
	 * aliases; applying it to 0x2f... SRAM can discard a peer's publication.
	 */
	(void)address;
	(void)size;
	shared_rmb();
}

static inline void shared_writeback(const volatile void *address, size_t size)
{
	(void)address;
	(void)size;
	shared_wmb();
}

static uint16_t frame_checksum(const uint8_t *frame, size_t length)
{
	uint16_t checksum = 0;

	while (length--)
		checksum += *frame++;
	return checksum;
}

static void notify_hart1(void)
{
	shared_wmb();
	REG_WRITE(HP_SYSTEM_CPU_INT_FROM_CPU_2_REG,
		  HP_SYSTEM_CPU_INT_FROM_CPU_2);
}

static void IRAM_ATTR h1_doorbell_isr(void *arg)
{
	BaseType_t wake = pdFALSE;

	(void)arg;
	REG_WRITE(HP_SYSTEM_CPU_INT_FROM_CPU_3_REG, 0);
	s_ctrl->h0_irq_count++;
	shared_wmb();
	if (s_rx_task)
		vTaskNotifyGiveFromISR(s_rx_task, &wake);
	if (wake)
		portYIELD_FROM_ISR();
}

int s31_hosted_sram_send_meta(uint8_t if_type, uint8_t if_num,
			      const void *payload, size_t length, uint8_t flags,
			      uint16_t seq_num, uint8_t packet_type)
{
	volatile struct s31_hosted_ring_state *ring = &s_ctrl->h0_to_h1;
	volatile struct s31_hosted_slot *slot;
	struct s31_esp_payload_header header = { 0 };
	size_t frame_length = sizeof(header) + length;
	uint32_t producer;
	uint32_t consumer;
	uint32_t index;

	if (!payload || !length || frame_length > S31_HOSTED_SLOT_DATA_SIZE)
		return -1;

	header.if_type = if_type;
	header.if_num = if_num;
	header.flags = flags;
	header.len = length;
	header.offset = sizeof(header);
	header.hci_pkt_type = packet_type;

	portENTER_CRITICAL(&s_tx_lock);
	header.seq_num = seq_num ? seq_num : ++s_tx_sequence;
	producer = ring->producer;
	consumer = ring->consumer;
	if (producer - consumer >= S31_HOSTED_SLOT_COUNT) {
		ring->drops++;
		portEXIT_CRITICAL(&s_tx_lock);
		return -1;
	}

	index = producer & (S31_HOSTED_SLOT_COUNT - 1);
	slot = &s_h0_to_h1[index];
	memcpy((void *)slot->data, &header, sizeof(header));
	memcpy((void *)(slot->data + sizeof(header)), payload, length);
	((struct s31_esp_payload_header *)(void *)slot->data)->checksum =
		frame_checksum((const uint8_t *)(const void *)slot->data,
			       frame_length);
	slot->length = frame_length;
	slot->flags = 0;
	/* Publish sequence last; producer is the final ring commit. */
	shared_wmb();
	slot->sequence = producer + 1;
	shared_writeback(slot, offsetof(struct s31_hosted_slot, data) +
			 frame_length);
	ring->producer = producer + 1;
	shared_writeback(&ring->producer, sizeof(ring->producer));
	portEXIT_CRITICAL(&s_tx_lock);

	notify_hart1();
	return 0;
}

int s31_hosted_sram_send(uint8_t if_type, const void *payload, size_t length,
			 uint8_t hci_packet_type)
{
	return s31_hosted_sram_send_meta(if_type, 0, payload, length, 0, 0,
					 hci_packet_type);
}

void s31_hosted_sram_set_frame_handler(s31_hosted_frame_handler_t handler,
				       void *arg)
{
	portENTER_CRITICAL(&s_tx_lock);
	s_frame_handler_arg = arg;
	shared_wmb();
	s_frame_handler = handler;
	portEXIT_CRITICAL(&s_tx_lock);
}

static void send_control(uint8_t type, uint8_t value)
{
	struct s31_hosted_control_msg msg = {
		.type = type,
		.value = value,
		.length = sizeof(msg),
		.generation = s_ctrl->generation,
	};

	(void)s31_hosted_sram_send(S31_HOSTED_PRIV_IF, &msg, sizeof(msg), 0);
}

static int send_clock_stamp(uint8_t type, uint32_t cookie,
			    uint32_t duration_sec, uint64_t timestamp_us)
{
	struct s31_hosted_control_msg msg = {
		.type = type,
		.length = sizeof(msg),
		.generation = s_ctrl->generation,
	};
	struct s31_hosted_clock_stamp stamp = {
		.cookie = cookie,
		.duration_sec = duration_sec,
		.freertos_us = timestamp_us,
	};

	memcpy(msg.data, &stamp, sizeof(stamp));
	return s31_hosted_sram_send(S31_HOSTED_PRIV_IF, &msg, sizeof(msg), 0);
}

static void clock_test_poll(void)
{
	int64_t now;

	if (!s_clock_test_active)
		return;
	now = esp_timer_get_time();
	if (now < s_clock_test_deadline_us)
		return;
	/* Keep retrying if the outbound ring happens to be full at expiry. */
	if (!send_clock_stamp(S31_HOSTED_CTRL_CLOCK_STOP,
			      s_clock_test_cookie, s_clock_test_duration, now))
		s_clock_test_active = false;
}

static bool process_clock_control(const struct s31_hosted_control_msg *msg)
{
	struct s31_hosted_clock_stamp stamp;
	int64_t now;

	if (msg->type != S31_HOSTED_CTRL_CLOCK_START)
		return false;
	memcpy(&stamp, msg->data, sizeof(stamp));
	if (!stamp.duration_sec || stamp.duration_sec > 600)
		return true;
	now = esp_timer_get_time();
	if (send_clock_stamp(S31_HOSTED_CTRL_CLOCK_START, stamp.cookie,
			     stamp.duration_sec, now))
		return true;
	s_clock_test_cookie = stamp.cookie;
	s_clock_test_duration = stamp.duration_sec;
	s_clock_test_deadline_us =
		now + (int64_t)stamp.duration_sec * 1000000;
	s_clock_test_active = true;
	return true;
}

static bool process_mem_stats_control(const struct s31_hosted_control_msg *msg)
{
	const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA |
			      MALLOC_CAP_8BIT;
	struct s31_hosted_control_msg response = {
		.type = S31_HOSTED_CTRL_MEM_STATS_RESPONSE,
		.length = sizeof(response),
		.generation = s_ctrl->generation,
	};
	struct s31_hosted_mem_stats stats;

	if (msg->type != S31_HOSTED_CTRL_MEM_STATS_REQUEST)
		return false;

	/* MALLOC_CAP_DMA excludes LP RAM and PSRAM on ESP32-S31. */
	stats.total_bytes = heap_caps_get_total_size(caps);
	stats.free_bytes = heap_caps_get_free_size(caps);
	stats.minimum_free_bytes = heap_caps_get_minimum_free_size(caps);
	stats.largest_free_block = heap_caps_get_largest_free_block(caps);
	memcpy(response.data, &stats, sizeof(stats));
	(void)s31_hosted_sram_send(S31_HOSTED_PRIV_IF, &response,
				   sizeof(response), 0);
	return true;
}

static bool process_cpu_freq_control(const struct s31_hosted_control_msg *msg)
{
	struct s31_hosted_cpu_freq_msg request;
	struct s31_hosted_cpu_freq_msg response_data = { 0 };
	struct s31_hosted_control_msg response = {
		.type = S31_HOSTED_CTRL_CPU_FREQ_SET_RESPONSE,
		.length = sizeof(response),
		.generation = s_ctrl->generation,
	};
	rtc_cpu_freq_config_t actual_config;
	esp_pm_config_t pm_config;
	esp_err_t err;

	if (msg->type != S31_HOSTED_CTRL_CPU_FREQ_SET)
		return false;
	memcpy(&request, msg->data, sizeof(request));
	response_data.target_mhz = request.target_mhz;
	if (!rtc_clk_cpu_freq_mhz_to_config(request.target_mhz, &actual_config)) {
		response_data.status = ESP_ERR_INVALID_ARG;
		goto send_response;
	}

	/* Linux changes only the PM floor; FreeRTOS task activity owns the mode. */
	err = esp_pm_get_configuration(&pm_config);
	if (err != ESP_OK) {
		response_data.status = err;
		goto send_response;
	}
	if (request.target_mhz > (uint32_t)pm_config.max_freq_mhz) {
		response_data.status = ESP_ERR_INVALID_ARG;
		goto send_response;
	}
	pm_config.min_freq_mhz = request.target_mhz;
	err = esp_pm_configure(&pm_config);
	if (err != ESP_OK) {
		response_data.status = err;
		goto send_response;
	}

	rtc_clk_cpu_freq_get_config(&actual_config);
	response_data.actual_mhz = actual_config.freq_mhz;
	response_data.status = ESP_OK;

send_response:
	memcpy(response.data, &response_data, sizeof(response_data));
	(void)s31_hosted_sram_send(S31_HOSTED_PRIV_IF, &response,
				   sizeof(response), 0);
	return true;
}

bool __attribute__((weak)) hosted_wifi_config_handler(const uint8_t *data,
						 size_t len)
{
	(void)data;
	(void)len;
	return false;
}

void __attribute__((weak)) hosted_wifi_rx_handler(const uint8_t *data,
							size_t len)
{
	/* Weak default: no-op.  Override in WiFi module to forward to esp_wifi. */
	(void)data;
	(void)len;
}

void __attribute__((weak)) hosted_ap_rx_handler(const uint8_t *data,
						       size_t len)
{
	(void)data;
	(void)len;
}

void __attribute__((weak)) hosted_hci_rx_handler(const uint8_t *data,
						       size_t len)
{
	(void)data;
	(void)len;
}

static void process_h1_frame(const uint8_t *frame, size_t frame_length)
{
	struct s31_esp_payload_header *header = (void *)frame;
	const struct s31_hosted_control_msg *msg;
	uint16_t received_checksum;
	uint16_t offset;
	uint16_t length;

	if (frame_length < sizeof(*header))
		return;
	received_checksum = header->checksum;
	header->checksum = 0;
	if (frame_checksum(frame, frame_length) != received_checksum) {
		header->checksum = received_checksum;
		return;
	}
	header->checksum = received_checksum;
	offset = header->offset;
	length = header->len;
	if (offset < sizeof(*header) || offset + length > frame_length)
		return;
	if (header->if_type == S31_HOSTED_PRIV_IF &&
	    length >= sizeof(struct s31_hosted_control_msg)) {
		if (hosted_wifi_config_handler(frame + offset, length))
			return;
		msg = (const void *)(frame + offset);
		if (process_cpu_freq_control(msg))
			return;
		if (process_clock_control(msg))
			return;
		if (process_mem_stats_control(msg))
			return;
	}

	/*
	 * TEST_IF remains owned by the transport so the Linux probe can validate
	 * both rings independently of the Hosted control plane.
	 */
	if (header->if_type != S31_HOSTED_TEST_IF && s_frame_handler &&
	    s_frame_handler(frame, frame_length, s_frame_handler_arg))
		return;

	switch (header->if_type) {
	case S31_HOSTED_PRIV_IF:
		if (length < sizeof(struct s31_hosted_control_msg))
			return;
		msg = (const void *)(frame + offset);
		switch (msg->type) {
		case S31_HOSTED_CTRL_PING:
			ESP_LOGI(TAG, "Linux transport ping generation=%" PRIu32,
				 msg->generation);
			send_control(S31_HOSTED_CTRL_PONG, 0);
			break;
		case S31_HOSTED_CTRL_LINK:
			/* Link state change forwarded to upper layers. */
			ESP_LOGI(TAG, "link state: %s",
				 msg->value == S31_HOSTED_LINK_UP ?
				 "up" : "down");
			break;
		default:
			break;
		}
		break;
	case S31_HOSTED_STA_IF:
		hosted_wifi_rx_handler(frame + offset, length);
		break;
	case S31_HOSTED_AP_IF:
		hosted_ap_rx_handler(frame + offset, length);
		break;
	case S31_HOSTED_HCI_IF:
		hosted_hci_rx_handler(frame + offset, length);
		break;
	case S31_HOSTED_TEST_IF:
		/* Linux probe uses TEST_IF to exercise both rings at MTU size. */
		(void)s31_hosted_sram_send(S31_HOSTED_TEST_IF,
					   frame + offset, length, 0);
		break;
	default:
		break;
	}
}

int s31_hosted_sram_wifi_tx(const void *data, size_t length)
{
	return s31_hosted_sram_send(S31_HOSTED_STA_IF, data, length, 0);
}

int s31_hosted_sram_ap_tx(const void *data, size_t length)
{
	return s31_hosted_sram_send(S31_HOSTED_AP_IF, data, length, 0);
}

int s31_hosted_sram_hci_tx(const void *data, size_t length)
{
	return s31_hosted_sram_send(S31_HOSTED_HCI_IF, data, length, 0);
}

static void drain_h1_ring(void)
{
	volatile struct s31_hosted_ring_state *ring = &s_ctrl->h1_to_h0;

	for (;;) {
		volatile struct s31_hosted_slot *slot;
		uint8_t frame[S31_HOSTED_SLOT_DATA_SIZE];
		uint32_t consumer = ring->consumer;
		uint32_t producer;
		uint32_t index;
		uint16_t length;

		shared_rmb();
		shared_invalidate(&ring->producer, sizeof(ring->producer));
		producer = ring->producer;
		s_ctrl->h0_seen_h1_producer = producer;
		if (consumer == producer)
			break;
		if (producer - consumer > S31_HOSTED_SLOT_COUNT) {
			ring->drops++;
			ring->consumer = producer;
			shared_wmb();
			break;
		}

		index = consumer & (S31_HOSTED_SLOT_COUNT - 1);
		slot = &s_h1_to_h0[index];
		shared_invalidate(slot, sizeof(*slot));
		s_ctrl->h0_seen_h1_sequence = slot->sequence;
		length = slot->length;
		if (slot->sequence == consumer + 1 &&
		    length && length <= sizeof(frame)) {
			memcpy(frame, (const void *)slot->data, length);
			process_h1_frame(frame, length);
		} else {
			ring->drops++;
		}
		shared_wmb();
		ring->consumer = consumer + 1;
		shared_writeback(&ring->consumer, sizeof(ring->consumer));
	}
}

static void hosted_rx_task(void *arg)
{
	(void)arg;

	for (;;) {
		uint32_t system_request = s_ctrl->h0_h1_doorbell;

		if (system_request) {
			s_ctrl->h0_h1_doorbell = 0;
			shared_wmb();
			if (system_request == S31_HOSTED_CTRL_POWER_OFF) {
				ESP_LOGI(TAG, "OpenSBI requested system power off");
				esp_deep_sleep_start();
			}
			if (system_request == S31_HOSTED_CTRL_RESTART) {
				ESP_LOGI(TAG, "OpenSBI requested system restart");
				esp_restart();
			}
		}

		s_ctrl->h0_rx_polls++;
		drain_h1_ring();
		clock_test_poll();

		/*
		 * Linux may disable the interrupt fabric and stop the FreeRTOS tick
		 * while rebooting.  Do not block on a notification or a tick timeout:
		 * continuously poll so OpenSBI system requests are always observed.
		 */
		REG_WRITE(HP_SYSTEM_CPU_INT_FROM_CPU_3_REG, 0);
		__asm__ volatile("nop");
	}
}

void s31_hosted_sram_set_features(uint32_t features)
{
	s_ctrl->features = features;
	shared_wmb();
}

void s31_hosted_sram_set_wifi_state(uint32_t state)
{
	s_ctrl->wifi_state = state;
	shared_wmb();
	send_control(S31_HOSTED_CTRL_LINK, state ? S31_HOSTED_LINK_UP :
		     S31_HOSTED_LINK_DOWN);
}

void s31_hosted_sram_set_sta_mac(const uint8_t mac[6])
{
	memcpy((void *)s_ctrl->sta_mac, mac, 6);
	shared_wmb();
}

void s31_hosted_sram_set_bt_mac(const uint8_t mac[6])
{
	memcpy((void *)s_ctrl->bt_mac, mac, 6);
	shared_wmb();
}

esp_err_t s31_hosted_sram_start(void)
{
	uint32_t generation = 1;
	esp_pm_config_t pm_config = {
		.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
		.min_freq_mhz = S31_PM_MIN_FREQ_MHZ,
		.light_sleep_enable = false,
	};
	esp_err_t err;

	err = esp_pm_configure(&pm_config);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "failed to configure ESP PM: %s", esp_err_to_name(err));
		return err;
	}

	if (s_ctrl->magic == S31_HOSTED_MAGIC &&
	    s_ctrl->abi_version == S31_HOSTED_ABI_VERSION)
		generation = s_ctrl->generation + 1;
	memset((void *)s_ctrl, 0, S31_HOSTED_SRAM_SIZE);
	s_ctrl->magic = S31_HOSTED_MAGIC;
	s_ctrl->abi_version = S31_HOSTED_ABI_VERSION;
	s_ctrl->generation = generation ? generation : 1;
	s_ctrl->state = S31_HOSTED_H0_READY;
	/* Publish and clean every line dirtied by memset before hart1 starts. */
	shared_writeback(s_ctrl, S31_HOSTED_SRAM_SIZE);

	REG_WRITE(HP_SYSTEM_CPU_INT_FROM_CPU_2_REG, 0);
	REG_WRITE(HP_SYSTEM_CPU_INT_FROM_CPU_3_REG, 0);
	err = esp_intr_alloc(ETS_CPU_INTR_FROM_CPU_3_SOURCE,
			      ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_INTRDISABLED,
			      h1_doorbell_isr, NULL, &s_h1_irq);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "failed to allocate Linux doorbell IRQ");
		return err;
	}

	if (xTaskCreatePinnedToCore(hosted_rx_task, "hosted_rx", 4096, NULL, 1,
				    &s_rx_task, 0) != pdPASS) {
		esp_intr_free(s_h1_irq);
		s_h1_irq = NULL;
		return ESP_ERR_NO_MEM;
	}
	err = esp_intr_enable(s_h1_irq);
	if (err != ESP_OK) {
		vTaskDelete(s_rx_task);
		s_rx_task = NULL;
		esp_intr_free(s_h1_irq);
		s_h1_irq = NULL;
		return err;
	}

	ESP_LOGI(TAG, "SRAM transport ready: 0x%08x..0x%08x generation=%" PRIu32,
		 S31_HOSTED_SRAM_BASE,
		 S31_HOSTED_SRAM_BASE + S31_HOSTED_SRAM_SIZE,
		 s_ctrl->generation);
	send_control(S31_HOSTED_CTRL_RADIO_READY, 0);
	return ESP_OK;
}
