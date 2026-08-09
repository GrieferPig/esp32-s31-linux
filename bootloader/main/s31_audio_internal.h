/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define S31_AUDIO_BLOCK_FRAMES 256U
#define S31_AUDIO_MAX_INPUT_FRAMES 512U

esp_err_t s31_audio_hw_start(void);
esp_err_t s31_audio_hw_tx_start(void);
esp_err_t s31_audio_hw_tx_stop(void);
esp_err_t s31_audio_hw_rx_start(void);
esp_err_t s31_audio_hw_rx_stop(void);
esp_err_t s31_audio_hw_read(uint16_t *samples, size_t frames);
esp_err_t s31_audio_hw_write(const uint16_t *samples, size_t frames);
const struct s31_audio_output *s31_audio_hw_output(void);
BaseType_t s31_audio_notify_from_isr(void);

esp_err_t s31_audio_asrc_init(void);
esp_err_t s31_audio_asrc_convert(unsigned int lane, unsigned int input_rate,
				 const uint16_t *input, size_t input_frames,
				 uint16_t *output, size_t output_frames,
				 unsigned int channels);
