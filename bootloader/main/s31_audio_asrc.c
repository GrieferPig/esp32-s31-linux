/* SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>
#include <stdint.h>

#include "asrc_adapter.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "hal/asrc_hal.h"

#include "s31_audio_internal.h"

#define ASRC_LANES 2
#define ASRC_BURST_BYTES 16
#define ASRC_DESCRIPTOR_BYTES 4080U

struct asrc_lane {
	SemaphoreHandle_t lock;
	QueueHandle_t events;
	asrc_hw_gdma_channel_handle_t tx;
	asrc_hw_gdma_channel_handle_t rx;
	asrc_hw_gdma_link_list_handle_t input_list;
	asrc_hw_gdma_link_list_handle_t output_list;
	uint32_t input_descriptors;
	uint32_t output_descriptors;
	int16_t input[S31_AUDIO_MAX_INPUT_FRAMES * 2];
	int16_t output[S31_AUDIO_BLOCK_FRAMES * 2];
};

static struct asrc_lane s_lane[ASRC_LANES];
static asrc_hal_context_t s_hal;

static bool supported_rate(unsigned int rate)
{
	static const unsigned int rates[] = { 8000, 16000, 24000, 32000, 44100, 48000 };

	for (unsigned int i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
		if (rates[i] == rate)
			return true;
	return false;
}

esp_err_t s31_audio_asrc_init(void)
{
	asrc_hal_init(&s_hal);
	asrc_hal_enable_asrc_module(&s_hal, true);
	for (unsigned int i = 0; i < ASRC_LANES; i++) {
		s_lane[i].lock = xSemaphoreCreateMutex();
		s_lane[i].events = xQueueCreate(2, sizeof(asrc_hw_gdma_evt_t));
		if (!s_lane[i].lock || !s_lane[i].events)
			return ESP_ERR_NO_MEM;
		if (asrc_hw_gdma_create_channel(i, s_lane[i].events,
						ASRC_BURST_BYTES, &s_lane[i].tx,
						&s_lane[i].rx) != ESP_OK)
			return ESP_FAIL;
	}
	return ESP_OK;
}

esp_err_t s31_audio_asrc_convert(unsigned int lane, unsigned int input_rate,
				 const uint16_t *input, size_t input_frames,
				 uint16_t *output, size_t output_frames,
				 unsigned int channels)
{
	struct asrc_lane *state;
	asrc_hal_config_t config = {
		.src_info = { input_rate, channels, 16 },
		.dest_info = { 48000, channels, 16 },
	};
	asrc_hw_gdma_evt_t event;
	uint32_t input_bytes = input_frames * channels * sizeof(*input);
	uint32_t output_bytes = output_frames * channels * sizeof(*output);
	uint32_t input_desc = (input_bytes + ASRC_DESCRIPTOR_BYTES - 1) /
		ASRC_DESCRIPTOR_BYTES;
	uint32_t output_desc = (output_bytes + ASRC_DESCRIPTOR_BYTES - 1) /
		ASRC_DESCRIPTOR_BYTES;
	esp_err_t result = ESP_FAIL;

	if (lane >= ASRC_LANES || !input || !output || !input_frames ||
	    !output_frames || !supported_rate(input_rate) ||
	    (channels != 1 && channels != 2) ||
	    input_frames > S31_AUDIO_MAX_INPUT_FRAMES ||
	    output_frames > S31_AUDIO_BLOCK_FRAMES)
		return ESP_ERR_INVALID_ARG;
	state = &s_lane[lane];
	xSemaphoreTake(state->lock, portMAX_DELAY);
	xQueueReset(state->events);
	for (size_t i = 0; i < input_frames * channels; i++)
		state->input[i] = (int16_t)(input[i] ^ 0x8000U);
	if (asrc_hw_gdma_create_link_list(input_bytes, &state->input_list,
					&state->input_descriptors) != ESP_OK ||
	    asrc_hw_gdma_create_link_list(output_bytes, &state->output_list,
					&state->output_descriptors) != ESP_OK ||
	    asrc_hw_gdma_mount_link_list(state->input_list, input_desc,
			(uint8_t *)state->input, input_bytes) != ESP_OK ||
	    asrc_hw_gdma_mount_link_list(state->output_list, output_desc,
			(uint8_t *)state->output, output_bytes) != ESP_OK)
		goto out;
	asrc_hal_init_stream(&s_hal, &config, lane);
	asrc_hal_set_in_bytes_num(&s_hal, lane, input_bytes);
	asrc_hal_set_out_bytes_num(&s_hal, lane, output_bytes);
	if (asrc_hw_gdma_start_channel(state->rx, state->output_list) != ESP_OK ||
	    asrc_hw_gdma_start_channel(state->tx, state->input_list) != ESP_OK)
		goto stop;
	asrc_hal_start(&s_hal, lane);
	if (xQueueReceive(state->events, &event, pdMS_TO_TICKS(20)) == pdTRUE &&
	    event.gdma_evt == ASRC_HW_GDMA_RECV_FRAME_DONE &&
	    asrc_hal_get_out_data_bytes(&s_hal, lane) >= output_bytes) {
		for (size_t i = 0; i < output_frames * channels; i++)
			output[i] = (uint16_t)state->output[i] ^ 0x8000U;
		result = ESP_OK;
	}
stop:
	asrc_hal_deinit_stream(&s_hal, lane);
out:
	xSemaphoreGive(state->lock);
	return result;
}
