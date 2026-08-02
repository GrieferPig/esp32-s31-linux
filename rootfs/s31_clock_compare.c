// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct s31_hosted_clock_test {
	uint32_t duration_sec;
	uint32_t cookie;
	uint64_t linux_start_ns;
	uint64_t linux_end_ns;
	uint64_t freertos_start_us;
	uint64_t freertos_end_us;
};

#define S31_HOSTED_IOC_CLOCK_TEST \
	_IOWR('S', 0x31, struct s31_hosted_clock_test)

int main(int argc, char **argv)
{
	struct s31_hosted_clock_test test = { .duration_sec = 60 };
	double linux_seconds;
	double freertos_seconds;
	double difference_ms;
	double ppm;
	char *end;
	long seconds;
	int fd;

	if (argc > 2) {
		fprintf(stderr, "Usage: %s [seconds]\n", argv[0]);
		return 2;
	}
	if (argc == 2) {
		errno = 0;
		seconds = strtol(argv[1], &end, 10);
		if (errno || *end || seconds < 1 || seconds > 600) {
			fprintf(stderr, "duration must be between 1 and 600 seconds\n");
			return 2;
		}
		test.duration_sec = (uint32_t)seconds;
	}

	fd = open("/dev/esps0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/esps0");
		return 1;
	}
	if (ioctl(fd, S31_HOSTED_IOC_CLOCK_TEST, &test)) {
		perror("S31 clock test ioctl");
		close(fd);
		return 1;
	}
	close(fd);

	linux_seconds = (test.linux_end_ns - test.linux_start_ns) / 1e9;
	freertos_seconds =
		(test.freertos_end_us - test.freertos_start_us) / 1e6;
	difference_ms = (linux_seconds - freertos_seconds) * 1000.0;
	ppm = (linux_seconds / freertos_seconds - 1.0) * 1e6;
	printf("FreeRTOS elapsed: %.6f s\n", freertos_seconds);
	printf("Linux RAW elapsed: %.9f s\n", linux_seconds);
	printf("Linux - FreeRTOS: %+.3f ms (%+.2f ppm)\n",
	       difference_ms, ppm);
	printf("IPC-synchronized cookie: %" PRIu32 "\n", test.cookie);
	return 0;
}
