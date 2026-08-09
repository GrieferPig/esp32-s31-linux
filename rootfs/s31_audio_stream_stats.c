/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SAMPLE_RATE 48000U

struct audio_stats {
	uint64_t square_sum;
	int32_t sum;
	uint32_t samples;
	uint16_t minimum;
	uint16_t maximum;
};

static uint64_t integer_sqrt(uint64_t value)
{
	uint64_t result = 0;
	uint64_t bit = UINT64_C(1) << 62;

	while (bit > value)
		bit >>= 2;
	while (bit) {
		if (value >= result + bit) {
			value -= result + bit;
			result = (result >> 1) + bit;
		} else {
			result >>= 1;
		}
		bit >>= 2;
	}
	return result;
}

static void reset_stats(struct audio_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	stats->minimum = UINT16_MAX;
}

static void add_sample(struct audio_stats *stats, uint16_t sample,
		       uint64_t *second)
{
	int32_t centered = (int32_t)sample - 32768;

	if (sample < stats->minimum)
		stats->minimum = sample;
	if (sample > stats->maximum)
		stats->maximum = sample;
	stats->sum += centered;
	stats->square_sum += (uint32_t)(centered * centered);
	stats->samples++;
	if (stats->samples != SAMPLE_RATE)
		return;

	(*second)++;
	fprintf(stderr,
		"second=%" PRIu64 " samples=%u min=%u max=%u span=%u "
		"mean=%" PRId32 " rms=%" PRIu64 "\n",
		*second, stats->samples, stats->minimum, stats->maximum,
		stats->maximum - stats->minimum,
		stats->sum / (int32_t)stats->samples,
		integer_sqrt(stats->square_sum / stats->samples));
	fflush(stderr);
	reset_stats(stats);
}

static int write_all(const uint8_t *buffer, size_t length)
{
	while (length) {
		ssize_t written = write(STDOUT_FILENO, buffer, length);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!written) {
			errno = EIO;
			return -1;
		}
		buffer += written;
		length -= written;
	}
	return 0;
}

int main(void)
{
	struct audio_stats stats;
	uint8_t buffer[4096];
	uint64_t second = 0;
	uint8_t low_byte = 0;
	int have_low_byte = 0;

	reset_stats(&stats);
	for (;;) {
		ssize_t length = read(STDIN_FILENO, buffer, sizeof(buffer));
		size_t offset = 0;

		if (length < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "audio input: %s\n", strerror(errno));
			return 1;
		}
		if (!length)
			break;
		if (write_all(buffer, length) < 0) {
			fprintf(stderr, "audio output: %s\n", strerror(errno));
			return 1;
		}
		if (have_low_byte) {
			add_sample(&stats, low_byte | (uint16_t)buffer[0] << 8,
				   &second);
			offset = 1;
			have_low_byte = 0;
		}
		while (offset + 1 < (size_t)length) {
			add_sample(&stats, buffer[offset] |
				   (uint16_t)buffer[offset + 1] << 8, &second);
			offset += 2;
		}
		if (offset < (size_t)length) {
			low_byte = buffer[offset];
			have_low_byte = 1;
		}
	}
	if (have_low_byte) {
		fprintf(stderr, "audio input ended with an incomplete U16_LE sample\n");
		return 1;
	}
	return 0;
}
