/* SPDX-License-Identifier: BSD-2-Clause */
#include <alsa/asoundlib.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mlws.h"

enum { SAMPLE_RATE = 48000, BLOCK_FRAMES = 256, HARMONICS = 24 };
enum waveform { SINE, SAW, SQUARE, TRIANGLE };

static const char *const wave_names[] = { "sine", "saw", "square", "triangle" };

struct patch {
	enum waveform wave;
	unsigned attack, decay, sustain, release;
	unsigned cutoff, resonance;
};

static void say(FILE *screen, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	fflush(stdout);

	va_start(args, format);
	vfprintf(screen, format, args);
	va_end(args);
}

static void show_help(FILE *screen)
{
	say(screen, "help | status | quit\n"
		    "wave sine|saw|square|triangle\n"
		    "play HZ [MS]\n"
		    "env ATTACK DECAY SUSTAIN RELEASE\n"
		    "filter HZ RESONANCE\n");
}

static void show_patch(FILE *screen, const struct patch *patch)
{
	say(screen, "%s  cutoff %u Hz  resonance %u%%\n"
		    "A %u  D %u  S %u%%  R %u (ms)\n",
	    wave_names[patch->wave], patch->cutoff, patch->resonance,
	    patch->attack, patch->decay, patch->sustain, patch->release);
}

static void build_wave(i16 *table, enum waveform wave, unsigned frequency)
{
	i32 weights[HARMONICS] = { FIXED_ONE_Q16 };
	u32 phases[HARMONICS] = { 0 };
	unsigned count = (SAMPLE_RATE / 2 - 1) / frequency;

	if (count > HARMONICS)
		count = HARMONICS;
	if (wave == SINE)
		count = 1;

	for (unsigned h = 2; h <= count; ++h) {
		if (wave == SAW || (h & 1))
			weights[h - 1] = FIXED_ONE_Q16 / h;
		if (wave == TRIANGLE && (h & 1)) {
			weights[h - 1] /= h;
			if (h % 4 == 3)
				phases[h - 1] = 0x80000000u;
		}
	}
	osc_build_wavetable(table, weights, phases, count);
}

static int write_note(snd_pcm_t *pcm, Voice *voice, unsigned frames)
{
	i16 stereo[2 * BLOCK_FRAMES];
	VoiceMixer mixer;

	voice_mixer_init(&mixer, 1);
	while (frames) {
		unsigned count = frames < BLOCK_FRAMES ? frames : BLOCK_FRAMES;

		for (unsigned i = 0; i < count; ++i) {
			i16 sample = voice_mixer_process(&mixer, voice, 1);

			stereo[2 * i] = sample;
			stereo[2 * i + 1] = sample;
		}
		for (unsigned sent = 0; sent < count; ) {
			snd_pcm_sframes_t n = snd_pcm_writei(pcm, stereo + 2 * sent,
							    count - sent);

			if (n <= 0)
				return n ? (int)n : -EIO;
			sent += (unsigned)n;
		}
		frames -= count;
	}
	return 0;
}

static int play(const char *device, const struct patch *patch,
		unsigned frequency, unsigned duration)
{
	i16 table[WAVETABLE_SIZE];
	snd_pcm_t *pcm;
	Voice voice;
	int ret, close_ret;

	build_wave(table, patch->wave, frequency);
	voice_init(&voice, table, Hz(SAMPLE_RATE));
	voice_set_envelope(&voice, Ms(patch->attack), Ms(patch->decay),
			   Percent(patch->sustain), Ms(patch->release));
	voice_set_filter(&voice, Hz(patch->cutoff), Percent(patch->resonance));

	ret = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
	if (ret < 0)
		return ret;

	/* S31 TRM ch.27 I2S Controller — I2S_TX_START. The CoreBoard ALSA
	 * driver owns the I2S clock and TX DMA for the ES8311.
	 */
	ret = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
				 SND_PCM_ACCESS_RW_INTERLEAVED, 2, SAMPLE_RATE, 0, 80000);
	if (ret >= 0) {
		voice_note_on(&voice, Hz(frequency));
		ret = write_note(pcm, &voice, duration * (SAMPLE_RATE / 1000));
	}
	if (ret >= 0) {
		voice_note_off(&voice);
		ret = write_note(pcm, &voice, patch->release * (SAMPLE_RATE / 1000));
	}
	if (ret >= 0)
		ret = snd_pcm_drain(pcm);
	close_ret = snd_pcm_close(pcm);
	return ret < 0 ? ret : close_ret;
}

static int init_audio(void)
{
	snd_ctl_elem_value_t *volume;
	snd_ctl_t *card;
	int ret;

	ret = snd_ctl_open(&card, "hw:CARD=ES8311", 0);
	if (ret < 0)
		return ret;

	snd_ctl_elem_value_alloca(&volume);
	snd_ctl_elem_value_set_interface(volume, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_value_set_name(volume, "DAC Playback Volume");
	ret = snd_ctl_elem_read(card, volume);
	if (ret >= 0 && snd_ctl_elem_value_get_integer(volume, 0) == 0) {
		/* ES8311 DAC_REG32 starts at -95.5 dB. Use 0 dB (0xbf)
		 * as in Espressif's ES8311 reference driver.
		 */
		snd_ctl_elem_value_set_integer(volume, 0, 0xbf);
		ret = snd_ctl_elem_write(card, volume);
	}
	snd_ctl_close(card);
	return ret;
}

static bool number(const char *arg, unsigned min, unsigned max, unsigned *value)
{
	char *end;
	unsigned long parsed;

	if (!isdigit((unsigned char)arg[0]))
		return false;
	errno = 0;
	parsed = strtoul(arg, &end, 10);
	if (errno || *end || parsed < min || parsed > max)
		return false;
	*value = (unsigned)parsed;
	return true;
}

static int command(FILE *screen, const char *device, struct patch *patch,
		   unsigned count, char *const args[])
{
	const char *name = args[0];
	unsigned freq, ms = 500, a, d, s, r;
	int ret;

	if (count == 1 && !strcmp(name, "help")) {
		show_help(screen);
	} else if (count == 1 && !strcmp(name, "status")) {
		show_patch(screen, patch);
	} else if (!strcmp(name, "wave")) {
		if (count == 2) {
			for (unsigned i = SINE; i <= TRIANGLE; ++i) {
				if (!strcmp(args[1], wave_names[i])) {
					patch->wave = i;
					show_patch(screen, patch);
					return 0;
				}
			}
		}
		say(screen, "Use wave sine|saw|square|triangle\n");
		return -1;
	} else if (!strcmp(name, "env")) {
		if (count != 5 || !number(args[1], 0, 2000, &a) ||
		    !number(args[2], 0, 2000, &d) || !number(args[3], 0, 100, &s) ||
		    !number(args[4], 0, 2000, &r)) {
			say(screen, "A/D/R 0..2000 ms, S 0..100%%\n");
			return -1;
		}
		patch->attack = a;
		patch->decay = d;
		patch->sustain = s;
		patch->release = r;
		show_patch(screen, patch);
	} else if (!strcmp(name, "filter")) {
		if (count != 3 || !number(args[1], 50, 6000, &freq) ||
		    !number(args[2], 0, 100, &r)) {
			say(screen, "50..6000 Hz, resonance 0..100%%\n");
			return -1;
		}
		patch->cutoff = freq;
		patch->resonance = r;
		show_patch(screen, patch);
	} else if (!strcmp(name, "play")) {
		if (count < 2 || count > 3 || !number(args[1], 20, 8000, &freq) ||
		    (count == 3 && !number(args[2], 20, 10000, &ms))) {
			say(screen, "Play 20..8000 Hz, 20..10000 ms\n");
			return -1;
		}
		say(screen, "Playing %s %u Hz for %u ms\n", wave_names[patch->wave], freq, ms);
		ret = play(device, patch, freq, ms);
		if (ret < 0) {
			say(screen, "PCM %s\n", snd_strerror(ret));
			return -1;
		}
		say(screen, "Done\n");
	} else {
		say(screen, "Unknown command. Type help.\n");
		return -1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	const char *device = "hw:CARD=ES8311,DEV=0";
	bool banner = argc == 2 && !strcmp(argv[1], "--banner");
	struct patch patch = { SINE, 10, 80, 75, 100, 4500, 15 };
	char line[128];
	int status = EXIT_SUCCESS;
	FILE *screen;

	if (argc == 2 && !strcmp(argv[1], "--init-audio")) {
		int ret = init_audio();

		if (ret < 0) {
			fprintf(stderr, "Initializing ES8311 DAC volume: %s\n", snd_strerror(ret));
			return EXIT_FAILURE;
		}
		return EXIT_SUCCESS;
	}
	if (argc == 3 && !strcmp(argv[1], "--pcm")) {
		device = argv[2];
	} else if (argc != 1 && !banner) {
		fprintf(stderr, "Usage: %s [--pcm DEVICE | --banner | --init-audio]\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (access("/dev/fb0", F_OK) < 0) {
		perror("/dev/fb0");
		return EXIT_FAILURE;
	}
	screen = fopen("/dev/tty1", "w");
	if (!screen) {
		perror("/dev/tty1");
		return EXIT_FAILURE;
	}
	setvbuf(screen, NULL, _IONBF, 0);
	fputs("\033[2J\033[H\033[0m", screen);
	say(screen, "My Little Wavesynth\nESP32-S31 / ILI9341\n\n");
	if (banner)
		say(screen, "Run s31-wavesynth on the UART.\n\n");
	show_help(screen);
	if (!banner)
		show_patch(screen, &patch);

	while (!banner) {
		char *args[5], *cursor;
		unsigned count = 0;

		say(screen, "mlws> ");
		if (!fgets(line, sizeof(line), stdin))
			break;
		if (!strchr(line, '\n') && !feof(stdin)) {
			int ch;

			while ((ch = getchar()) != '\n' && ch != EOF)
				;
			say(screen, "Command too long\n");
			status = EXIT_FAILURE;
			continue;
		}
		line[strcspn(line, "\r\n")] = '\0';
		for (char *p = line; *p; ++p)
			if (!isprint((unsigned char)*p) && *p != '\t')
				*p = '?';
		fprintf(screen, "%s\n", line);
		for (char *word = strtok_r(line, " \t", &cursor); word;
		     word = strtok_r(NULL, " \t", &cursor)) {
			if (count == sizeof(args) / sizeof(*args)) {
				++count;
				break;
			}
			args[count++] = word;
		}
		if (!count)
			continue;
		if (count == 1 && !strcmp(args[0], "quit")) {
			say(screen, "Exited\n");
			break;
		}
		if (count > sizeof(args) / sizeof(*args)) {
			say(screen, "Too many arguments\n");
			status = EXIT_FAILURE;
		} else if (command(screen, device, &patch, count, args) < 0) {
			status = EXIT_FAILURE;
		}
	}
	if (ferror(stdin)) {
		perror("stdin");
		status = EXIT_FAILURE;
	}
	if (ferror(screen)) {
		fprintf(stderr, "Writing /dev/tty1 failed\n");
		status = EXIT_FAILURE;
	}
	if (fclose(screen) == EOF) {
		perror("/dev/tty1");
		status = EXIT_FAILURE;
	}
	return status;
}
