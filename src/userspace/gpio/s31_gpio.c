// SPDX-License-Identifier: GPL-2.0-only
/* Persistent GPIO ownership for esp32-config. One process holds all requests. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/gpio.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef GPIO_IOCTL
#define GPIO_IOCTL ioctl
#endif
#ifndef GPIO_CLOSE
#define GPIO_CLOSE close
#endif

#define PINS 62
#define CHIP_LABEL "20583000.pinctrl"
#define CONSUMER "esp32-config"
#define REPLY_SIZE 16384

enum mode { APPLICATION, INPUT, LOW, HIGH };
enum bias { NONE, UP, DOWN };
struct setting { enum mode mode; enum bias bias; };
static struct setting active[PINS], saved[PINS];
static int requests[PINS], chip_fd = -1;
static char config_path[1024], run_path[1024], socket_path[108];
static char reply[REPLY_SIZE];
static size_t reply_used;
static volatile sig_atomic_t stopping;

static bool safe_line(unsigned int line)
{
	return line < PINS && !(line >= 26 && line <= 34) &&
		line != 41 && line != 58 && line != 59;
}

static const char *mode_name(enum mode mode)
{
	static const char * const names[] = { "application", "input", "low", "high" };
	return names[mode];
}

static const char *bias_name(enum bias bias)
{
	static const char * const names[] = { "none", "up", "down" };
	return names[bias];
}

static bool equal(struct setting a, struct setting b)
{
	return a.mode == b.mode && a.bias == b.bias;
}

static int parse_line(const char *s, unsigned int *line)
{
	char *end;
	unsigned long value;
	if (!*s || strspn(s, "0123456789") != strlen(s))
		return -1;
	errno = 0;
	value = strtoul(s, &end, 10);
	if (errno || *end || value >= PINS || !safe_line((unsigned int)value))
		return -1;
	*line = value;
	return 0;
}

static int parse_setting(const char *mode, const char *bias, struct setting *s)
{
	if (!strcmp(mode, "application")) s->mode = APPLICATION;
	else if (!strcmp(mode, "input")) s->mode = INPUT;
	else if (!strcmp(mode, "low")) s->mode = LOW;
	else if (!strcmp(mode, "high")) s->mode = HIGH;
	else return -1;
	if (!bias || !strcmp(bias, "none")) s->bias = NONE;
	else if (!strcmp(bias, "up")) s->bias = UP;
	else if (!strcmp(bias, "down")) s->bias = DOWN;
	else return -1;
	return s->mode != INPUT && s->bias != NONE ? -1 : 0;
}

static int read_config(const char *path, struct setting *settings,
		       bool missing_ok, char *error, size_t size)
{
	char line[160], number[24], mode[24], bias[24], extra[2];
	bool seen[PINS] = { false };
	unsigned int offset, lineno = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	FILE *file;
	struct stat st;
	memset(settings, 0, PINS * sizeof(*settings));
	if (fd < 0) {
		if (errno == ENOENT && missing_ok) return 0;
		goto fail;
	}
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 10000) {
		close(fd);
		errno = EINVAL;
		goto fail;
	}
	file = fdopen(fd, "r");
	if (!file) { close(fd); goto fail; }
	for (;;) {
		size_t length = 0;
		int ch;
		char *p = line;
		while ((ch = fgetc(file)) != EOF && ch != '\n') {
			if (!ch || length >= sizeof(line) - 1) goto invalid;
			line[length++] = (char)ch;
		}
		if (ch == EOF && length == 0) break;
		line[length] = '\0';
		lineno++;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\0') continue;
		if (sscanf(p, "%23s %23s %23s %1s", number, mode, bias, extra) != 3 ||
		    parse_line(number, &offset) || seen[offset] ||
		    parse_setting(mode, bias, &settings[offset])) goto invalid;
		seen[offset] = true;
	}
	if (ferror(file)) { fclose(file); goto fail; }
	fclose(file);
	return 0;
invalid:
	fclose(file);
	snprintf(error, size, "Invalid GPIO configuration at line %u.", lineno);
	return -1;
fail:
	snprintf(error, size, "Cannot read GPIO configuration: %s.", strerror(errno));
	return -1;
}

static int write_config(const struct setting *settings, char *error, size_t size)
{
	char temporary[1100], directory[1024], *slash;
	int fd, directory_fd, failure = 0;
	FILE *file;
	if (snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", config_path) >=
	    (int)sizeof(temporary)) { errno = ENAMETOOLONG; goto fail; }
	strcpy(directory, config_path);
	slash = strrchr(directory, '/');
	if (!slash) { errno = EINVAL; goto fail; }
	*slash = '\0';
	directory_fd = open(*directory ? directory : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (directory_fd < 0) goto fail;
	fd = mkstemp(temporary);
	if (fd < 0) { close(directory_fd); goto fail; }
	file = fdopen(fd, "w");
	if (!file) { close(fd); unlink(temporary); close(directory_fd); goto fail; }
	if (fchmod(fd, 0600) || fprintf(file, "# esp32-config GPIO assignments\n") < 0)
		failure = 1;
	for (unsigned int i = 0; i < PINS && !failure; i++)
		if (settings[i].mode != APPLICATION &&
		    fprintf(file, "%u %s %s\n", i, mode_name(settings[i].mode),
			    bias_name(settings[i].bias)) < 0) failure = 1;
	if (fflush(file) || fsync(fd)) failure = 1;
	if (fclose(file)) failure = 1;
	if (!failure && rename(temporary, config_path)) failure = 1;
	if (failure) {
		int saved_errno = errno;
		unlink(temporary);
		close(directory_fd);
		errno = saved_errno ? saved_errno : EIO;
		goto fail;
	}
	/* rename has committed. A directory sync failure cannot undo that commit. */
	if (fsync(directory_fd))
		snprintf(error, size, "Applied and saved; directory sync failed: %s.", strerror(errno));
	close(directory_fd);
	return 0;
fail:
	snprintf(error, size, "Cannot save GPIO configuration: %s.", strerror(errno));
	return -1;
}

static int open_chip(void)
{
	glob_t paths = {0};
	struct gpiochip_info info;
	int fd = -1;
	if (chip_fd >= 0) return 0;
	if (glob("/dev/gpiochip[0-9]*", 0, NULL, &paths)) {
		globfree(&paths);
		errno = ENODEV;
		return -1;
	}
	for (size_t i = 0; i < paths.gl_pathc; i++) {
		fd = open(paths.gl_pathv[i], O_RDONLY | O_CLOEXEC);
		if (fd < 0) continue;
		memset(&info, 0, sizeof(info));
		if (!GPIO_IOCTL(fd, GPIO_GET_CHIPINFO_IOCTL, &info) &&
		    info.lines == PINS && !strcmp(info.label, CHIP_LABEL)) break;
		close(fd);
		fd = -1;
	}
	globfree(&paths);
	if (fd < 0) { errno = ENODEV; return -1; }
	chip_fd = fd;
	return 0;
}

static int line_info(unsigned int pin, struct gpio_v2_line_info *info)
{
	memset(info, 0, sizeof(*info));
	info->offset = pin;
	return GPIO_IOCTL(chip_fd, GPIO_V2_GET_LINEINFO_IOCTL, info);
}

static void make_config(struct setting setting, struct gpio_v2_line_config *config)
{
	memset(config, 0, sizeof(*config));
	config->flags = setting.mode == INPUT ? GPIO_V2_LINE_FLAG_INPUT : GPIO_V2_LINE_FLAG_OUTPUT;
	config->flags |= setting.bias == UP ? GPIO_V2_LINE_FLAG_BIAS_PULL_UP :
		setting.bias == DOWN ? GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN : GPIO_V2_LINE_FLAG_BIAS_DISABLED;
	if (setting.mode == LOW || setting.mode == HIGH) {
		config->num_attrs = 1;
		config->attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
		config->attrs[0].attr.values = setting.mode == HIGH;
		config->attrs[0].mask = 1;
	}
}

static int configure_line(unsigned int pin, struct setting setting)
{
	struct gpio_v2_line_request request = {0};
	struct gpio_v2_line_config config;
	make_config(setting, &config);
	if (requests[pin] >= 0) {
		if ((active[pin].mode == LOW || active[pin].mode == HIGH) &&
		    (setting.mode == LOW || setting.mode == HIGH)) {
			struct gpio_v2_line_values values = { .mask = 1, .bits = setting.mode == HIGH };
			if (GPIO_IOCTL(requests[pin], GPIO_V2_LINE_SET_VALUES_IOCTL, &values)) return -1;
		} else if (GPIO_IOCTL(requests[pin], GPIO_V2_LINE_SET_CONFIG_IOCTL, &config)) return -1;
	} else {
		struct gpio_v2_line_info info;
		if (line_info(pin, &info)) return -1;
		if (info.flags & GPIO_V2_LINE_FLAG_USED) { errno = EBUSY; return -1; }
		request.offsets[0] = pin;
		request.num_lines = 1;
		request.config = config;
		strcpy(request.consumer, CONSUMER);
		if (GPIO_IOCTL(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request)) return -1;
		requests[pin] = request.fd;
		fcntl(request.fd, F_SETFD, FD_CLOEXEC);
	}
	active[pin] = setting;
	return 0;
}

static void release_line(unsigned int pin)
{
	if (requests[pin] >= 0) GPIO_CLOSE(requests[pin]);
	requests[pin] = -1;
	active[pin] = (struct setting){ APPLICATION, NONE };
}

static int apply_settings(const struct setting *target, const struct setting *desired,
			  bool persist, char *error, size_t size)
{
	struct setting before[PINS];
	bool changed[PINS] = { false };
	bool rollback_failed = false;
	if (!desired) desired = target;
	memcpy(before, active, sizeof(before));
	for (unsigned int i = 0; i < PINS; i++) {
		if (target[i].mode == APPLICATION || equal(target[i], active[i])) continue;
		changed[i] = true;
		if (configure_line(i, target[i])) {
			snprintf(error, size, "GPIO%u could not be configured: %s.", i, strerror(errno));
			goto rollback;
		}
	}
	if (persist && write_config(desired, error, size)) goto rollback;
	/* Defer releases until new requests and the saved configuration succeed. */
	for (unsigned int i = 0; i < PINS; i++)
		if (target[i].mode == APPLICATION) release_line(i);
	memmove(saved, desired, sizeof(saved));
	return 0;
rollback:
	for (int i = PINS - 1; i >= 0; i--) {
		if (!changed[i]) continue;
		if (before[i].mode == APPLICATION) release_line(i);
		else {
			struct gpio_v2_line_config config;
			/* A failed SET_CONFIG may have partly changed the hardware. */
			make_config(before[i], &config);
			if (requests[i] < 0 || GPIO_IOCTL(requests[i], GPIO_V2_LINE_SET_CONFIG_IOCTL, &config)) {
				/* Do not leave a request in an unknown electrical state. */
				release_line(i);
				rollback_failed = true;
			} else active[i] = before[i];
		}
	}
	if (rollback_failed && strlen(error) < size)
		snprintf(error + strlen(error), size - strlen(error),
			 " Some previous settings could not be restored; those requests were released.");
	return -1;
}

static void append(const char *format, ...)
{
	va_list args;
	int length;
	if (reply_used >= sizeof(reply)) return;
	va_start(args, format);
	length = vsnprintf(reply + reply_used, sizeof(reply) - reply_used, format, args);
	va_end(args);
	if (length > 0) reply_used += (size_t)length < sizeof(reply) - reply_used ?
		(size_t)length : sizeof(reply) - reply_used - 1;
}

static void clean_label(char *label, size_t size)
{
	label[size - 1] = '\0';
	for (char *p = label; *p; p++)
		if ((unsigned char)*p < 32 || (unsigned char)*p > 126) *p = '?';
}

static int value_of(unsigned int pin)
{
	struct gpio_v2_line_values values = { .mask = 1 };
	/* GPIO output readback is its configured level, not an external pad sample. */
	if (requests[pin] >= 0 && (active[pin].mode == LOW || active[pin].mode == HIGH))
		return active[pin].mode == HIGH;
	if (requests[pin] < 0 || GPIO_IOCTL(requests[pin], GPIO_V2_LINE_GET_VALUES_IOCTL, &values))
		return -1;
	return !!(values.bits & 1);
}

static void list_lines(void)
{
	append("OK\n");
	for (unsigned int i = 0; i < PINS; i++) {
		struct gpio_v2_line_info info;
		const char *mode = mode_name(active[i].mode), *owner = "-";
		char value[16] = "-";
		if (!safe_line(i)) { mode = "reserved"; owner = "Board-reserved"; }
		else if (line_info(i, &info)) { mode = "unavailable"; owner = "Unavailable"; }
		else if (requests[i] >= 0) {
			int v = value_of(i);
			owner = CONSUMER;
			if (v >= 0) snprintf(value, sizeof(value), "%d", v);
		} else if (info.flags & GPIO_V2_LINE_FLAG_USED) {
			mode = "busy";
			clean_label(info.consumer, sizeof(info.consumer));
			owner = *info.consumer ? info.consumer : "Interface/driver";
		}
		/* No request is taken to inspect unowned inputs or peripheral pins. */
		append("%u\t%s\t%s\t%s\t%s\t%s\t%s\n", i, mode,
		       bias_name(active[i].bias), value, owner,
		       mode_name(saved[i].mode), bias_name(saved[i].bias));
	}
}

static int words(char *text, char **argv, int limit)
{
	char *saveptr, *token;
	int argc = 0;
	for (token = strtok_r(text, " \t\r\n", &saveptr); token;
	     token = strtok_r(NULL, " \t\r\n", &saveptr)) {
		if (argc == limit) return -1;
		argv[argc++] = token;
	}
	return argc;
}

static void dispatch(char *command)
{
	char *argv[5], error[512] = "";
	struct setting target[PINS];
	unsigned int pin;
	int argc = words(command, argv, 5);
	reply_used = 0;
	reply[0] = '\0';
	if (argc == 1 && !strcmp(argv[0], "ping")) { append("OK\n"); return; }
	if (argc == 1 && !strcmp(argv[0], "stop")) {
		stopping = 1;
		append("OK\n");
		return;
	}
	/* Imports may replace saved settings while live requests remain active. */
	if (argc > 0 && (!strcmp(argv[0], "set") || !strcmp(argv[0], "list") ||
			 !strcmp(argv[0], "status"))) {
		if (read_config(config_path, target, true, error, sizeof(error))) goto failure;
		memcpy(saved, target, sizeof(saved));
	}
	if (argc == 1 && (!strcmp(argv[0], "list") || !strcmp(argv[0], "status"))) {
		list_lines();
		return;
	}
	if (argc == 2 && !strcmp(argv[0], "read") && !parse_line(argv[1], &pin)) {
		int value;
		if (requests[pin] < 0) {
			append("ERR GPIO%u is not managed; configure it as an input first.\n", pin);
			return;
		}
		value = value_of(pin);
		if (value < 0) append("ERR Cannot read GPIO%u: %s.\n", pin, strerror(errno));
		else append("OK\n%d\n", value);
		return;
	}
	memcpy(target, saved, sizeof(target));
	if ((argc == 3 || argc == 4) && !strcmp(argv[0], "set") &&
	    !parse_line(argv[1], &pin) &&
	    !parse_setting(argv[2], argc == 4 ? argv[3] : NULL, &target[pin])) {
		struct setting live_target[PINS];
		/* An edit applies this pin only; other imported pins stay pending. */
		memcpy(live_target, active, sizeof(live_target));
		live_target[pin] = target[pin];
		if (apply_settings(live_target, target, true, error, sizeof(error))) goto failure;
	} else if (argc == 1 && !strcmp(argv[0], "reset")) {
		memset(target, 0, sizeof(target));
		if (apply_settings(target, NULL, true, error, sizeof(error))) goto failure;
	} else if (argc == 1 && !strcmp(argv[0], "apply")) {
		if (read_config(config_path, target, true, error, sizeof(error))) goto failure;
		/* Keep desired values visible if a boot/restore conflict prevents apply. */
		memcpy(saved, target, sizeof(saved));
		if (apply_settings(target, NULL, false, error, sizeof(error))) goto failure;
	} else {
		append("ERR Invalid GPIO command, reserved pin, mode or bias.\n");
		return;
	}
	append("OK\n");
	if (*error) append("%s\n", error);
	return;
failure:
	append("ERR %s\n", error);
}

static int paths_init(void)
{
	const char *conf = getenv("ESP32_CONFIG_DIR"), *run = getenv("ESP32_CONFIG_RUN_DIR");
	if (!conf || !*conf) conf = "/etc/esp32-conf";
	if (!run || !*run) run = "/run/esp32-config";
	if (snprintf(config_path, sizeof(config_path), "%s/gpio.conf", conf) >= (int)sizeof(config_path) ||
	    snprintf(run_path, sizeof(run_path), "%s", run) >= (int)sizeof(run_path) ||
	    snprintf(socket_path, sizeof(socket_path), "%s/gpio.sock", run) >= (int)sizeof(socket_path)) {
		fprintf(stderr, "GPIO configuration or runtime path is too long.\n");
		return -1;
	}
	return 0;
}

static int private_directory(const char *path)
{
	struct stat st;
	if (mkdir(path, 0700) && errno != EEXIST) return -1;
	if (lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 077)) {
		errno = EPERM;
		return -1;
	}
	return 0;
}

static void signal_stop(int signal_number)
{
	(void)signal_number;
	stopping = 1;
}

static int serve(void)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct sigaction action = { .sa_handler = signal_stop };
	char lock_path[1100], error[512] = "";
	int listener = -1, lock_fd = -1, result = 1;
	bool bound = false;
	/* The init launcher is a background child, so detach its terminal group. */
	if (setsid() < 0 && errno != EPERM) { perror("GPIO service session"); return 1; }
	signal(SIGHUP, SIG_IGN);
	if (private_directory(run_path)) { perror("GPIO runtime directory"); return 1; }
	if (snprintf(lock_path, sizeof(lock_path), "%s/gpio.lock", run_path) >= (int)sizeof(lock_path))
		return 1;
	lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB)) { perror("GPIO manager lock"); goto out; }
	if (open_chip()) { perror("ESP32-S31 GPIO controller"); goto out; }
	/* Validation precedes electrical changes; invalid stored records are rejected. */
	if (read_config(config_path, saved, true, error, sizeof(error))) {
		fprintf(stderr, "%s\n", error);
		goto out;
	}
	/* S46's boot action explicitly applies the saved set. On-demand startup
	 * only serves requests, so editing one pin cannot activate a pending import. */
	listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (listener < 0) { perror("GPIO socket"); goto out; }
	strcpy(address.sun_path, socket_path);
	unlink(socket_path);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address))) { perror("GPIO bind"); goto out; }
	bound = true;
	if (chmod(socket_path, 0600) || listen(listener, 4)) { perror("GPIO listen"); goto out; }
	sigemptyset(&action.sa_mask);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	signal(SIGPIPE, SIG_IGN);
	while (!stopping) {
		struct ucred peer;
		socklen_t peer_size = sizeof(peer);
		struct timeval timeout = { .tv_sec = 2 };
		char command[128];
		ssize_t length;
		int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
		if (client < 0) {
			if (errno == EINTR) continue;
			perror("GPIO accept");
			break;
		}
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
		if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) || peer.uid != 0) {
			close(client);
			continue;
		}
		length = recv(client, command, sizeof(command) - 1, MSG_TRUNC);
		if (length > 0 && length < (ssize_t)sizeof(command) && !memchr(command, '\0', length)) {
			command[length] = '\0';
			dispatch(command);
			(void)send(client, reply, reply_used, MSG_NOSIGNAL);
		}
		close(client);
	}
	result = 0;
out:
	for (unsigned int i = 0; i < PINS; i++) release_line(i);
	if (listener >= 0) close(listener);
	if (bound) unlink(socket_path);
	if (chip_fd >= 0) close(chip_fd);
	chip_fd = -1;
	if (lock_fd >= 0) close(lock_fd);
	return result;
}

static int client_command(const char *command, bool quiet)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct timeval timeout = { .tv_sec = 5 };
	ssize_t length;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	strcpy(address.sun_path, socket_path);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	if (connect(fd, (struct sockaddr *)&address, sizeof(address))) { close(fd); return -1; }
	if (send(fd, command, strlen(command), MSG_NOSIGNAL) < 0) { close(fd); return -1; }
	length = recv(fd, reply, sizeof(reply) - 1, MSG_TRUNC);
	close(fd);
	if (length <= 0 || length >= (ssize_t)sizeof(reply)) return -1;
	reply[length] = '\0';
	if (!strncmp(reply, "OK\n", 3)) {
		if (!quiet) fputs(reply + 3, stdout);
		return 0;
	}
	if (!quiet) fputs(!strncmp(reply, "ERR ", 4) ? reply + 4 : reply, stderr);
	return 1;
}

static void usage(void)
{
	puts("Usage: s31-gpio list|status|read LINE|apply|reset|ping|stop|serve\n"
	     "       s31-gpio set LINE {application|input [none|up|down]|low|high}\n"
	     "       s31-gpio check FILE\n"
	     "list fields: line, active mode, bias, value, owner, saved mode, saved bias (TSV).");
}

int main(int argc, char **argv)
{
	char command[128] = "", error[512] = "";
	struct setting checked[PINS];
	int result;
	for (unsigned int i = 0; i < PINS; i++) requests[i] = -1;
	if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "help"))) { usage(); return 0; }
	if (argc == 3 && !strcmp(argv[1], "check")) {
		if (read_config(argv[2], checked, false, error, sizeof(error))) {
			fprintf(stderr, "%s\n", error);
			return 1;
		}
		return 0;
	}
	if (argc < 2 || argc > 5) { usage(); return 1; }
	if (geteuid() != 0) { fprintf(stderr, "GPIO configuration requires root.\n"); return 1; }
	if (paths_init()) return 1;
	if (argc == 2 && !strcmp(argv[1], "serve")) return serve();
	for (int i = 1; i < argc; i++) {
		if (strpbrk(argv[i], " \t\r\n") || strlen(command) + strlen(argv[i]) + 2 > sizeof(command)) {
			fprintf(stderr, "Invalid GPIO command argument.\n");
			return 1;
		}
		if (i > 1) strcat(command, " ");
		strcat(command, argv[i]);
	}
	result = client_command(command, !strcmp(command, "ping"));
	if (result >= 0) return result;
	/* Read-only inspection works before a daemon is needed. */
	if (!strcmp(command, "list") || !strcmp(command, "status")) {
		if (open_chip()) { perror("ESP32-S31 GPIO controller"); return 1; }
		if (read_config(config_path, saved, true, error, sizeof(error))) {
			fprintf(stderr, "%s\n", error);
			return 1;
		}
		reply_used = 0;
		list_lines();
		fputs(reply + 3, stdout);
		return 0;
	}
	if (strcmp(command, "ping")) fprintf(stderr, "GPIO manager is unavailable. Start /etc/init.d/S46s31-gpio.\n");
	return 1;
}
