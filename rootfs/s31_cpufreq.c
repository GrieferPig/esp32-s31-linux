// SPDX-License-Identifier: GPL-2.0-only

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>

#define CPUFREQ_DIR "/sys/devices/system/cpu/cpu0/cpufreq"
#define GOVERNOR_PATH CPUFREQ_DIR "/scaling_governor"
#define MIN_PATH CPUFREQ_DIR "/scaling_min_freq"
#define MAX_PATH CPUFREQ_DIR "/scaling_max_freq"
#define CUR_PATH CPUFREQ_DIR "/scaling_cur_freq"
#define AVAILABLE_PATH CPUFREQ_DIR "/scaling_available_frequencies"
#define SAMPLE_PATH "/sys/devices/system/cpu/cpufreq/ondemand/sampling_rate"

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s status\n"
		"  %s governor <performance|ondemand>\n"
		"  %s range <min-hz> <max-hz>\n"
		"  %s min <hz>\n"
		"  %s max <hz>\n"
		"  %s sample-rate <microseconds>\n"
		"\n"
		"Frequency values accept Hz, kHz, or MHz suffixes.\n",
		program, program, program, program, program, program);
}

static int read_text(const char *path, char *buffer, size_t size)
{
	int fd;
	ssize_t length;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	length = read(fd, buffer, size - 1);
	close(fd);
	if (length < 0)
		return -1;
	buffer[length] = '\0';
	while (length > 0 && isspace((unsigned char)buffer[length - 1]))
		buffer[--length] = '\0';
	return 0;
}

static int write_text(const char *path, const char *value)
{
	int fd;
	size_t length = strlen(value);
	ssize_t written;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	written = write(fd, value, length);
	close(fd);
	return written == (ssize_t)length ? 0 : -1;
}

static int parse_scaled(const char *text, uint64_t *value)
{
	char *end;
	uint64_t number;
	uint64_t scale = 1;

	errno = 0;
	number = strtoull(text, &end, 10);
	if (errno || end == text)
		return -1;
	if (*end) {
		if (strcasecmp(end, "khz") == 0)
			scale = 1000ULL;
		else if (strcasecmp(end, "mhz") == 0)
			scale = 1000000ULL;
		else if (strcasecmp(end, "ghz") == 0)
			scale = 1000000000ULL;
		else
			return -1;
	}
	if (number > UINT64_MAX / scale)
		return -1;
	*value = number * scale;
	return 0;
}

static int parse_frequency_khz(const char *text, unsigned int *khz)
{
	uint64_t hz;

	if (parse_scaled(text, &hz) || !hz || hz % 1000 ||
	    hz / 1000 > UINT_MAX)
		return -1;
	*khz = hz / 1000;
	return 0;
}

static int parse_sample_us(const char *text, unsigned int *usec)
{
	char *end;
	uint64_t value;
	uint64_t scale = 1;

	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno || end == text)
		return -1;
	if (*end) {
		if (strcasecmp(end, "ms") == 0)
			scale = 1000ULL;
		else if (strcasecmp(end, "s") == 0)
			scale = 1000000ULL;
		else
			return -1;
	}
	if (!value || value > UINT_MAX / scale)
		return -1;
	*usec = value * scale;
	return 0;
}

static int read_khz(const char *path, unsigned int *khz)
{
	char buffer[32];
	char *end;
	unsigned long value;

	if (read_text(path, buffer, sizeof(buffer)))
		return -1;
	errno = 0;
	value = strtoul(buffer, &end, 10);
	if (errno || end == buffer || *end || value > UINT_MAX)
		return -1;
	*khz = value;
	return 0;
}

static int write_khz(const char *path, unsigned int khz)
{
	char buffer[32];

	snprintf(buffer, sizeof(buffer), "%u", khz);
	return write_text(path, buffer);
}

static int show_status(void)
{
	char governor[32];
	char available[256];
	unsigned int current, minimum, maximum;

	if (read_text(GOVERNOR_PATH, governor, sizeof(governor)) ||
		read_khz(CUR_PATH, &current) || read_khz(MIN_PATH, &minimum) ||
		read_khz(MAX_PATH, &maximum)) {
		perror("s31-cpufreq: read cpufreq status");
		return 1;
	}
	printf("governor: %s\n", governor);
	printf("current:  %u kHz (%u Hz)\n", current, current * 1000U);
	printf("min:      %u kHz (%u Hz)\n", minimum, minimum * 1000U);
	printf("max:      %u kHz (%u Hz)\n", maximum, maximum * 1000U);
	if (!read_text(SAMPLE_PATH, available, sizeof(available)))
		printf("ondemand sample: %s us\n", available);
	if (!read_text(AVAILABLE_PATH, available, sizeof(available)))
		printf("available: %s kHz\n", available);
	return 0;
}

static int set_range(unsigned int minimum, unsigned int maximum)
{
	unsigned int current_min;

	if (!minimum || minimum > maximum || read_khz(MIN_PATH, &current_min)) {
		fprintf(stderr, "s31-cpufreq: invalid frequency range\n");
		return 1;
	}
	/* Keep every intermediate sysfs policy valid while changing the range. */
	if (maximum < current_min) {
		if (write_khz(MIN_PATH, minimum))
			goto fail;
		if (write_khz(MAX_PATH, maximum))
			goto fail;
	} else {
		if (write_khz(MAX_PATH, maximum))
			goto fail;
		if (write_khz(MIN_PATH, minimum))
			goto fail;
	}
	return 0;
fail:
	perror("s31-cpufreq: set frequency range");
	return 1;
}

int main(int argc, char **argv)
{
	unsigned int minimum, maximum, sample;

	if (argc == 2 && !strcmp(argv[1], "status"))
		return show_status();
	if (argc == 3 && !strcmp(argv[1], "governor")) {
		if (strcmp(argv[2], "performance") && strcmp(argv[2], "ondemand"))
			goto usage_error;
		if (write_text(GOVERNOR_PATH, argv[2])) {
			perror("s31-cpufreq: set governor");
			return 1;
		}
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "range")) {
		if (parse_frequency_khz(argv[2], &minimum) ||
			parse_frequency_khz(argv[3], &maximum))
			goto usage_error;
		return set_range(minimum, maximum);
	}
	if (argc == 3 && (!strcmp(argv[1], "min") || !strcmp(argv[1], "max"))) {
		unsigned int requested;

		if (parse_frequency_khz(argv[2], &requested))
			goto usage_error;
		if (!strcmp(argv[1], "min")) {
			if (read_khz(MAX_PATH, &maximum) || requested > maximum ||
				write_khz(MIN_PATH, requested))
				goto frequency_error;
		} else {
			if (read_khz(MIN_PATH, &minimum) || minimum > requested ||
				write_khz(MAX_PATH, requested))
				goto frequency_error;
		}
		return 0;
	}
	if (argc == 3 && (!strcmp(argv[1], "sample-rate") ||
					  !strcmp(argv[1], "sample"))) {
		char governor[32];

		if (parse_sample_us(argv[2], &sample))
			goto usage_error;
		if (write_khz(SAMPLE_PATH, sample)) {
			if (read_text(GOVERNOR_PATH, governor, sizeof(governor)) == 0 &&
				strcmp(governor, "ondemand")) {
				fprintf(stderr,
					"s31-cpufreq: select ondemand before setting sample rate\n");
				return 1;
			}
			goto sample_error;
		}
		return 0;
	}

usage_error:
	usage(argv[0]);
	return 2;
frequency_error:
	perror("s31-cpufreq: set frequency");
	return 1;
sample_error:
	perror("s31-cpufreq: set sample rate");
	return 1;
}
