/*
 * ESP-Hosted physical interface over the S31 inter-hart SRAM transport.
 * The co-processor library uses one-based interface identifiers while the
 * classic FG wire ABI is zero-based, so the conversion belongs here.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_hosted_header.h"
#include "esp_hosted_interface.h"
#include "esp_hosted_transport.h"
#include "esp_hosted_transport_init.h"
#include "esp_hosted_coprocessor_fw_ver.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "hosted_sram.h"
#include "interface.h"
#include "s31_hosted_sram.h"

#define SRAM_RX_QUEUE_DEPTH 8
#define SRAM_STARTUP_MAX    64

struct sram_rx_item {
	uint8_t *frame;
	uint16_t length;
};

static const char *TAG = "hosted_sram_if";
static interface_context_t s_context;
static interface_handle_t s_handle;
static QueueHandle_t s_rx_queue;

void esp_hosted_transport_wifi_state_changed(bool connected)
{
	uint8_t mac[6];

	if (connected && esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK)
		s31_hosted_sram_set_sta_mac(mac);
	s31_hosted_sram_set_wifi_state(connected ? 1 : 0);
}

static void free_rx_frame(void *buffer)
{
	free(buffer);
}

static int sram_frame_received(const uint8_t *frame, size_t length, void *arg)
{
	const struct s31_esp_payload_header *header = (const void *)frame;
	struct sram_rx_item item;

	(void)arg;
	if (!s_rx_queue || length < sizeof(*header) ||
	    header->if_type >= S31_HOSTED_MAX_IF)
		return 0;

	item.frame = malloc(length);
	if (!item.frame)
		return 1;
	memcpy(item.frame, frame, length);
	item.length = length;
	if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE)
		free(item.frame);
	return 1;
}

static interface_handle_t *sram_init(void)
{
	s_rx_queue = xQueueCreate(SRAM_RX_QUEUE_DEPTH, sizeof(struct sram_rx_item));
	if (!s_rx_queue)
		return NULL;

	memset(&s_handle, 0, sizeof(s_handle));
	s_handle.state = ACTIVE;
	s31_hosted_sram_set_frame_handler(sram_frame_received, NULL);
	return &s_handle;
}

static int sram_read(interface_handle_t *handle,
		     interface_buffer_handle_t *buf_handle)
{
	struct sram_rx_item item;
	struct esp_payload_header *header;

	if (!handle || handle->state != ACTIVE ||
	    xQueueReceive(s_rx_queue, &item, portMAX_DELAY) != pdTRUE)
		return 0;

	header = (void *)item.frame;
	/* Convert classic FG wire numbering to the MCU co-processor numbering. */
	buf_handle->if_type = header->if_type + 1;
	buf_handle->if_num = header->if_num;
	buf_handle->payload = item.frame;
	buf_handle->payload_len = item.length;
	buf_handle->flag = header->flags;
	buf_handle->seq_num = header->seq_num;
	buf_handle->priv_buffer_handle = item.frame;
	buf_handle->free_buf_handle = free_rx_frame;
	return item.length;
}

static int32_t sram_write(interface_handle_t *handle,
			  interface_buffer_handle_t *buf_handle)
{
	const uint8_t *payload;
	uint16_t payload_len;
	uint8_t packet_type = 0;
	uint8_t wire_if;

	if (!handle || handle->state != ACTIVE || !buf_handle ||
	    !buf_handle->payload || !buf_handle->payload_len ||
	    buf_handle->if_type <= ESP_INVALID_IF)
		return ESP_FAIL;

	wire_if = buf_handle->if_type - 1;
	payload = buf_handle->payload;
	payload_len = buf_handle->payload_len;
	/*
	 * VHCI callbacks use H4 framing.  ESP-Hosted stores the H4 packet type
	 * in the final byte of the Hosted header so the receiver can prepend it
	 * without copying the payload.
	 */
	if (wire_if == S31_HOSTED_HCI_IF) {
		packet_type = payload[0];
		payload++;
		payload_len--;
		if (!payload_len)
			return ESP_FAIL;
	}
	if (s31_hosted_sram_send_meta(wire_if, buf_handle->if_num,
				      payload, payload_len,
				      buf_handle->flag,
				      buf_handle->seq_num, packet_type))
		return ESP_FAIL;
	return buf_handle->payload_len;
}

static esp_err_t sram_reset(interface_handle_t *handle)
{
	if (!handle)
		return ESP_ERR_INVALID_ARG;
	handle->state = ACTIVE;
	return ESP_OK;
}

static void sram_deinit(interface_handle_t *handle)
{
	if (handle)
		handle->state = DEINIT;
	s31_hosted_sram_set_frame_handler(NULL, NULL);
}

static if_ops_t s_if_ops = {
	.init = sram_init,
	.write = sram_write,
	.read = sram_read,
	.reset = sram_reset,
	.deinit = sram_deinit,
};

interface_context_t *interface_insert_driver(int (*event_handler)(uint8_t val))
{
	memset(&s_context, 0, sizeof(s_context));
	s_context.type = SPI;
	s_context.if_ops = &s_if_ops;
	s_context.event_handler = event_handler;
	return &s_context;
}

int interface_remove_driver(void)
{
	sram_deinit(&s_handle);
	memset(&s_context, 0, sizeof(s_context));
	return 0;
}

void generate_startup_event(uint8_t cap, uint32_t ext_cap)
{
	uint8_t payload[SRAM_STARTUP_MAX] = { 0 };
	struct esp_priv_event *event = (void *)payload;
	uint8_t *pos = event->event_data;
	uint16_t len = 0;
	uint32_t fw_version = ESP_HOSTED_VERSION_VAL(PROJECT_VERSION_MAJOR_1,
						     PROJECT_VERSION_MINOR_1,
						     PROJECT_VERSION_PATCH_1);

#define ADD_TLV_U8(tag, value) do { \
	*pos++ = (tag); *pos++ = LENGTH_1_BYTE; *pos++ = (value); len += 3; \
} while (0)
#define ADD_TLV_U32(tag, value) do { \
	uint32_t v = (value); \
	*pos++ = (tag); *pos++ = LENGTH_4_BYTE; \
	*pos++ = v; *pos++ = v >> 8; *pos++ = v >> 16; *pos++ = v >> 24; \
	len += 6; \
} while (0)

	event->event_type = ESP_PRIV_EVENT_INIT;
	ADD_TLV_U8(ESP_PRIV_FIRMWARE_CHIP_ID, CONFIG_IDF_FIRMWARE_CHIP_ID);
	ADD_TLV_U8(ESP_PRIV_CAPABILITY, cap);
	ADD_TLV_U32(ESP_PRIV_CAP_EXT, ext_cap);
	ADD_TLV_U8(ESP_PRIV_TEST_RAW_TP, 0);
	ADD_TLV_U8(ESP_PRIV_RX_Q_SIZE, SRAM_RX_QUEUE_DEPTH);
	ADD_TLV_U8(ESP_PRIV_TX_Q_SIZE, S31_HOSTED_SLOT_COUNT);
	ADD_TLV_U32(ESP_PRIV_FIRMWARE_VERSION, fw_version);
	event->event_len = len;
	len += sizeof(event->event_type) + sizeof(event->event_len);

	if (s31_hosted_sram_send_meta(S31_HOSTED_PRIV_IF, 0, payload, len, 0,
				      0, ESP_PACKET_TYPE_EVENT))
		ESP_LOGE(TAG, "failed to publish startup event");
	else
		ESP_LOGI(TAG, "startup event published, capabilities=0x%02x "
			      "extended=0x%08lx", cap, (unsigned long)ext_cap);
}
