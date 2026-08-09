/* SPDX-License-Identifier: Apache-2.0 */

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_caps.h"
#include "esp_rom_serial_output.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/hp_system_reg.h"
#include "soc/soc.h"

#include "s31_audio.h"
#include "s31_audio_internal.h"
#include "s31_audio_sram.h"

#define S31_AUDIO_MAX_PLUGINS 8
#define S31_AUDIO_MAX_OUTPUTS 4
#define S31_AUDIO_AUTOMATION_DEPTH 8
#define S31_AUDIO_UNITY_Q16 65536
#define S31_AUDIO_ACTIVE_FLOOR 256
#ifdef CONFIG_S31_AUDIO_MIC_RIGHT_SLOT
#define S31_AUDIO_MIC_SLOT 1
#else
#define S31_AUDIO_MIC_SLOT 0
#endif

struct automation_state {
	struct s31_audio_automation_segment queue[S31_AUDIO_AUTOMATION_DEPTH];
	uint8_t read;
	uint8_t write;
	uint32_t position;
	int32_t gain_q16;
};

struct source_state {
	uint8_t priority;
	int32_t duck_target_q16;
	int32_t duck_gain_q16;
	uint32_t attack_frames;
	uint32_t release_frames;
	struct automation_state automation;
};

static const char *TAG = "s31_audio";
static volatile struct s31_audio_control *const s_control =
	(void *)S31_AUDIO_CONTROL_BASE;
static struct source_state s_source[S31_AUDIO_PLAYBACK_COUNT];
static const struct s31_audio_plugin *s_plugins[S31_AUDIO_MAX_PLUGINS];
static const struct s31_audio_output *s_outputs[S31_AUDIO_MAX_OUTPUTS];
static const struct s31_audio_output *s_output;
static SemaphoreHandle_t s_api_lock;
static TaskHandle_t s_audio_task;
static uint16_t s_input[S31_AUDIO_PLAYBACK_COUNT]
	[S31_AUDIO_MAX_INPUT_FRAMES * S31_AUDIO_HW_CHANNELS];
static uint16_t s_converted[S31_AUDIO_PLAYBACK_COUNT]
	[S31_AUDIO_BLOCK_FRAMES * S31_AUDIO_HW_CHANNELS];
static uint16_t s_capture[S31_AUDIO_BLOCK_FRAMES * S31_AUDIO_HW_CHANNELS];
static uint16_t s_capture_ring[S31_AUDIO_BLOCK_FRAMES * S31_AUDIO_HW_CHANNELS];
static uint16_t s_pcm_out[S31_AUDIO_BLOCK_FRAMES * S31_AUDIO_HW_CHANNELS];
static int32_t s_mix[S31_AUDIO_BLOCK_FRAMES * S31_AUDIO_HW_CHANNELS];

static inline void shared_fence(void)
{
	__asm__ volatile("fence rw, rw" ::: "memory");
}

static void notify_linux(void)
{
	shared_fence();
	REG_WRITE(HP_SYSTEM_CPU_INT_FROM_CPU_1_REG,
		  HP_SYSTEM_CPU_INT_FROM_CPU_1);
}

static void notify_linux_period(volatile struct s31_audio_ring_control *ring,
				s31_audio_u32 previous, s31_audio_u32 current)
{
	s31_audio_u32 period = ring->period_bytes;

	if (ring->owner != S31_AUDIO_OWNER_LINUX)
		return;
	if (!period || previous / period != current / period)
		notify_linux();
}

static volatile uint8_t *ring_data(unsigned int stream)
{
	return (void *)(S31_AUDIO_SRAM_BASE + s31_audio_ring_offset(stream));
}

static void ring_copy_from(volatile uint8_t *ring, uint32_t position,
			   void *destination, size_t bytes)
{
	size_t first = S31_AUDIO_RING_BYTES - position;

	if (first > bytes)
		first = bytes;
	memcpy(destination, (const void *)(ring + position), first);
	if (first != bytes)
		memcpy((uint8_t *)destination + first, (const void *)ring,
		       bytes - first);
}

static void ring_copy_to(volatile uint8_t *ring, uint32_t position,
			 const void *source, size_t bytes)
{
	size_t first = S31_AUDIO_RING_BYTES - position;

	if (first > bytes)
		first = bytes;
	memcpy((void *)(ring + position), source, first);
	if (first != bytes)
		memcpy((void *)ring, (const uint8_t *)source + first, bytes - first);
}

static size_t playback_read(unsigned int stream, uint16_t *samples,
			    size_t frames, unsigned int *rate,
			    unsigned int *channels)
{
	volatile struct s31_audio_ring_control *ring = &s_control->stream[stream];
	uint32_t producer;
	uint32_t consumer;
	uint32_t generation;
	uint32_t available;
	size_t frame_bytes;
	size_t bytes;
	size_t copied_frames;

	if (ring->state != S31_AUDIO_STATE_RUNNING ||
	    ring->format != S31_AUDIO_FORMAT_U16_LE ||
	    (ring->channels != 1 && ring->channels != 2))
		return 0;
	shared_fence();
	generation = ring->generation;
	producer = ring->producer;
	consumer = ring->consumer;
	available = producer - consumer;
	if (available > S31_AUDIO_RING_BYTES) {
		ring->consumer = producer;
		ring->xruns++;
		return 0;
	}
	frame_bytes = ring->channels * S31_AUDIO_SAMPLE_BYTES;
	bytes = frames * frame_bytes;
	if (bytes > available)
		bytes = available - available % frame_bytes;
	if (!bytes)
		return 0;
	ring_copy_from(ring_data(stream), consumer % S31_AUDIO_RING_BYTES,
		       samples, bytes);
	copied_frames = bytes / frame_bytes;
	for (size_t i = copied_frames * ring->channels;
	     i < frames * ring->channels; i++)
		samples[i] = 32768;
	shared_fence();
	if (ring->generation != generation ||
	    ring->state != S31_AUDIO_STATE_RUNNING)
		return 0;
	ring->consumer = consumer + bytes;
	ring->transferred_bytes += bytes;
	notify_linux_period(ring, consumer, consumer + bytes);
	*rate = ring->rate;
	*channels = ring->channels;
	return frames;
}

static void capture_write(const uint16_t *samples, size_t frames)
{
	volatile struct s31_audio_ring_control *ring =
		&s_control->stream[S31_AUDIO_CAPTURE_STREAM];
	uint32_t producer;
	uint32_t consumer;
	uint32_t generation;
	uint32_t used;
	unsigned int channels = ring->channels;
	size_t bytes;

	if (ring->owner != S31_AUDIO_OWNER_LINUX ||
	    ring->state != S31_AUDIO_STATE_RUNNING ||
	    ring->format != S31_AUDIO_FORMAT_U16_LE ||
	    ring->rate != S31_AUDIO_HW_RATE ||
	    (channels != 1 && channels != 2))
		return;
	/* ES8311 has one ADC; stereo capture intentionally duplicates the mic. */
	for (size_t frame = 0; frame < frames; frame++) {
		uint16_t mic = samples[frame * S31_AUDIO_HW_CHANNELS +
			S31_AUDIO_MIC_SLOT];

		s_capture_ring[frame * channels] = mic;
		if (channels == 2)
			s_capture_ring[frame * channels + 1] = mic;
	}
	bytes = frames * channels * S31_AUDIO_SAMPLE_BYTES;
	shared_fence();
	generation = ring->generation;
	producer = ring->producer;
	consumer = ring->consumer;
	used = producer - consumer;
	if (used > S31_AUDIO_RING_BYTES || bytes > S31_AUDIO_RING_BYTES - used) {
		ring->xruns++;
		return;
	}
	ring_copy_to(ring_data(S31_AUDIO_CAPTURE_STREAM),
		     producer % S31_AUDIO_RING_BYTES, s_capture_ring, bytes);
	shared_fence();
	if (ring->generation != generation ||
	    ring->state != S31_AUDIO_STATE_RUNNING)
		return;
	ring->producer = producer + bytes;
	ring->transferred_bytes += bytes;
	notify_linux_period(ring, producer, producer + bytes);
}

static int32_t automation_gain(struct automation_state *automation)
{
	struct s31_audio_automation_segment *segment;
	int64_t delta;

	if (automation->read == automation->write)
		return automation->gain_q16;
	segment = &automation->queue[automation->read];
	if (!segment->duration_frames) {
		automation->gain_q16 = segment->end_gain_q16;
		automation->read = (automation->read + 1) % S31_AUDIO_AUTOMATION_DEPTH;
		automation->position = 0;
		return automation->gain_q16;
	}
	delta = (int64_t)segment->end_gain_q16 - segment->start_gain_q16;
	automation->gain_q16 = segment->start_gain_q16 +
		(int32_t)(delta * automation->position / segment->duration_frames);
	if (++automation->position >= segment->duration_frames) {
		automation->gain_q16 = segment->end_gain_q16;
		automation->read = (automation->read + 1) % S31_AUDIO_AUTOMATION_DEPTH;
		automation->position = 0;
	}
	return automation->gain_q16;
}

static int32_t approach_gain(int32_t current, int32_t target,
			     int32_t full_range, uint32_t duration_frames)
{
	int32_t distance = target - current;
	int32_t step;

	if (!duration_frames || !distance)
		return target;
	step = full_range / duration_frames;
	if (!step)
		step = 1;
	if (distance < 0)
		step = -step;
	if ((distance > 0 && step >= distance) ||
	    (distance < 0 && step <= distance))
		return target;
	return current + step;
}

static bool linux_playback_requested(void)
{
	volatile struct s31_audio_ring_control *ring =
		&s_control->stream[S31_AUDIO_LINUX_STREAM];

	shared_fence();
	return ring->owner == S31_AUDIO_OWNER_LINUX &&
		ring->state == S31_AUDIO_STATE_RUNNING;
}

static bool freertos_playback_pending(void)
{
	volatile struct s31_audio_ring_control *ring =
		&s_control->stream[S31_AUDIO_FREERTOS_STREAM];

	shared_fence();
	return ring->owner == S31_AUDIO_OWNER_FREERTOS &&
		ring->state == S31_AUDIO_STATE_RUNNING &&
		ring->producer != ring->consumer;
}

static bool capture_requested(void)
{
	volatile struct s31_audio_ring_control *ring =
		&s_control->stream[S31_AUDIO_CAPTURE_STREAM];

	shared_fence();
	return ring->owner == S31_AUDIO_OWNER_LINUX &&
		ring->state == S31_AUDIO_STATE_RUNNING;
}

static void update_i2s_state(bool playback, bool capture)
{
	esp_err_t error;
	bool tx_clock = playback || capture;

	/* The duplex master derives BCLK/WS from TX even for capture-only use. */
	if (tx_clock) {
		error = s31_audio_hw_tx_start();
		if (error != ESP_OK)
			ESP_LOGE(TAG, "failed to start I2S TX: %s",
				 esp_err_to_name(error));
	}
	if (capture) {
		error = s31_audio_hw_rx_start();
		if (error != ESP_OK)
			ESP_LOGE(TAG, "failed to start I2S RX: %s",
				 esp_err_to_name(error));
	}
}

static void audio_task(void *argument)
{
	(void)argument;

	for (;;) {
		bool playback = linux_playback_requested() ||
			freertos_playback_pending();
		bool capture = capture_requested();
		unsigned int highest_priority = 0;
		bool active[S31_AUDIO_PLAYBACK_COUNT] = { false };
		uint16_t *source_pcm[S31_AUDIO_PLAYBACK_COUNT] = { 0 };
		size_t source_frames[S31_AUDIO_PLAYBACK_COUNT] = { 0 };
		unsigned int source_channels[S31_AUDIO_PLAYBACK_COUNT] = { 0 };
		unsigned int asrc_lane = 0;

		update_i2s_state(playback, capture);
		if (!playback && !capture) {
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
			continue;
		}
		if (capture && !playback) {
			for (size_t i = 0; i < S31_AUDIO_BLOCK_FRAMES * 2; i++)
				s_pcm_out[i] = 32768;
			if (s31_audio_hw_write(s_pcm_out,
					       S31_AUDIO_BLOCK_FRAMES) != ESP_OK)
				continue;
		}
		/* RX completion is the full-duplex hardware clock for every block. */
		if (s31_audio_hw_read(s_capture, S31_AUDIO_BLOCK_FRAMES) != ESP_OK)
			continue;
		if (capture)
			capture_write(s_capture, S31_AUDIO_BLOCK_FRAMES);
		if (!playback)
			continue;
		memset(s_mix, 0, sizeof(s_mix));

		for (unsigned int stream = 0; stream < S31_AUDIO_PLAYBACK_COUNT;
		     stream++) {
			unsigned int rate = s_control->stream[stream].rate;
			unsigned int channels = s_control->stream[stream].channels;
			size_t wanted = S31_AUDIO_BLOCK_FRAMES;
			size_t got;
			int peak = 0;

			if (rate && rate != S31_AUDIO_HW_RATE) {
				wanted = ((uint64_t)S31_AUDIO_BLOCK_FRAMES * rate +
					  S31_AUDIO_HW_RATE - 1) / S31_AUDIO_HW_RATE;
				if (wanted > S31_AUDIO_MAX_INPUT_FRAMES)
					wanted = S31_AUDIO_MAX_INPUT_FRAMES;
			}
			got = playback_read(stream, s_input[stream], wanted, &rate,
					    &channels);
			if (!got)
				continue;
			if (rate != S31_AUDIO_HW_RATE) {
				if (asrc_lane >= 2 || s31_audio_asrc_convert(asrc_lane++, rate,
						s_input[stream], got, s_converted[stream],
						S31_AUDIO_BLOCK_FRAMES, channels) != ESP_OK) {
					s_control->stream[stream].xruns++;
					continue;
				}
				source_pcm[stream] = s_converted[stream];
				source_frames[stream] = S31_AUDIO_BLOCK_FRAMES;
			} else {
				source_pcm[stream] = s_input[stream];
				source_frames[stream] = got;
			}
			source_channels[stream] = channels;
			/* Plugins are a source-0 insertion point, never a post-mix chain. */
			if (stream == 0 &&
			    xSemaphoreTake(s_api_lock, portMAX_DELAY) == pdTRUE) {
				struct s31_audio_block block = {
					.samples = source_pcm[stream],
					.frames = source_frames[stream],
					.channels = channels,
					.rate = S31_AUDIO_HW_RATE,
				};

				for (unsigned int i = 0; i < S31_AUDIO_MAX_PLUGINS; i++)
					if (s_plugins[i] && s_plugins[i]->process)
						s_plugins[i]->process(s_plugins[i]->context, &block);
				xSemaphoreGive(s_api_lock);
			}
			for (size_t i = 0; i < source_frames[stream] * channels; i++) {
				int value = (int)source_pcm[stream][i] - 32768;
				if (value < 0)
					value = -value;
				if (value > peak)
					peak = value;
			}
			active[stream] = peak >= S31_AUDIO_ACTIVE_FLOOR;
			if (active[stream] && s_source[stream].priority > highest_priority)
				highest_priority = s_source[stream].priority;
		}

		for (unsigned int stream = 0; stream < S31_AUDIO_PLAYBACK_COUNT;
		     stream++) {
			struct source_state *state = &s_source[stream];
			int32_t target = highest_priority > state->priority ?
				state->duck_target_q16 : S31_AUDIO_UNITY_Q16;
			uint32_t duration = target < state->duck_gain_q16 ?
				state->attack_frames : state->release_frames;

			if (!source_pcm[stream] || !active[stream])
				continue;
			for (size_t frame = 0; frame < source_frames[stream]; frame++) {
				int32_t gain = automation_gain(&state->automation);
				int32_t left;
				int32_t right;

				state->duck_gain_q16 = approach_gain(state->duck_gain_q16,
								 target,
								 S31_AUDIO_UNITY_Q16 -
								 state->duck_target_q16,
								 duration);
				gain = (int32_t)((int64_t)gain * state->duck_gain_q16 >> 16);
				left = (int32_t)source_pcm[stream]
					[frame * source_channels[stream]] - 32768;
				right = source_channels[stream] == 2 ?
					(int32_t)source_pcm[stream][frame * 2 + 1] - 32768 : left;
				s_mix[frame * 2] += (int32_t)((int64_t)left * gain >> 16);
				s_mix[frame * 2 + 1] +=
					(int32_t)((int64_t)right * gain >> 16);
			}
		}

		for (size_t i = 0; i < S31_AUDIO_BLOCK_FRAMES * 2; i++) {
			int32_t sample = s_mix[i];
			if (sample > INT16_MAX)
				sample = INT16_MAX;
			else if (sample < INT16_MIN)
				sample = INT16_MIN;
			s_pcm_out[i] = (uint16_t)(sample + 32768);
		}
		if (s_output)
			s_output->write(s_output->context, s_pcm_out,
					S31_AUDIO_BLOCK_FRAMES);
	}
}

BaseType_t s31_audio_notify_from_isr(void)
{
	BaseType_t wake = pdFALSE;

	if (s_audio_task)
		vTaskNotifyGiveFromISR(s_audio_task, &wake);
	return wake;
}

esp_err_t s31_audio_start(void)
{
	uint32_t generation = s_control->generation + 1;
	esp_err_t error;

	memset((void *)s_control, 0, sizeof(*s_control));
	memset((void *)S31_AUDIO_SRAM_BASE, 0, S31_AUDIO_RING_AREA_SIZE);
	REG_WRITE(HP_SYSTEM_CPU_INT_FROM_CPU_1_REG, 0);
	s_api_lock = xSemaphoreCreateMutex();
	if (!s_api_lock)
		return ESP_ERR_NO_MEM;
	for (unsigned int i = 0; i < S31_AUDIO_PLAYBACK_COUNT; i++) {
		s_source[i].priority = i;
		s_source[i].duck_target_q16 = S31_AUDIO_UNITY_Q16 / 4;
		s_source[i].duck_gain_q16 = S31_AUDIO_UNITY_Q16;
		s_source[i].attack_frames = S31_AUDIO_HW_RATE / 50;
		s_source[i].release_frames = S31_AUDIO_HW_RATE / 4;
		s_source[i].automation.gain_q16 = S31_AUDIO_UNITY_Q16;
	}
	error = s31_audio_asrc_init();
	if (error != ESP_OK) {
		ESP_LOGE(TAG, "ASRC initialization failed: %s", esp_err_to_name(error));
		return error;
	}
	error = s31_audio_hw_start();
	if (error != ESP_OK) {
		ESP_LOGE(TAG, "ES8311/I2S initialization failed: %s",
			 esp_err_to_name(error));
		return error;
	}
	s_output = s31_audio_hw_output();
	s_outputs[0] = s_output;
	if (s_output->start && s_output->start(s_output->context,
					      S31_AUDIO_HW_RATE) != ESP_OK)
		return ESP_FAIL;

	s_control->abi_version = S31_AUDIO_ABI_VERSION;
	s_control->generation = generation;
	s_control->hardware_rate = S31_AUDIO_HW_RATE;
	s_control->hardware_channels = S31_AUDIO_HW_CHANNELS;
	s_control->ring_bytes = S31_AUDIO_RING_BYTES;
	s_control->feature_bits = S31_AUDIO_FEAT_MIXER | S31_AUDIO_FEAT_ASRC |
		S31_AUDIO_FEAT_PLUGINS | S31_AUDIO_FEAT_DUCKING |
		S31_AUDIO_FEAT_AUTOMATION | S31_AUDIO_FEAT_OUTPUT_SWITCH |
		S31_AUDIO_FEAT_I2S_DMA;
	s_control->stream[S31_AUDIO_CAPTURE_STREAM].rate = S31_AUDIO_HW_RATE;
	s_control->stream[S31_AUDIO_CAPTURE_STREAM].channels = 1;
	s_control->stream[S31_AUDIO_CAPTURE_STREAM].format = S31_AUDIO_FORMAT_U16_LE;
	shared_fence();
	s_control->magic = S31_AUDIO_MAGIC;
	s_control->ready = 1;
	shared_fence();

	if (xTaskCreate(audio_task, "s31_audio", 6144, NULL,
			configMAX_PRIORITIES - 2, &s_audio_task) != pdPASS)
		return ESP_ERR_NO_MEM;
	ESP_LOGI(TAG, "48-kHz ES8311 audio core started");
	return ESP_OK;
}

esp_err_t s31_audio_plugin_register(const struct s31_audio_plugin *plugin)
{
	if (!plugin || !plugin->process)
		return ESP_ERR_INVALID_ARG;
	xSemaphoreTake(s_api_lock, portMAX_DELAY);
	for (unsigned int i = 0; i < S31_AUDIO_MAX_PLUGINS; i++) {
		if (!s_plugins[i]) {
			s_plugins[i] = plugin;
			xSemaphoreGive(s_api_lock);
			return ESP_OK;
		}
	}
	xSemaphoreGive(s_api_lock);
	return ESP_ERR_NO_MEM;
}

esp_err_t s31_audio_plugin_unregister(const struct s31_audio_plugin *plugin)
{
	xSemaphoreTake(s_api_lock, portMAX_DELAY);
	for (unsigned int i = 0; i < S31_AUDIO_MAX_PLUGINS; i++) {
		if (s_plugins[i] == plugin) {
			s_plugins[i] = NULL;
			xSemaphoreGive(s_api_lock);
			return ESP_OK;
		}
	}
	xSemaphoreGive(s_api_lock);
	return ESP_ERR_NOT_FOUND;
}

esp_err_t s31_audio_output_register(const struct s31_audio_output *output)
{
	if (!output || !output->name || !output->write)
		return ESP_ERR_INVALID_ARG;
	xSemaphoreTake(s_api_lock, portMAX_DELAY);
	for (unsigned int i = 0; i < S31_AUDIO_MAX_OUTPUTS; i++) {
		if (!s_outputs[i]) {
			s_outputs[i] = output;
			xSemaphoreGive(s_api_lock);
			return ESP_OK;
		}
	}
	xSemaphoreGive(s_api_lock);
	return ESP_ERR_NO_MEM;
}

esp_err_t s31_audio_output_select(const char *name)
{
	const struct s31_audio_output *next = NULL;

	if (!name)
		return ESP_ERR_INVALID_ARG;
	xSemaphoreTake(s_api_lock, portMAX_DELAY);
	for (unsigned int i = 0; i < S31_AUDIO_MAX_OUTPUTS; i++)
		if (s_outputs[i] && !strcmp(s_outputs[i]->name, name))
			next = s_outputs[i];
	if (!next) {
		xSemaphoreGive(s_api_lock);
		return ESP_ERR_NOT_FOUND;
	}
	if (next != s_output) {
		if (next->start && next->start(next->context, S31_AUDIO_HW_RATE) != ESP_OK) {
			xSemaphoreGive(s_api_lock);
			return ESP_FAIL;
		}
		if (s_output && s_output->stop)
			s_output->stop(s_output->context);
		s_output = next;
	}
	xSemaphoreGive(s_api_lock);
	return ESP_OK;
}

esp_err_t s31_audio_freertos_write(const uint16_t *samples, size_t frames,
				   unsigned int rate, unsigned int channels)
{
	volatile struct s31_audio_ring_control *ring;
	uint32_t producer;
	uint32_t consumer;
	size_t bytes;

	if (!samples || !frames || !rate || (channels != 1 && channels != 2))
		return ESP_ERR_INVALID_ARG;
	ring = &s_control->stream[S31_AUDIO_FREERTOS_STREAM];
	if (ring->owner != S31_AUDIO_OWNER_NONE &&
	    ring->owner != S31_AUDIO_OWNER_FREERTOS)
		return ESP_ERR_INVALID_STATE;
	bytes = frames * channels * S31_AUDIO_SAMPLE_BYTES;
	if (bytes > S31_AUDIO_RING_BYTES)
		return ESP_ERR_INVALID_SIZE;
	producer = ring->producer;
	consumer = ring->consumer;
	if (producer - consumer > S31_AUDIO_RING_BYTES - bytes)
		return ESP_ERR_NO_MEM;
	ring->owner = S31_AUDIO_OWNER_FREERTOS;
	ring->rate = rate;
	ring->channels = channels;
	ring->format = S31_AUDIO_FORMAT_U16_LE;
	ring->state = S31_AUDIO_STATE_RUNNING;
	ring_copy_to(ring_data(S31_AUDIO_FREERTOS_STREAM),
		     producer % S31_AUDIO_RING_BYTES,
		     samples, bytes);
	shared_fence();
	ring->producer = producer + bytes;
	if (s_audio_task)
		xTaskNotifyGive(s_audio_task);
	return ESP_OK;
}

esp_err_t s31_audio_stream_set_priority(unsigned int stream, uint8_t priority)
{
	if (stream >= S31_AUDIO_PLAYBACK_COUNT)
		return ESP_ERR_INVALID_ARG;
	s_source[stream].priority = priority;
	return ESP_OK;
}

esp_err_t s31_audio_stream_set_ducking(unsigned int stream, int32_t gain_q16,
				       uint32_t attack_frames,
				       uint32_t release_frames)
{
	if (stream >= S31_AUDIO_PLAYBACK_COUNT || gain_q16 < 0 ||
	    gain_q16 > S31_AUDIO_UNITY_Q16)
		return ESP_ERR_INVALID_ARG;
	s_source[stream].duck_target_q16 = gain_q16;
	s_source[stream].attack_frames = attack_frames;
	s_source[stream].release_frames = release_frames;
	return ESP_OK;
}

esp_err_t s31_audio_stream_automate(unsigned int stream,
				    const struct s31_audio_automation_segment *segment)
{
	struct automation_state *automation;
	uint8_t next;

	if (stream >= S31_AUDIO_PLAYBACK_COUNT || !segment ||
	    segment->start_gain_q16 < 0 || segment->end_gain_q16 < 0)
		return ESP_ERR_INVALID_ARG;
	automation = &s_source[stream].automation;
	next = (automation->write + 1) % S31_AUDIO_AUTOMATION_DEPTH;
	if (next == automation->read)
		return ESP_ERR_NO_MEM;
	automation->queue[automation->write] = *segment;
	automation->write = next;
	return ESP_OK;
}
