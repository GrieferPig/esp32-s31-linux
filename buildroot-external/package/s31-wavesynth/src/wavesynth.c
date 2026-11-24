/* SPDX-License-Identifier: BSD-2-Clause */
#include <alsa/asoundlib.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
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
		    "play HZ [MS] | midi [PATH]\n"
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

static void init_voice(Voice *voice, const i16 *table, const struct patch *patch)
{
	voice_init(voice, table, Hz(SAMPLE_RATE));
	voice_set_envelope(voice, Ms(patch->attack), Ms(patch->decay),
			   Percent(patch->sustain), Ms(patch->release));
	voice_set_filter(voice, Hz(patch->cutoff), Percent(patch->resonance));
}

static int write_frames(snd_pcm_t *pcm, const i16 *stereo, unsigned frames)
{
	for (unsigned sent = 0; sent < frames; ) {
		snd_pcm_sframes_t n = snd_pcm_writei(pcm, stereo + 2 * sent, frames - sent);

		if (n <= 0)
			return n ? (int)n : -EIO;
		sent += (unsigned)n;
	}
	return 0;
}

static int open_pcm(snd_pcm_t **pcm, const char *device, unsigned latency)
{
	int ret = snd_pcm_open(pcm, device, SND_PCM_STREAM_PLAYBACK, 0);

	if (ret < 0)
		return ret;

	/* S31 TRM ch.27 I2S Controller — I2S_TX_START. The ALSA
	 * driver owns the I2S clock and TX DMA for the ES8311.
	 */
	ret = snd_pcm_set_params(*pcm, SND_PCM_FORMAT_S16_LE,
				 SND_PCM_ACCESS_RW_INTERLEAVED, 2, SAMPLE_RATE, 0, latency);
	if (ret < 0)
		snd_pcm_close(*pcm);
	return ret;
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
		int ret = write_frames(pcm, stereo, count);
		if (ret < 0)
			return ret;
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
	init_voice(&voice, table, patch);

	ret = open_pcm(&pcm, device, 80000);
	if (ret < 0)
		return ret;
	voice_note_on(&voice, Hz(frequency));
	ret = write_note(pcm, &voice, duration * (SAMPLE_RATE / 1000));
	if (ret >= 0) {
		voice_note_off(&voice);
		ret = write_note(pcm, &voice, patch->release * (SAMPLE_RATE / 1000));
	}
	if (ret >= 0)
		ret = snd_pcm_drain(pcm);
	close_ret = snd_pcm_close(pcm);
	return ret < 0 ? ret : close_ret;
}

enum { MIDI_CHANNEL_COUNT = 16, MIDI_NOTES = 128, MIDI_VOICES = 4,
       MIDI_TABLES = (MIDI_NOTES + 11) / 12, MIDI_BLOCK_FRAMES = 128 };

struct midi_voice {
	Voice synth;
	unsigned channel, note;
	i32 gain;
	u64 started;
	bool held, sustained;
};

struct midi_session {
	struct midi_voice voices[MIDI_VOICES];
	i16 tables[MIDI_TABLES][WAVETABLE_SIZE];
	u32 increments[MIDI_NOTES];
	bool sustain[MIDI_CHANNEL_COUNT];
	unsigned status, first_data;
	bool pending_data;
	u64 note_ons, note_offs;
	unsigned peak_held;
};

static volatile sig_atomic_t midi_stop;

static void stop_midi(int signal)
{
	(void)signal;
	midi_stop = 1;
}

static void midi_note_on(struct midi_session *midi, const struct patch *patch,
			 unsigned channel, unsigned note, unsigned velocity)
{
	struct midi_voice *slot = &midi->voices[0];
	unsigned held = 0;

	for (unsigned i = 0; i < MIDI_VOICES; ++i) {
		struct midi_voice *voice = &midi->voices[i];

		if (voice->held && voice->channel == channel && voice->note == note) {
			slot = voice;
			break;
		}
		if (voice->synth.env.state == ENV_IDLE ||
		    (slot->synth.env.state != ENV_IDLE && voice->started < slot->started))
			slot = voice;
	}
	init_voice(&slot->synth, midi->tables[note / 12], patch);
	slot->synth.osc.increment = midi->increments[note];
	env_note_on(&slot->synth.env);
	slot->channel = channel;
	slot->note = note;
	slot->gain = (i32)(velocity * FIXED_ONE_Q16 / 127);
	slot->started = ++midi->note_ons;
	slot->held = true;
	slot->sustained = false;

	for (unsigned i = 0; i < MIDI_VOICES; ++i)
		held += midi->voices[i].held;
	if (held > midi->peak_held)
		midi->peak_held = held;
}

static void release_midi_voice(struct midi_voice *voice, bool sustain)
{
	voice->held = false;
	voice->sustained = sustain;
	if (!sustain)
		voice_note_off(&voice->synth);
}

static void midi_event(struct midi_session *midi, const struct patch *patch,
		       unsigned type, unsigned channel, unsigned first, unsigned second)
{
	if (type == 9 && second) {
		midi_note_on(midi, patch, channel, first, second);
		return;
	}
	if (type == 8 || type == 9) {
		++midi->note_offs;
		for (unsigned i = 0; i < MIDI_VOICES; ++i) {
			struct midi_voice *voice = &midi->voices[i];

			if (voice->held && voice->channel == channel && voice->note == first)
				release_midi_voice(voice, midi->sustain[channel]);
		}
		return;
	}
	if (type != 11)
		return;

	if (first == 64)
		midi->sustain[channel] = second >= 64;
	for (unsigned i = 0; i < MIDI_VOICES; ++i) {
		struct midi_voice *voice = &midi->voices[i];

		if (voice->channel != channel || voice->synth.env.state == ENV_IDLE)
			continue;
		if (first == 120) {
			voice->held = voice->sustained = false;
			voice->synth.env.state = ENV_IDLE;
		} else if (first == 123 && voice->held) {
			release_midi_voice(voice, midi->sustain[channel]);
		} else if (first == 64 && !midi->sustain[channel] && voice->sustained) {
			release_midi_voice(voice, false);
		}
	}
}

static void midi_byte(struct midi_session *midi, const struct patch *patch, unsigned byte)
{
	unsigned type;

	/* Realtime bytes may be interleaved with channel messages. */
	if (byte >= 0xf8)
		return;
	if (byte & 0x80) {
		midi->status = byte < 0xf0 ? byte : 0;
		midi->pending_data = false;
		return;
	}
	type = midi->status >> 4;
	if (!type || type == 12 || type == 13)
		return;
	if (!midi->pending_data) {
		midi->first_data = byte;
		midi->pending_data = true;
		return;
	}
	midi_event(midi, patch, type, midi->status & 15, midi->first_data, byte);
	midi->pending_data = false;
}

static i16 midi_output(i64 sum)
{
	const i32 knee = 24576, headroom = 32767 - knee;
	i64 level = sum < 0 ? -sum : sum;

	/* Keep single notes at their velocity-scaled level. Compress only the
	 * peaks of louder sums so chords can share the 16-bit output range.
	 */
	if (level > knee) {
		i64 excess = level - knee;

		level = knee + excess * headroom / (excess + headroom);
	}
	return sum < 0 ? -level : level;
}

static int stream_midi(snd_pcm_t *pcm, int input, struct midi_session *midi,
		       const struct patch *patch)
{
	i16 stereo[2 * MIDI_BLOCK_FRAMES];
	unsigned char data[64];
	struct pollfd midi_poll = { .fd = input, .events = POLLIN };

	while (!midi_stop) {
		int ready = poll(&midi_poll, 1, 0);

		if (ready < 0)
			return -errno;
		if (midi_poll.revents & (POLLERR | POLLHUP | POLLNVAL))
			return -ENODEV;
		if (midi_poll.revents & POLLIN) {
			ssize_t n = read(input, data, sizeof(data));

			if (!n)
				return -ENODEV;
			if (n < 0 && errno != EAGAIN)
				return -errno;
			for (ssize_t i = 0; i < n; ++i)
				midi_byte(midi, patch, data[i]);
		}

		for (unsigned i = 0; i < MIDI_BLOCK_FRAMES; ++i) {
			i64 sum = 0;

			for (unsigned v = 0; v < MIDI_VOICES; ++v) {
				struct midi_voice *voice = &midi->voices[v];

				if (voice->synth.env.state != ENV_IDLE)
					sum += fixed_mul(voice_process(&voice->synth), voice->gain);
			}
			i16 sample = midi_output(sum);

			stereo[2 * i] = sample;
			stereo[2 * i + 1] = sample;
		}
		int ret = write_frames(pcm, stereo, MIDI_BLOCK_FRAMES);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int play_midi(FILE *screen, const char *output, const struct patch *patch,
		     const char *input)
{
	struct midi_session midi = { 0 };
	struct sigaction action = { .sa_handler = stop_midi }, previous;
	snd_pcm_t *pcm;
	glob_t paths = { 0 };
	int fd, ret;

	if (!input) {
		ret = glob("/dev/snd/midiC*D*", 0, NULL, &paths);
		if (ret || paths.gl_pathc != 1) {
			if (ret == GLOB_NOMATCH)
				say(screen, "Plug in a USB MIDI controller.\n");
			else if (ret)
				say(screen, "Could not list MIDI inputs.\n");
			else
				say(screen, "Multiple devices. Use midi PATH.\n");
			globfree(&paths);
			return -1;
		}
		input = paths.gl_pathv[0];
	}

	fd = open(input, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		say(screen, "MIDI input %s\n", strerror(errno));
		globfree(&paths);
		return -1;
	}
	say(screen, "MIDI %s (Ctrl-C to stop)\n", input);
	globfree(&paths);

	for (unsigned n = 0; n < MIDI_NOTES; ++n) {
		double hz = 440.0 * pow(2.0, ((int)n - 69) / 12.0);

		/* Preserve fractional Hz when converting to the oscillator step. */
		midi.increments[n] = (u32)(hz * (4294967296.0 / SAMPLE_RATE) + 0.5);
		if (n % 12 == 11 || n == MIDI_NOTES - 1)
			build_wave(midi.tables[n / 12], patch->wave, (unsigned)ceil(hz));
	}

	midi_stop = 0;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, &previous) < 0) {
		say(screen, "MIDI signal setup %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	ret = open_pcm(&pcm, output, 20000);
	if (ret >= 0) {
		ret = stream_midi(pcm, fd, &midi, patch);
		snd_pcm_drop(pcm);
		snd_pcm_close(pcm);
	}
	sigaction(SIGINT, &previous, NULL);
	close(fd);

	say(screen, "MIDI on %llu off %llu peak held %u\n",
	    midi.note_ons, midi.note_offs, midi.peak_held);
	if (ret == -ENODEV) {
		say(screen, "MIDI input disconnected.\n");
		return -1;
	}
	if (ret < 0 && !(midi_stop && ret == -EINTR)) {
		say(screen, "MIDI stopped: %s\n", snd_strerror(ret));
		return -1;
	}
	return 0;
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
	} else if (!strcmp(name, "midi")) {
		if (count > 2) {
			say(screen, "Use midi [PATH]\n");
			return -1;
		}
		return play_midi(screen, device, patch, count == 2 ? args[1] : NULL);
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
