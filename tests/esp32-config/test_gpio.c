// SPDX-License-Identifier: GPL-2.0-only
/* Host regression tests: actual parser, transactions and IPC; GPIO ioctl mock. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <time.h>
static int mock_ioctl(int fd, unsigned long request, ...);
static int mock_close(int fd);
#define GPIO_IOCTL mock_ioctl
#define GPIO_CLOSE mock_close
#define main gpio_program_main
#include "../../rootfs/s31_gpio.c"
#undef main

static bool held[PINS], busy[PINS];
static struct gpio_v2_line_config hardware[PINS];
static int input_values[PINS], request_count, release_count, config_count, value_count;
static int fail_request = -1, fail_config = -1;
static char tempdir[80];

static int mock_ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	void *arg;
	int pin = fd - 1000;
	va_start(args, request);
	arg = va_arg(args, void *);
	va_end(args);
	if (request == GPIO_V2_GET_LINEINFO_IOCTL) {
		struct gpio_v2_line_info *info = arg;
		assert(info->offset < PINS);
		info->flags = busy[info->offset] || held[info->offset] ? GPIO_V2_LINE_FLAG_USED : 0;
		if (busy[info->offset]) strcpy(info->consumer, "test-peripheral");
		return 0;
	}
	if (request == GPIO_V2_GET_LINE_IOCTL) {
		struct gpio_v2_line_request *req = arg;
		pin = req->offsets[0];
		assert(req->num_lines == 1 && pin >= 0 && pin < PINS);
		if (pin == fail_request || busy[pin] || held[pin]) { errno = EBUSY; return -1; }
		assert(!strcmp(req->consumer, CONSUMER));
		held[pin] = true;
		hardware[pin] = req->config;
		req->fd = 1000 + pin;
		request_count++;
		return 0;
	}
	assert(pin >= 0 && pin < PINS && held[pin]);
	if (request == GPIO_V2_LINE_SET_CONFIG_IOCTL) {
		config_count++;
		hardware[pin] = *(struct gpio_v2_line_config *)arg;
		if (pin == fail_config) { fail_config = -1; errno = EIO; return -1; }
		return 0;
	}
	if (request == GPIO_V2_LINE_SET_VALUES_IOCTL) {
		struct gpio_v2_line_values *values = arg;
		assert(hardware[pin].flags & GPIO_V2_LINE_FLAG_OUTPUT);
		assert(values->mask == 1);
		hardware[pin].attrs[0].attr.values = values->bits;
		value_count++;
		return 0;
	}
	if (request == GPIO_V2_LINE_GET_VALUES_IOCTL) {
		struct gpio_v2_line_values *values = arg;
		assert(values->mask == 1);
		values->bits = input_values[pin];
		return 0;
	}
	assert(!"Unexpected GPIO ioctl");
	return -1;
}

static int mock_close(int fd)
{
	int pin = fd - 1000;
	assert(pin >= 0 && pin < PINS && held[pin]);
	held[pin] = false;
	memset(&hardware[pin], 0, sizeof(hardware[pin]));
	release_count++;
	return 0;
}

static void reset_state(void)
{
	memset(held, 0, sizeof(held));
	memset(busy, 0, sizeof(busy));
	memset(active, 0, sizeof(active));
	memset(saved, 0, sizeof(saved));
	memset(hardware, 0, sizeof(hardware));
	memset(input_values, 0, sizeof(input_values));
	for (int i = 0; i < PINS; i++) requests[i] = -1;
	request_count = release_count = config_count = value_count = 0;
	fail_request = fail_config = -1;
	chip_fd = 100;
	stopping = 0;
	snprintf(config_path, sizeof(config_path), "%s/gpio.conf", tempdir);
	unlink(config_path);
}

static void write_bytes(const void *data, size_t length)
{
	FILE *file = fopen(config_path, "wb");
	assert(file);
	assert(fwrite(data, 1, length, file) == length);
	assert(!fclose(file));
}

static void run_command(const char *text, bool ok)
{
	char command[128];
	assert(strlen(text) < sizeof(command));
	strcpy(command, text);
	dispatch(command);
	if (ok) assert(!strncmp(reply, "OK\n", 3));
	else assert(!strncmp(reply, "ERR ", 4));
}

static void test_parser(void)
{
	struct setting settings[PINS];
	char error[512];
	unsigned int pin;
	const char *valid = "# GPIO configuration\n0 input up\n1 low none\n2 high none\n3 application none\n61 input down\n";
	const char *invalid[] = {
		"26 input none\n", "41 high none\n", "58 input none\n", "62 low none\n",
		"4294967296 low none\n", "18446744073709551616 high none\n", "-1 high none\n",
		"1 low up\n", "1 application down\n", "1 input\n", "1 input none extra\n",
		"1 input none\n01 high none\n", "$(touch /tmp/bad) input none\n", "1 input pull-up\n"
	};
	reset_state();
	write_bytes(valid, strlen(valid));
	assert(!read_config(config_path, settings, false, error, sizeof(error)));
	assert(settings[0].mode == INPUT && settings[0].bias == UP);
	assert(settings[61].bias == DOWN);
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		write_bytes(invalid[i], strlen(invalid[i]));
		assert(read_config(config_path, settings, false, error, sizeof(error)) < 0);
	}
	const char embedded_nul[] = "1 input none\0ignored";
	write_bytes(embedded_nul, sizeof(embedded_nul) - 1);
	assert(read_config(config_path, settings, false, error, sizeof(error)) < 0);
	char oversized[200];
	memset(oversized, ' ', sizeof(oversized));
	write_bytes(oversized, sizeof(oversized));
	assert(read_config(config_path, settings, false, error, sizeof(error)) < 0);
	assert(parse_line("4294967296", &pin) < 0);
	assert(!parse_line("61", &pin) && pin == 61);
}

static void test_owned_reconfiguration(void)
{
	struct setting settings[PINS];
	struct stat st;
	char error[512];
	reset_state();
	run_command("set 4 low", true);
	assert(request_count == 1 && held[4] && hardware[4].attrs[0].attr.values == 0);
	assert(hardware[4].flags & GPIO_V2_LINE_FLAG_BIAS_DISABLED);
	run_command("set 4 high", true);
	assert(request_count == 1 && release_count == 0 && value_count == 1);
	assert(hardware[4].attrs[0].attr.values == 1);
	run_command("set 4 input up", true);
	assert(request_count == 1 && release_count == 0 && config_count == 1);
	assert(hardware[4].flags & GPIO_V2_LINE_FLAG_INPUT);
	assert(hardware[4].flags & GPIO_V2_LINE_FLAG_BIAS_PULL_UP);
	input_values[4] = 1;
	run_command("read 4", true);
	assert(!strcmp(reply, "OK\n1\n") && request_count == 1);
	run_command("set 4 input up", true);
	assert(config_count == 1 && release_count == 0);
	assert(!read_config(config_path, settings, false, error, sizeof(error)));
	assert(settings[4].mode == INPUT && settings[4].bias == UP);
	assert(!stat(config_path, &st) && (st.st_mode & 0777) == 0600);
	run_command("set 4 application", true);
	assert(!held[4] && release_count == 1 && active[4].mode == APPLICATION);
	assert(!read_config(config_path, settings, false, error, sizeof(error)));
	assert(settings[4].mode == APPLICATION);
	run_command("read 4", false);
}

static void test_failures_preserve_state(void)
{
	struct setting target[PINS], parsed[PINS];
	char error[512] = "", good_path[1024];
	reset_state();
	run_command("set 4 high", true);
	busy[5] = true;
	run_command("set 5 low", false);
	assert(held[4] && !held[5] && saved[5].mode == APPLICATION && request_count == 1);
	run_command("set 26 low", false);
	run_command("set 4294967296 high", false);
	assert(request_count == 1);
	fail_config = 4;
	run_command("set 4 input down", false);
	assert(held[4] && active[4].mode == HIGH && (hardware[4].flags & GPIO_V2_LINE_FLAG_OUTPUT));
	assert(hardware[4].attrs[0].attr.values == 1 && release_count == 0);
	strcpy(good_path, config_path);
	snprintf(config_path, sizeof(config_path), "%s/missing/gpio.conf", tempdir);
	run_command("set 4 low", false);
	assert(held[4] && active[4].mode == HIGH && hardware[4].attrs[0].attr.values == 1);
	run_command("set 4 application", false);
	assert(held[4] && release_count == 0);
	run_command("set 6 input up", false);
	assert(!held[6] && held[4] && release_count == 1);
	strcpy(config_path, good_path);
	assert(!read_config(config_path, parsed, false, error, sizeof(error)));
	assert(parsed[4].mode == HIGH && parsed[6].mode == APPLICATION);
	memcpy(target, active, sizeof(target));
	target[4] = (struct setting){ INPUT, DOWN };
	target[7] = (struct setting){ LOW, NONE };
	target[8] = (struct setting){ HIGH, NONE };
	fail_request = 8;
	assert(apply_settings(target, NULL, false, error, sizeof(error)) < 0);
	assert(held[4] && !held[7] && !held[8] && active[4].mode == HIGH);
	assert(hardware[4].attrs[0].attr.values == 1);
}

static void test_restore_and_inspection(void)
{
	const char *config = "4 input down\n5 high none\n";
	reset_state();
	write_bytes(config, strlen(config));
	busy[5] = true;
	run_command("apply", false);
	assert(!held[4] && !held[5] && saved[4].mode == INPUT && saved[5].mode == HIGH);
	run_command("list", true);
	assert(strstr(reply, "5\tbusy\tnone\t-\ttest-peripheral\thigh\tnone"));
	assert(!held[4] && !held[5]);
	busy[5] = false;
	run_command("apply", true);
	assert(held[4] && held[5]);
	assert(hardware[4].flags & GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN);
	input_values[4] = 1;
	run_command("list", true);
	assert(strstr(reply, "4\tinput\tdown\t1\tesp32-config\tinput\tdown"));
	run_command("reset", true);
	assert(!held[4] && !held[5]);
}

static void test_external_restore_is_visible(void)
{
	const char *replacement = "4 input up\n5 high none\n";
	reset_state();
	run_command("set 4 low", true);
	write_bytes(replacement, strlen(replacement));
	run_command("list", true);
	assert(strstr(reply, "4\tlow\tnone\t0\tesp32-config\tinput\tup"));
	assert(held[4] && !held[5] && active[4].mode == LOW);
	/* An edit preserves imported settings without applying unrelated pins. */
	run_command("set 6 input down", true);
	assert(held[4] && !held[5] && held[6] && active[4].mode == LOW);
	assert(saved[5].mode == HIGH);
	run_command("apply", true);
	assert(held[5] && active[4].mode == INPUT);
	write_bytes("", 0);
	run_command("list", true);
	assert(strstr(reply, "4\tinput\tup\t0\tesp32-config\tapplication\tnone"));
	assert(held[4]);
	run_command("apply", true);
	assert(!held[4] && !held[5] && !held[6]);
}

static void test_ipc(void)
{
	int status;
	pid_t daemon, unprivileged;
	struct stat st;
	int capability = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (capability < 0 && (errno == EPERM || errno == EACCES)) {
		puts("SKIP GPIO IPC runtime test: this host blocks Unix sockets.");
		return;
	}
	assert(capability >= 0);
	close(capability);
	reset_state();
	strcpy(run_path, tempdir);
	snprintf(socket_path, sizeof(socket_path), "%s/gpio.sock", tempdir);
	daemon = fork();
	assert(daemon >= 0);
	if (!daemon) _exit(serve());
	int result = -1;
	for (int i = 0; i < 200; i++) {
		result = client_command("ping", true);
		if (!result) break;
		usleep(10000);
	}
	assert(!result);
	assert(!stat(socket_path, &st) && (st.st_mode & 0777) == 0600);
	assert(!client_command("set 4 high", true));
	assert(!client_command("read 4", true) && !strcmp(reply, "OK\n1\n"));
	assert(client_command("set 26 high", true) == 1);
	unprivileged = fork();
	assert(unprivileged >= 0);
	if (!unprivileged) {
		assert(!setuid(65534));
		_exit(client_command("reset", true) < 0 ? 0 : 1);
	}
	assert(waitpid(unprivileged, &status, 0) == unprivileged && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(!client_command("stop", true));
	assert(waitpid(daemon, &status, 0) == daemon && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(access(socket_path, F_OK) < 0);
}

int main(void)
{
	strcpy(tempdir, "/tmp/s31-gpio-tests-XXXXXX");
	assert(mkdtemp(tempdir));
	test_parser();
	test_owned_reconfiguration();
	test_failures_preserve_state();
	test_restore_and_inspection();
	test_external_restore_is_visible();
	if (!geteuid()) test_ipc();
	else puts("SKIP root-only GPIO IPC test (run as root to include it)");
	char lock[120];
	snprintf(lock, sizeof(lock), "%s/gpio.lock", tempdir);
	unlink(lock);
	unlink(config_path);
	assert(!rmdir(tempdir));
	puts("GPIO host tests passed.");
	return 0;
}
