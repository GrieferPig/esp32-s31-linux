# ESP32-S31 FreeRTOS audio subsystem

## Ownership and data path

Hart0/FreeRTOS exclusively owns I2S0, I2C0, the ES8311 codec, both S31 ASRC
lanes, mixing, and the physical output clock. Hart1/Linux never accesses an
audio peripheral. Its complete data path is:

```text
ALSA application <-> ALSA PCM copy callback <-> dedicated PSRAM SPSC ring
                                               <-> FreeRTOS audio task
```

The shared ABI is `shared/s31_audio_sram.h`. Its control structure occupies an
uncached HP-SRAM window at `0x2f06b180..0x2f06b580`. Three 16 KiB payload rings
occupy `0x50fe0000..0x50fec000` in the final audio-reserved 64 KiB PSRAM page.
The remaining 16 KiB is reserved for expansion. OpenSBI RW occupies the final
PSRAM page at `0x50ff0000..0x51000000`.

| Stream | Direction from Linux | ALSA PCM | Producer | Consumer |
| --- | --- | --- | --- | --- |
| 0 | playback | `linux-playback` | Linux | audio mixer input 0 |
| 1 | not exposed | none | FreeRTOS | audio mixer input 1 |
| 2 | capture | `capture` | I2S input task | Linux |

Every ring is SPSC. Producer and consumer counters are monotonic 32-bit byte
counters on separate 64-byte cache lines. Counter subtraction is wrap-safe;
a distance larger than the ring is corruption and causes an XRUN recovery.
The mixer topology and ownership are fixed: Linux can produce only input 0 and
`s31_audio_freertos_write()` can produce only input 1. Applications must not
bypass those APIs and rewrite ownership.

Linux registers one ALSA card with one playback and one capture PCM. Playback supports
interleaved mono or stereo U16_LE PCM at 8, 16, 24, 32, 44.1, and 48 kHz.
Capture supports mono or duplicated stereo U16_LE at the hardware rate of
48 kHz and defaults to mono in the shared configuration. FreeRTOS rings the
dedicated `CPU_INT_FROM_CPU_1` doorbell after advancing a capture producer or
playback consumer across an ALSA period boundary. Linux CLIC ID 41 delivers the
ALSA pointer notification and wakes capture readers; there are no audio or
capture polling timers. The managed runtime buffer is allocated to the exact
negotiated ALSA buffer size, reused as the copy bounce buffer, and released by
`hw_free`. Linux does not resample, mix, or control the codec.

The driver's `audio_stats` sysfs attribute reports each ABI stream index,
hardware-transferred byte count, and XRUN count. For the generic board it is
available at
`/sys/bus/platform/devices/2f06b180.audio/audio_stats`.

## Real-time graph

The hardware domain is fixed at 48 kHz, stereo, U16_LE with 256-frame blocks
(5.33 ms). Keeping one clock domain prevents Linux stream setup from changing
I2S timing underneath another producer.

Linux trigger transitions update the shared stream state and ring the hart0
doorbell. A local FreeRTOS write directly notifies the task, and a 10 ms idle
poll covers a lost notification. With no running Linux stream and no pending
FreeRTOS playback, the task blocks and submits no I2S transfers. I2S channels
remain enabled because the S31 duplex driver cannot reliably disable and
re-enable RX after codec setup.

For each requested block the high-priority FreeRTOS audio task:

1. Reads one 256-frame ES8311/I2S capture block and publishes it to ring 2.
2. Pulls one block from the fixed Linux and FreeRTOS mixer inputs.
3. Converts non-48-kHz streams with the S31 ASRC peripheral.
4. Runs registered post-processing plugins on playback stream 0 only.
5. Evaluates stream activity and smoothly updates ducking envelopes.
6. Applies queued per-stream volume automation and converts mono sources to
   stereo while accumulating centered 32-bit samples.
7. Saturates, returns samples to U16, and writes the stereo output backend.

Capture requires TX-generated BCLK and WS because I2S0 is the bus master. A
capture-only request therefore submits one U16 digital-silence TX block for
each RX block; this is clock generation, not an anti-pop fade or delayed stop.

I2S RX and TX use four AHB-GDMA descriptors of 256 frames each, exactly matching
the mixer block. ESP-IDF's DMA EOF queues drive `i2s_channel_read/write`; both
directions use a 20 ms bounded wait. The RX EOF wait paces every active mixer
block, including playback-only operation; TX writes may complete when a DMA
descriptor is merely queued and therefore are not a hardware-rate clock.
ASRC uses two additional AHB-GDMA pairs.
PSRAM ring copies remain CPU copies: an AHB-GDMA mem2mem hardware trial stalled
after 15 capture blocks because Linux DMA ownership and cross-hart PSRAM cache
maintenance are not coordinated, so that unsafe path is deliberately disabled.

The S31 exposes two ASRC streams, one for each mixer input, so both may use
non-native rates simultaneously. Native 48-kHz streams bypass ASRC. The ASRC
implementation uses AHB GDMA and waits on
the receive-EOF event; the mixer never performs software sample interpolation.

## FreeRTOS APIs

`bootloader/main/s31_audio.h` is the firmware API.

- `s31_audio_freertos_write()` writes only fixed mixer input 1 and declares
  whether its U16 samples are mono or stereo.
- `s31_audio_stream_set_priority()` assigns ducking priority. A numerically
  higher active priority ducks lower-priority streams.
- `s31_audio_stream_set_ducking()` sets attenuation plus attack/release times
  in frames. Defaults are -12 dB, 20 ms attack, and 250 ms release.
- `s31_audio_stream_automate()` queues linear Q16.16 gain segments. Chaining
  segments provides sample-smooth fades and arbitrary volume envelopes.
- `s31_audio_plugin_register()` adds an in-place U16 processor to playback
  stream 0. Plugins run after ASRC and before activity detection, automation,
  gain, and ducking. They run in the real-time audio task and must not block,
  allocate, or call non-real-time services.
- `s31_audio_output_register()` and `s31_audio_output_select()` provide the
  reserved output switch. The built-in backend is `es8311`; a future Bluetooth
  backend implements the same fixed-domain `start/write/stop` contract. Every
  backend always receives stereo U16 at 48 kHz; source channel layouts never
  leak into the output-switch API.

All source control belongs to FreeRTOS. ALSA mixer controls are deliberately
not used because that would duplicate policy and make Linux more than a PCM
pump.

## Codec and board configuration

The ES8311 is an I2S clock consumer and is configured over I2C with the
Espressif `esp_codec_dev` component. `bootloader/main/Kconfig.projbuild`
contains every board signal. Current checked defaults are:

```text
I2C0 SDA/SCL: 51/50
I2S0 MCLK/BCLK/WS/DOUT/DIN: 52/53/55/56/54
PA enable: 57 (active high)
Ambient mic I2S slot: left
```

These defaults follow Espressif's `esp-board-manager` definition for
`esp32_s31_function_coreboard_1`. Pin wiring is intentionally not represented
in the Linux device tree because Linux does not own those pins. Espressif's
Korvo-1 V1.1 is a different board with an ES8389 codec.

Failure to initialize the codec is logged and leaves the audio ABI offline, but
does not prevent hart1 from booting Linux. On failure, firmware scans the I2C
bus once and reports any responding 7-bit addresses to distinguish an address
mismatch from wiring or power failure.

The ES8311 contains one ADC path, so mono capture publishes that microphone
slot directly and stereo capture duplicates it to left and right. Set
`CONFIG_S31_AUDIO_MIC_RIGHT_SLOT` if the board revision routes ADC data to the
right I2S slot.

## Ambient microphone test

The root filesystem includes `s31-audio-mic-test`. On hardware it records two
seconds from `hw:0,1` as mono U16_LE and verifies that at least one second of
non-constant samples was delivered. This covers the ES8311 ADC, I2S RX,
FreeRTOS capture conversion, shared ring, ALSA driver, and userspace copy path:

```text
s31-audio-mic-test
```

The command reports sample count, minimum, maximum, span, centered mean, and
RMS. Speak or tap near the board while it runs. A build can verify the tool and
driver but cannot establish that the physical microphone is electrically
working; that final result requires running this command on the board.

For live Linux capture-to-playback testing, run `s31-audio-loopback`. It uses
4096-frame ALSA buffers with 256-frame periods and runs until Ctrl-C. An
optional duration in seconds makes the test terminate automatically:

```text
s31-audio-loopback 10
```

While running, the command reports the received sample count, minimum, maximum,
span, centered mean, and RMS once per second. Statistics are written to stderr
so the U16_LE stream passed from capture to playback remains unchanged.

Keep the speaker volume low and move it away from the microphone to avoid
acoustic feedback.

## Hardware validation

The compact layout and interrupt-driven driver were validated on the generic
board with simultaneous 48-kHz mono capture and playback for 10 seconds. The
capture delivered 480,000 samples, all three ABI streams reported zero XRUNs,
and Linux CLIC ID 41 handled 2,458 audio doorbell interrupts. The recorded data
spanned 8,515 sample values with an RMS of 101, confirming a live microphone
rather than a constant buffer.

## Failure behavior

- Playback starvation produces silence without consuming a partial ASRC block.
- Capture overflow drops the new block and increments `xruns`.
- Invalid ring distance resets the consumer to the producer and increments
  `xruns`.
- Unsupported rates are rejected by ALSA and the FreeRTOS ASRC API.
- Codec, I2S, ASRC, or audio task startup failure stops the loader before Linux
  boots, avoiding ALSA devices that can never make progress.
