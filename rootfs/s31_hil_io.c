// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <linux/spi/spidev.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

static uint32_t crc32(const uint8_t *data, size_t length)
{
	uint32_t crc = ~0U;

	while (length--) {
		crc ^= *data++;
		for (unsigned int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1));
	}
	return ~crc;
}

static speed_t uart_speed(unsigned long baud)
{
	switch (baud) {
	case 9600: return B9600;
	case 115200: return B115200;
#ifdef B230400
	case 230400: return B230400;
#endif
#ifdef B460800
	case 460800: return B460800;
#endif
#ifdef B921600
	case 921600: return B921600;
#endif
	default: return 0;
	}
}

static int write_all(int fd, const uint8_t *buffer, size_t length)
{
	while (length) {
		ssize_t done = write(fd, buffer, length);
		if (done < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				struct pollfd pfd = { .fd = fd, .events = POLLOUT };
				if (poll(&pfd, 1, 3000) > 0)
					continue;
			}
			return -1;
		}
		buffer += done;
		length -= done;
	}
	return 0;
}

static int uart_test(const char *device, unsigned long baud, size_t length,
		     int loopback)
{
	uint8_t *tx = NULL, *rx = NULL;
	struct termios tio;
	speed_t speed = uart_speed(baud);
	size_t received = 0;
	int fd = -1, ret = 1;

	if (!speed || length == 0 || length > 65536) {
		fprintf(stderr, "invalid UART baud/length\n");
		return 2;
	}
	tx = malloc(length);
	rx = malloc(length);
	if (!tx || !rx)
		goto out;
	for (size_t i = 0; i < length; i++)
		tx[i] = (uint8_t)(i * 37U + (i >> 8) + 11U);

	fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0 || tcgetattr(fd, &tio))
		goto out;
	cfmakeraw(&tio);
	tio.c_cflag |= CLOCAL | CREAD;
	cfsetispeed(&tio, speed);
	cfsetospeed(&tio, speed);
	if (tcsetattr(fd, TCSANOW, &tio))
		goto out;
	if (loopback) {
		int mask = TIOCM_LOOP;

		if (ioctl(fd, TIOCMBIS, &mask))
			goto out;
		/*
		 * UHCI RX is armed by open().  UART3's otherwise-unroutable RX
		 * input may produce an idle-EOF byte before internal loopback is
		 * selected, so establish the loop first and flush that startup
		 * event only after the new path has settled.
		 */
		usleep(20000);
	}
	if (tcflush(fd, TCIOFLUSH))
		goto out;
	while (received < length) {
		size_t chunk = length - received;
		if (chunk > 128)
			chunk = 128;
		if (write_all(fd, tx + received, chunk))
			goto out;
		size_t target = received + chunk;
		while (received < target) {
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			int ready = poll(&pfd, 1, 3000);
			if (ready <= 0)
				goto out;
			ssize_t done = read(fd, rx + received, target - received);
			if (done > 0)
				received += done;
			else if (done < 0 && errno != EAGAIN && errno != EINTR)
				goto out;
		}
	}
	if (memcmp(tx, rx, length)) {
		size_t mismatch = 0;
		while (mismatch < length && tx[mismatch] == rx[mismatch])
			mismatch++;
		fprintf(stderr,
			"UART payload mismatch at baud %lu offset=%zu expected=%02x got=%02x\n",
			baud, mismatch, tx[mismatch], rx[mismatch]);
		goto out;
	}
	printf("PASS uart%s device=%s baud=%lu bytes=%zu crc32=%08x\n",
	       loopback ? "-loopback" : "", device, baud, length,
	       crc32(rx, length));
	ret = 0;
out:
	if (ret)
		fprintf(stderr, "FAIL uart%s device=%s baud=%lu received=%zu: %s\n",
			loopback ? "-loopback" : "", device, baud, received,
			strerror(errno));
	if (fd >= 0 && loopback) {
		int mask = TIOCM_LOOP;

		ioctl(fd, TIOCMBIC, &mask);
	}
	if (fd >= 0)
		close(fd);
	free(tx);
	free(rx);
	return ret;
}

static int spi_test(const char *device, unsigned int mode, uint32_t speed,
		    size_t length, int tx_only, int rx_only, uint8_t bits)
{
	uint8_t *tx = NULL, *rx = NULL;
	uint8_t spi_mode = mode;
	struct spi_ioc_transfer transfer = {0};
	int fd = -1, ret = 1;

	if (mode > 3 || !speed || !length || length > 65536)
		return 2;
	if ((bits != 8 && bits != 16 && bits != 32) || length % (bits / 8))
		return 2;
	tx = malloc(length);
	rx = calloc(length, 1);
	if (!tx || !rx)
		goto out;
	for (size_t i = 0; i < length; i++) {
		size_t wire = i / (bits / 8) * (bits / 8) + (bits / 8) - 1 - i % (bits / 8);
		tx[i] = (uint8_t)(wire * 37U + 11U);
	}
	fd = open(device, O_RDWR);
	if (fd < 0 || ioctl(fd, SPI_IOC_WR_MODE, &spi_mode) < 0 ||
	    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
	    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0)
		goto out;
	transfer.tx_buf = rx_only ? 0 : (uintptr_t)tx;
	transfer.rx_buf = tx_only ? 0 : (uintptr_t)rx;
	transfer.len = length;
	transfer.speed_hz = speed;
	transfer.bits_per_word = bits;
	{
		int ioctl_ret;

		errno = 0;
		ioctl_ret = ioctl(fd, SPI_IOC_MESSAGE(1), &transfer);
		if (ioctl_ret != (int)length) {
			fprintf(stderr,
				"SPI ioctl returned %d expected %zu errno=%d\n",
				ioctl_ret, length, errno);
			goto out;
		}
	}
	for (size_t i = 0; !tx_only && i < length; i++) {
		size_t wire = i / (bits / 8) * (bits / 8) + (bits / 8) - 1 - i % (bits / 8);
		if (rx[i] != (uint8_t)(wire ^ 0xa5U)) {
			fprintf(stderr, "SPI mismatch at %zu: %02x\n", i, rx[i]);
			goto out;
		}
	}
	printf("PASS spi%s%s device=%s mode=%u speed=%u bytes=%zu tx_crc32=%08x rx_crc32=%08x\n",
	       tx_only ? "-tx" : "", rx_only ? "-rx" : "", device, mode,
	       speed, length, crc32(tx, length), crc32(rx, length));
	ret = 0;
out:
	if (ret)
		fprintf(stderr, "FAIL spi device=%s mode=%u speed=%u bytes=%zu: %s\n",
			device, mode, speed, length, strerror(errno));
	if (fd >= 0)
		close(fd);
	free(tx);
	free(rx);
	return ret;
}

static int i2c_transfer(int fd, struct i2c_msg *messages, unsigned int count)
{
	struct i2c_rdwr_ioctl_data request = {
		.msgs = messages,
		.nmsgs = count,
	};

	return ioctl(fd, I2C_RDWR, &request);
}

static int i2c_test(const char *device)
{
	uint8_t reg = 0x20, readback[16] = {0};
	uint8_t write_packet[17], verify_reg = 0x80, verify[16] = {0};
	uint8_t bad = 0;
	int fd = open(device, O_RDWR);

	if (fd < 0)
		goto fail;
	struct i2c_msg read_messages[] = {
		{ .addr = 0x42, .len = 1, .buf = &reg },
		{ .addr = 0x42, .flags = I2C_M_RD,
		  .len = sizeof(readback), .buf = readback },
	};
	if (i2c_transfer(fd, read_messages, 2) < 0)
		goto fail;
	for (size_t i = 0; i < sizeof(readback); ++i) {
		if (readback[i] != (uint8_t)(i ^ 0x5aU)) {
			fprintf(stderr,
				"I2C repeated-start mismatch offset=%zu expected=%02x got=%02x\n",
				i, (uint8_t)(i ^ 0x5aU), readback[i]);
			errno = EILSEQ;
			goto fail;
		}
	}

	write_packet[0] = verify_reg;
	for (size_t i = 1; i < sizeof(write_packet); ++i)
		write_packet[i] = (uint8_t)(i * 13U + 7U);
	struct i2c_msg write_message = {
		.addr = 0x42, .len = sizeof(write_packet), .buf = write_packet,
	};
	if (i2c_transfer(fd, &write_message, 1) < 0)
		goto fail;
	usleep(10000);
	struct i2c_msg verify_message = {
		.addr = 0x42, .flags = I2C_M_RD,
		.len = sizeof(verify), .buf = verify,
	};
	if (i2c_transfer(fd, &verify_message, 1) < 0 ||
	    memcmp(verify, write_packet + 1, sizeof(verify))) {
		errno = EILSEQ;
		goto fail;
	}

	struct i2c_msg nack_message = {
		.addr = 0x43, .len = 1, .buf = &bad,
	};
	if (i2c_transfer(fd, &nack_message, 1) >= 0) {
		errno = EPROTO;
		goto fail;
	}
	printf("PASS i2c device=%s addr=0x42 repeated-start=16 write-read=16 nack=0x43 crc32=%08x\n",
	       device, crc32(verify, sizeof(verify)));
	close(fd);
	return 0;

fail:
	fprintf(stderr, "FAIL i2c device=%s: %s\n", device, strerror(errno));
	if (fd >= 0)
		close(fd);
	return 1;
}

static int i2c_long_test(const char *device, size_t length)
{
	uint8_t reg = 0;
	uint8_t *rx = NULL, *tx = NULL;
	int fd = -1, ret = 1;

	if (!length || length > 4096)
		return 2;
	rx = calloc(length, 1);
	tx = malloc(length + 1);
	if (!rx || !tx)
		goto out;
	fd = open(device, O_RDWR);
	if (fd < 0)
		goto out;
	struct i2c_msg combined[] = {
		{ .addr = 0x42, .len = 1, .buf = &reg },
		{ .addr = 0x42, .flags = I2C_M_RD, .len = length, .buf = rx },
	};
	if (i2c_transfer(fd, combined, 2) < 0)
		goto out;
	for (size_t i = 0; i < length; i++) {
		if (rx[i] != (uint8_t)(i ^ 0x5aU)) {
			fprintf(stderr, "I2C long RX mismatch offset=%zu got=%02x\n", i, rx[i]);
			errno = EILSEQ;
			goto out;
		}
	}
	tx[0] = 0;
	for (size_t i = 1; i <= length; i++)
		tx[i] = (uint8_t)(i * 13U + 7U);
	struct i2c_msg write = { .addr = 0x42, .len = length + 1, .buf = tx };
	if (i2c_transfer(fd, &write, 1) < 0)
		goto out;
	printf("PASS i2c-long rx_bytes=%zu rx_crc32=%08x tx_bytes=%zu tx_crc32=%08x\n",
	       length, crc32(rx, length), length + 1, crc32(tx, length + 1));
	ret = 0;
out:
	if (ret)
		fprintf(stderr, "FAIL i2c-long bytes=%zu: %s\n", length, strerror(errno));
	if (fd >= 0)
		close(fd);
	free(rx);
	free(tx);
	return ret;
}

static int udp_echo_test(const char *address, unsigned long port,
			 size_t length, unsigned long count)
{
	uint8_t *tx = NULL, *rx = NULL;
	struct sockaddr_in peer = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
	};
	struct timeval timeout = { .tv_sec = 2 };
	int fd = -1, ret = 1;

	if (!port || port > 65535 || !length || length > 1472 ||
	    !count || count > 10000 || inet_pton(AF_INET, address, &peer.sin_addr) != 1)
		return 2;
	tx = malloc(length);
	rx = malloc(length);
	if (!tx || !rx)
		goto out;
	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
				 &timeout, sizeof(timeout)) ||
	    connect(fd, (struct sockaddr *)&peer, sizeof(peer)))
		goto out;
	for (unsigned long packet = 0; packet < count; packet++) {
		for (size_t i = 0; i < length; i++)
			tx[i] = (uint8_t)(i * 37U + packet * 13U + 11U);
		ssize_t done = send(fd, tx, length, 0);
		if (done != (ssize_t)length)
			goto out;
		done = recv(fd, rx, length, 0);
		if (done != (ssize_t)length || memcmp(tx, rx, length)) {
			errno = EILSEQ;
			goto out;
		}
	}
	printf("PASS udp-echo peer=%s:%lu bytes=%zu packets=%lu total=%zu crc32=%08x\n",
	       address, port, length, count, length * (size_t)count, crc32(rx, length));
	ret = 0;
out:
	if (ret)
		fprintf(stderr,
			"FAIL udp-echo peer=%s:%lu bytes=%zu packets=%lu: %s\n",
			address, port, length, count, strerror(errno));
	if (fd >= 0)
		close(fd);
	free(tx);
	free(rx);
	return ret;
}

static int pattern_file(const char *path, size_t length, int verify)
{
	uint8_t buffer[1024];
	size_t offset = 0;
	int fd = open(path, verify ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd < 0 || !length || length > 1024 * 1024)
		goto fail;
	while (offset < length) {
		size_t chunk = length - offset;
		if (chunk > sizeof(buffer))
			chunk = sizeof(buffer);
		if (verify) {
			ssize_t done = read(fd, buffer, chunk);
			if (done != (ssize_t)chunk)
				goto fail;
			for (size_t i = 0; i < chunk; ++i) {
				uint8_t expected = (uint8_t)((offset + i) * 37U + 11U);
				if (buffer[i] != expected) {
					fprintf(stderr,
						"pattern mismatch at %zu expected=%02x got=%02x\n",
						offset + i, expected, buffer[i]);
					errno = EILSEQ;
					goto fail;
				}
			}
		} else {
			for (size_t i = 0; i < chunk; ++i)
				buffer[i] = (uint8_t)((offset + i) * 37U + 11U);
			if (write_all(fd, buffer, chunk))
				goto fail;
		}
		offset += chunk;
	}
	close(fd);
	printf("PASS pattern-%s file=%s bytes=%zu\n",
	       verify ? "check" : "write", path, length);
	return 0;
fail:
	if (fd >= 0)
		close(fd);
	fprintf(stderr, "FAIL pattern-%s file=%s bytes=%zu: %s\n",
		verify ? "check" : "write", path, length, strerror(errno));
	return 1;
}

static int pattern_stream_check(const char *path, size_t length,
				size_t minimum_run)
{
	uint8_t *buffer = NULL;
	size_t best_start = 0, best_run = 0, best_phase = 0;
	size_t run_start = 0, run = 0, phase = 0;
	int fd = -1;

	if (!length || length > 1024 * 1024 || !minimum_run ||
	    minimum_run > length) {
		errno = EINVAL;
		goto fail;
	}
	buffer = malloc(length);
	fd = open(path, O_RDONLY);
	if (!buffer || fd < 0)
		goto fail;
	for (size_t received = 0; received < length;) {
		ssize_t done = read(fd, buffer + received, length - received);

		if (done <= 0)
			goto fail;
		received += done;
	}
	close(fd);
	fd = -1;

	for (size_t i = 0; i < length; ++i) {
		if (run && buffer[i] ==
		    (uint8_t)((phase + run) * 37U + 11U)) {
			run++;
		} else {
			if (run > best_run) {
				best_start = run_start;
				best_run = run;
				best_phase = phase;
			}
			run_start = i;
			/* 173 is the multiplicative inverse of 37 modulo 256. */
			phase = (uint8_t)((buffer[i] - 11U) * 173U);
			run = 1;
		}
	}
	if (run > best_run) {
		best_start = run_start;
		best_run = run;
		best_phase = phase;
	}
	if (best_run < minimum_run) {
		errno = EILSEQ;
		goto fail;
	}
	free(buffer);
	printf("PASS pattern-stream-check file=%s bytes=%zu start=%zu phase=%zu run=%zu minimum=%zu\n",
	       path, length, best_start, best_phase, best_run, minimum_run);
	return 0;
fail:
	if (fd >= 0)
		close(fd);
	free(buffer);
	fprintf(stderr,
		"FAIL pattern-stream-check file=%s bytes=%zu best-start=%zu phase=%zu run=%zu minimum=%zu: %s\n",
		path, length, best_start, best_phase, best_run, minimum_run,
		strerror(errno));
	return 1;
}

int main(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "uart"))
		return uart_test(argv[2], strtoul(argv[3], NULL, 0),
				 strtoul(argv[4], NULL, 0), 0);
	if (argc == 5 && !strcmp(argv[1], "uart-loopback"))
		return uart_test(argv[2], strtoul(argv[3], NULL, 0),
				 strtoul(argv[4], NULL, 0), 1);
	if ((argc == 6 || argc == 7) && !strcmp(argv[1], "spi"))
		return spi_test(argv[2], strtoul(argv[3], NULL, 0),
				strtoul(argv[4], NULL, 0), strtoul(argv[5], NULL, 0),
				0, 0, argc == 7 ? strtoul(argv[6], NULL, 0) : 8);
	if (argc == 6 && !strcmp(argv[1], "spi-tx"))
		return spi_test(argv[2], strtoul(argv[3], NULL, 0),
				strtoul(argv[4], NULL, 0), strtoul(argv[5], NULL, 0),
				1, 0, 8);
	if (argc == 6 && !strcmp(argv[1], "spi-rx"))
		return spi_test(argv[2], strtoul(argv[3], NULL, 0),
				strtoul(argv[4], NULL, 0), strtoul(argv[5], NULL, 0),
				0, 1, 8);
	if (argc == 3 && !strcmp(argv[1], "i2c"))
		return i2c_test(argv[2]);
	if (argc == 4 && !strcmp(argv[1], "i2c-long"))
		return i2c_long_test(argv[2], strtoul(argv[3], NULL, 0));
	if (argc == 6 && !strcmp(argv[1], "udp-echo"))
		return udp_echo_test(argv[2], strtoul(argv[3], NULL, 0),
				     strtoul(argv[4], NULL, 0),
				     strtoul(argv[5], NULL, 0));
	if (argc == 4 && !strcmp(argv[1], "pattern-write"))
		return pattern_file(argv[2], strtoul(argv[3], NULL, 0), 0);
	if (argc == 4 && !strcmp(argv[1], "pattern-check"))
		return pattern_file(argv[2], strtoul(argv[3], NULL, 0), 1);
	if (argc == 5 && !strcmp(argv[1], "pattern-stream-check"))
		return pattern_stream_check(argv[2], strtoul(argv[3], NULL, 0),
					    strtoul(argv[4], NULL, 0));
	fprintf(stderr, "usage: %s uart DEVICE BAUD LENGTH\n", argv[0]);
	fprintf(stderr, "       %s uart-loopback DEVICE BAUD LENGTH\n", argv[0]);
	fprintf(stderr, "       %s spi DEVICE MODE SPEED LENGTH [BITS]\n", argv[0]);
	fprintf(stderr, "       %s spi-tx|spi-rx DEVICE MODE SPEED LENGTH\n",
		argv[0]);
	fprintf(stderr, "       %s i2c DEVICE\n", argv[0]);
	fprintf(stderr, "       %s i2c-long DEVICE LENGTH\n", argv[0]);
	fprintf(stderr, "       %s udp-echo ADDRESS PORT LENGTH COUNT\n", argv[0]);
	fprintf(stderr, "       %s pattern-write|pattern-check FILE LENGTH\n", argv[0]);
	fprintf(stderr, "       %s pattern-stream-check FILE LENGTH MINIMUM-RUN\n",
		argv[0]);
	return 2;
}
