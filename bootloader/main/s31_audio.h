/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define S31_AUDIO_MIX_INPUTS 2

struct s31_audio_block {
	uint16_t *samples;
	size_t frames;
	unsigned int channels;
	unsigned int rate;
};

struct s31_audio_plugin {
	const char *name;
	esp_err_t (*process)(void *context, struct s31_audio_block *block);
	void *context;
};

struct s31_audio_output {
	const char *name;
	esp_err_t (*start)(void *context, unsigned int rate);
	/* Output backends always receive interleaved stereo U16 PCM. */
	esp_err_t (*write)(void *context, const uint16_t *samples, size_t frames);
	void (*stop)(void *context);
	void *context;
};

struct s31_audio_automation_segment {
	int32_t start_gain_q16;
	int32_t end_gain_q16;
	uint32_t duration_frames;
};

esp_err_t s31_audio_start(void);
esp_err_t s31_audio_plugin_register(const struct s31_audio_plugin *plugin);
esp_err_t s31_audio_plugin_unregister(const struct s31_audio_plugin *plugin);
esp_err_t s31_audio_output_register(const struct s31_audio_output *output);
esp_err_t s31_audio_output_select(const char *name);
esp_err_t s31_audio_freertos_write(const uint16_t *samples, size_t frames,
				   unsigned int rate, unsigned int channels);
esp_err_t s31_audio_stream_set_priority(unsigned int stream, uint8_t priority);
esp_err_t s31_audio_stream_set_ducking(unsigned int stream, int32_t gain_q16,
				       uint32_t attack_frames,
				       uint32_t release_frames);
esp_err_t s31_audio_stream_automate(unsigned int stream,
				    const struct s31_audio_automation_segment *segment);
