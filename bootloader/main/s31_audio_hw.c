/* SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "s31_audio.h"
#include "s31_audio_internal.h"
#include "s31_audio_sram.h"

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static esp_codec_dev_handle_t s_codec;
static bool s_tx_enabled;
static bool s_rx_enabled;
static int16_t s_i2s_tx[S31_AUDIO_BLOCK_FRAMES * S31_AUDIO_HW_CHANNELS];
static int16_t s_i2s_rx[S31_AUDIO_BLOCK_FRAMES * S31_AUDIO_HW_CHANNELS];

static void diagnose_i2c_bus(i2c_master_bus_handle_t bus)
{
	bool found = false;

	if (i2c_master_probe(bus, ES8311_CODEC_DEFAULT_ADDR >> 1, 20) == ESP_OK)
		return;
	ESP_LOGE("s31_audio", "ES8311 did not ACK at 7-bit I2C address 0x%02x",
		 ES8311_CODEC_DEFAULT_ADDR >> 1);
	for (uint8_t address = 0x08; address < 0x78; address++) {
		if (i2c_master_probe(bus, address, 20) == ESP_OK) {
			ESP_LOGW("s31_audio", "I2C device ACK at 0x%02x", address);
			found = true;
		}
	}
	if (!found)
		ESP_LOGE("s31_audio", "no devices ACK on the configured I2C bus");
}

static esp_err_t i2s_output_start(void *context, unsigned int rate)
{
	(void)context;
	return rate == S31_AUDIO_HW_RATE ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t i2s_output_write(void *context, const uint16_t *samples,
				  size_t frames)
{
	size_t written = 0;
	size_t bytes = frames * S31_AUDIO_HW_CHANNELS * sizeof(*samples);

	(void)context;
	if (frames > S31_AUDIO_BLOCK_FRAMES)
		return ESP_ERR_INVALID_SIZE;
	for (size_t i = 0; i < frames * S31_AUDIO_HW_CHANNELS; i++)
		s_i2s_tx[i] = (int16_t)(samples[i] ^ 0x8000U);
	if (i2s_channel_write(s_tx, s_i2s_tx, bytes, &written,
			      pdMS_TO_TICKS(20)) != ESP_OK)
		return ESP_FAIL;
	return written == bytes ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static const struct s31_audio_output s_i2s_output = {
	.name = "es8311",
	.start = i2s_output_start,
	.write = i2s_output_write,
};

esp_err_t s31_audio_hw_tx_start(void)
{
	esp_err_t error;
	size_t loaded = 0;

	if (s_tx_enabled)
		return ESP_OK;
	memset(s_i2s_tx, 0, sizeof(s_i2s_tx));
	error = i2s_channel_preload_data(s_tx, s_i2s_tx, sizeof(s_i2s_tx),
					 &loaded);
	if (error != ESP_OK || !loaded)
		return error == ESP_OK ? ESP_FAIL : error;
	error = i2s_channel_enable(s_tx);
	if (error == ESP_OK)
		s_tx_enabled = true;
	return error;
}

esp_err_t s31_audio_hw_tx_stop(void)
{
	esp_err_t error;

	if (!s_tx_enabled)
		return ESP_OK;
	error = i2s_channel_disable(s_tx);
	if (error == ESP_OK)
		s_tx_enabled = false;
	return error;
}

esp_err_t s31_audio_hw_rx_start(void)
{
	esp_err_t error;

	if (s_rx_enabled)
		return ESP_OK;
	error = i2s_channel_enable(s_rx);
	if (error == ESP_OK)
		s_rx_enabled = true;
	return error;
}

esp_err_t s31_audio_hw_rx_stop(void)
{
	esp_err_t error;

	if (!s_rx_enabled)
		return ESP_OK;
	error = i2s_channel_disable(s_rx);
	if (error == ESP_OK)
		s_rx_enabled = false;
	return error;
}

esp_err_t s31_audio_hw_start(void)
{
	i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
							 I2S_ROLE_MASTER);
	i2s_std_config_t standard = {
		.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(S31_AUDIO_HW_RATE),
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
			I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
		.gpio_cfg = {
			.mclk = CONFIG_S31_AUDIO_I2S_MCLK,
			.bclk = CONFIG_S31_AUDIO_I2S_BCLK,
			.ws = CONFIG_S31_AUDIO_I2S_WS,
			.dout = CONFIG_S31_AUDIO_I2S_DOUT,
			.din = CONFIG_S31_AUDIO_I2S_DIN,
		},
	};
	i2c_master_bus_handle_t i2c_bus;
	i2c_master_bus_config_t i2c_master = {
		.i2c_port = I2C_NUM_0,
		.sda_io_num = CONFIG_S31_AUDIO_I2C_SDA,
		.scl_io_num = CONFIG_S31_AUDIO_I2C_SCL,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	audio_codec_i2c_cfg_t codec_i2c = {
		.port = I2C_NUM_0,
		.addr = ES8311_CODEC_DEFAULT_ADDR,
		.clock_speed_hz = 400000,
	};
	audio_codec_i2s_cfg_t codec_i2s = {
		.port = I2S_NUM_0,
	};
	es8311_codec_cfg_t es8311 = {
		.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
		.master_mode = false,
		.use_mclk = CONFIG_S31_AUDIO_I2S_MCLK >= 0,
		.pa_pin = CONFIG_S31_AUDIO_PA_GPIO,
		.pa_reverted = false,
		.hw_gain = {
			.pa_voltage = 5.0,
			.codec_dac_voltage = 3.3,
		},
		.mclk_div = 256,
	};
	esp_codec_dev_cfg_t device = {
		.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
	};
	esp_codec_dev_sample_info_t sample = {
		.bits_per_sample = 16,
		.channel = S31_AUDIO_HW_CHANNELS,
		.channel_mask = 3,
		.sample_rate = S31_AUDIO_HW_RATE,
		.mclk_multiple = 256,
	};

	channel.dma_desc_num = 4;
	channel.dma_frame_num = S31_AUDIO_BLOCK_FRAMES;
	channel.auto_clear = true;
	ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, &s_tx, &s_rx),
			    "s31_audio", "create I2S channels");
	ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &standard),
			    "s31_audio", "configure I2S TX");
	ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &standard),
			    "s31_audio", "configure I2S RX");
	ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_master, &i2c_bus),
			    "s31_audio", "create codec I2C bus");
	diagnose_i2c_bus(i2c_bus);
	codec_i2c.bus_handle = i2c_bus;
	codec_i2s.tx_handle = s_tx;
	codec_i2s.rx_handle = s_rx;
	es8311.ctrl_if = audio_codec_new_i2c_ctrl(&codec_i2c);
	es8311.gpio_if = audio_codec_new_gpio();
	device.data_if = audio_codec_new_i2s_data(&codec_i2s);
	if (!es8311.ctrl_if || !es8311.gpio_if || !device.data_if)
		return ESP_ERR_NO_MEM;
	device.codec_if = es8311_codec_new(&es8311);
	if (!device.codec_if)
		return ESP_FAIL;
	s_codec = esp_codec_dev_new(&device);
	if (!s_codec || esp_codec_dev_open(s_codec, &sample) != ESP_CODEC_DEV_OK)
		return ESP_FAIL;
	/* esp_codec_dev_open() enables both channels; transfers remain request-driven. */
	s_tx_enabled = true;
	s_rx_enabled = true;
	if (esp_codec_dev_set_out_vol(s_codec, 70) != ESP_CODEC_DEV_OK ||
	    esp_codec_dev_set_in_gain(s_codec, 18.0) != ESP_CODEC_DEV_OK)
		return ESP_FAIL;
	return ESP_OK;
}

esp_err_t s31_audio_hw_read(uint16_t *samples, size_t frames)
{
	size_t read = 0;
	size_t bytes = frames * S31_AUDIO_HW_CHANNELS * sizeof(*samples);

	if (frames > S31_AUDIO_BLOCK_FRAMES)
		return ESP_ERR_INVALID_SIZE;
	if (i2s_channel_read(s_rx, s_i2s_rx, bytes, &read,
			     pdMS_TO_TICKS(20)) != ESP_OK)
		return ESP_FAIL;
	if (read != bytes)
		return ESP_ERR_INVALID_SIZE;
	for (size_t i = 0; i < frames * S31_AUDIO_HW_CHANNELS; i++)
		samples[i] = (uint16_t)s_i2s_rx[i] ^ 0x8000U;
	return ESP_OK;
}

esp_err_t s31_audio_hw_write(const uint16_t *samples, size_t frames)
{
	return i2s_output_write(NULL, samples, frames);
}

const struct s31_audio_output *s31_audio_hw_output(void)
{
	return &s_i2s_output;
}
