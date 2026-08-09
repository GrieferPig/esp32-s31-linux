/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(int argc, char **argv)
{
	FILE *file;
	uint64_t samples = 0;
	uint64_t square_sum = 0;
	int64_t sum = 0;
	uint16_t minimum = UINT16_MAX;
	uint16_t maximum = 0;
	unsigned char bytes[2];

	if (argc != 2) {
		fprintf(stderr, "usage: %s raw-u16le-file\n", argv[0]);
		return 2;
	}
	file = fopen(argv[1], "rb");
	if (!file) {
		fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
		return 2;
	}
	while (fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes)) {
		uint16_t sample = (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
		int32_t centered = (int32_t)sample - 32768;

		if (sample < minimum)
			minimum = sample;
		if (sample > maximum)
			maximum = sample;
		sum += centered;
		square_sum += (int64_t)centered * centered;
		samples++;
	}
	if (ferror(file) || !feof(file)) {
		fprintf(stderr, "failed to read complete U16_LE samples\n");
		fclose(file);
		return 2;
	}
	fclose(file);
	if (!samples) {
		fprintf(stderr, "capture contains no samples\n");
		return 1;
	}
	printf("samples=%" PRIu64 " min=%u max=%u span=%u mean=%" PRId64
	       " rms=%" PRIu64 "\n",
	       samples, minimum, maximum, maximum - minimum, sum / (int64_t)samples,
	       integer_sqrt(square_sum / samples));
	if (samples < 48000 || maximum - minimum < 16) {
		fprintf(stderr, "ambient microphone has insufficient varying data\n");
		return 1;
	}
	return 0;
}
