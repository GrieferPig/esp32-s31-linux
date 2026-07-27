/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp32s31/rom/cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/hp_system_reg.h"
#include "soc/hp_mem_apm_reg.h"
#include "soc/cpu_apm_reg.h"
#include "soc/interrupts.h"
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

esp_err_t s31_hosted_sram_reenable_irq(void)
{
	if (!s_h1_irq)
		return ESP_ERR_INVALID_STATE;

	return esp_intr_enable(s_h1_irq);
}

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
	uintptr_t start = (uintptr_t)address & ~(uintptr_t)63;
	uintptr_t end = ((uintptr_t)address + size + 63) & ~(uintptr_t)63;

	/*
	 * Use writeback-invalidate instead of plain invalidate: on the
	 * shared D-cache this is safe against stale dirty lines that a
	 * pure invalidate would discard without writing back.
	 */
	(void)Cache_WriteBack_Invalidate_Addr(CACHE_MAP_L1_DCACHE, start,
					      end - start);
	shared_rmb();
}

static inline void shared_writeback(const volatile void *address, size_t size)
{
	uintptr_t start = (uintptr_t)address & ~(uintptr_t)63;
	uintptr_t end = ((uintptr_t)address + size + 63) & ~(uintptr_t)63;

	shared_wmb();
	(void)Cache_WriteBack_Addr(CACHE_MAP_L1_DCACHE, start, end - start);
	shared_wmb();
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
	if (s_rx_task)
		vTaskNotifyGiveFromISR(s_rx_task, &wake);
	if (wake)
		portYIELD_FROM_ISR();
}

int s31_hosted_sram_send(uint8_t if_type, const void *payload, size_t length,
			 uint8_t hci_packet_type)
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
	header.len = length;
	header.offset = sizeof(header);
	header.hci_pkt_type = hci_packet_type;

	portENTER_CRITICAL(&s_tx_lock);
	header.seq_num = ++s_tx_sequence;
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
	slot->length = frame_length;
	slot->flags = 0;
	slot->sequence = producer + 1;
	shared_writeback(slot, frame_length + offsetof(struct s31_hosted_slot,
						      data));
	ring->producer = producer + 1;
	shared_writeback(&ring->producer, sizeof(ring->producer));
	portEXIT_CRITICAL(&s_tx_lock);

	notify_hart1();
	return 0;
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
	const struct s31_esp_payload_header *header = (const void *)frame;
	const struct s31_hosted_control_msg *msg;
	uint16_t offset;
	uint16_t length;

	if (frame_length < sizeof(*header))
		return;
	offset = header->offset;
	length = header->len;
	if (offset < sizeof(*header) || offset + length > frame_length)
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

		/*
		 * Writeback + invalidate the entire D-cache before reading
		 * the ring producer.  S31 has a shared 64 KiB D-cache, so
		 * this is a heavy hammer but it guarantees we see hart1's
		 * writes even if earlier cache ops silently dropped a line.
		 */
		(void)Cache_WriteBack_Invalidate_Addr(CACHE_MAP_L1_DCACHE,
						      S31_HOSTED_SRAM_BASE,
						      S31_HOSTED_SRAM_SIZE);
		shared_rmb();
		shared_invalidate(&ring->producer, sizeof(ring->producer));
		producer = ring->producer;
		s_ctrl->h0_seen_h1_producer = producer;
		s_ctrl->h0_seen_h1_sequence = s_h1_to_h0[
			ring->consumer & (S31_HOSTED_SLOT_COUNT - 1)].sequence;
		/* PMA read-back exported by core1_trampoline.S at +0x400. */
		s_ctrl->h0_apm_status =
			*(volatile uint32_t *)(S31_HOSTED_SRAM_BASE + 0x400);
		s_ctrl->h0_h1_doorbell =
			*(volatile uint32_t *)(S31_HOSTED_SRAM_BASE + 0x404);
		/*
		 * Temporary bring-up diagnostics.  Bit 0..5 are HP_MEM APM
		 * masters, bit 8..11 are CPU APM masters.  Preserve the first
		 * reported fault address at +0x40c.
		 */
		{
			volatile uint32_t *apm_mask =
				(volatile uint32_t *)(S31_HOSTED_SRAM_BASE + 0x408);
			volatile uint32_t *apm_addr =
				(volatile uint32_t *)(S31_HOSTED_SRAM_BASE + 0x40c);
			uint32_t mask = 0;

#define CAPTURE_APM(_bit, _status, _addr) do {		\
	if (REG_READ(_status) & 3) {			\
		mask |= BIT(_bit);				\
		if (!*apm_addr)				\
			*apm_addr = REG_READ(_addr);		\
	}							\
} while (0)
			CAPTURE_APM(0, HP_MEM_APM_M0_STATUS_REG,
				    HP_MEM_APM_M0_EXCEPTION_INFO1_REG);
			CAPTURE_APM(1, HP_MEM_APM_M1_STATUS_REG,
				    HP_MEM_APM_M1_EXCEPTION_INFO1_REG);
			CAPTURE_APM(2, HP_MEM_APM_M2_STATUS_REG,
				    HP_MEM_APM_M2_EXCEPTION_INFO1_REG);
			CAPTURE_APM(3, HP_MEM_APM_M3_STATUS_REG,
				    HP_MEM_APM_M3_EXCEPTION_INFO1_REG);
			CAPTURE_APM(4, HP_MEM_APM_M4_STATUS_REG,
				    HP_MEM_APM_M4_EXCEPTION_INFO1_REG);
			CAPTURE_APM(5, HP_MEM_APM_M5_STATUS_REG,
				    HP_MEM_APM_M5_EXCEPTION_INFO1_REG);
			CAPTURE_APM(8, CPU_APM_M0_STATUS_REG,
				    CPU_APM_M0_EXCEPTION_INFO1_REG);
			CAPTURE_APM(9, CPU_APM_M1_STATUS_REG,
				    CPU_APM_M1_EXCEPTION_INFO1_REG);
			CAPTURE_APM(10, CPU_APM_M2_STATUS_REG,
				    CPU_APM_M2_EXCEPTION_INFO1_REG);
			CAPTURE_APM(11, CPU_APM_M3_STATUS_REG,
				    CPU_APM_M3_EXCEPTION_INFO1_REG);
#undef CAPTURE_APM
			*apm_mask = mask;
		}
		/*
		 * Keep the bring-up counters observable from hart1.  They share
		 * the first cache line, so publish them together after sampling
		 * the inbound producer.
		 */
		shared_writeback(s_ctrl, 64);
		if (consumer == producer)
			break;

		index = consumer & (S31_HOSTED_SLOT_COUNT - 1);
		slot = &s_h1_to_h0[index];
		shared_invalidate(slot, sizeof(*slot));
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
	s_rx_task = xTaskGetCurrentTaskHandle();

	for (;;) {
		s_ctrl->h0_rx_polls++;
		shared_writeback(s_ctrl, 64);
		drain_h1_ring();
		/*
		 * Hart1 is released outside IDF's normal SMP startup and can
		 * temporarily disturb hart0's SysTick while its CLIC state is
		 * restored.  Keep the transport independently live during that
		 * window; doorbells still wake higher-priority radio work later.
		 */
		for (volatile unsigned int i = 0; i < 10000; i++)
			__asm__ volatile("nop");
		taskYIELD();
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
	esp_err_t err;

	if (s_ctrl->magic == S31_HOSTED_MAGIC &&
	    s_ctrl->abi_version == S31_HOSTED_ABI_VERSION)
		generation = s_ctrl->generation + 1;
	memset((void *)s_ctrl, 0, S31_HOSTED_SRAM_SIZE);
	s_ctrl->magic = S31_HOSTED_MAGIC;
	s_ctrl->abi_version = S31_HOSTED_ABI_VERSION;
	s_ctrl->generation = generation ? generation : 1;
	s_ctrl->state = S31_HOSTED_H0_READY;
	/* Only write back the control block (first 64 B); SRAM is non-cacheable. */
	shared_writeback(s_ctrl, 64);

	REG_WRITE(HP_SYSTEM_CPU_INT_FROM_CPU_2_REG, 0);
	REG_WRITE(HP_SYSTEM_CPU_INT_FROM_CPU_3_REG, 0);
	err = esp_intr_alloc(ETS_CPU_INTR_FROM_CPU_3_SOURCE,
			    ESP_INTR_FLAG_LEVEL1, h1_doorbell_isr, NULL,
			    &s_h1_irq);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "failed to allocate hart1 doorbell: %s",
			 esp_err_to_name(err));
		return err;
	}

	if (xTaskCreatePinnedToCore(hosted_rx_task, "hosted_rx", 3072, NULL, 1,
				    NULL, 0) != pdPASS) {
		esp_intr_free(s_h1_irq);
		s_h1_irq = NULL;
		return ESP_ERR_NO_MEM;
	}

	ESP_LOGI(TAG, "SRAM transport ready: 0x%08x..0x%08x generation=%" PRIu32,
		 S31_HOSTED_SRAM_BASE,
		 S31_HOSTED_SRAM_BASE + S31_HOSTED_SRAM_SIZE,
		 s_ctrl->generation);
	send_control(S31_HOSTED_CTRL_RADIO_READY, 0);
	return ESP_OK;
}
