/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
#ifndef S31_AUDIO_SRAM_H
#define S31_AUDIO_SRAM_H

#include "s31_memory_layout.h"

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 s31_audio_u8;
typedef u32 s31_audio_u32;
#else
#include <stdint.h>
typedef uint8_t s31_audio_u8;
typedef uint32_t s31_audio_u32;
#endif

/* Uncached HP-SRAM control and compact cacheable PSRAM payload rings. */
#define S31_AUDIO_CONTROL_BASE          0x2F06B180U
#define S31_AUDIO_CONTROL_MAP_SIZE      0x00000400U
#define S31_AUDIO_SRAM_BASE             S31_AUDIO_PSRAM_BASE
#define S31_AUDIO_SRAM_SIZE             S31_AUDIO_PSRAM_SIZE
#define S31_AUDIO_CONTROL_SIZE          0x00001000U
#define S31_AUDIO_RING_BYTES            0x00004000U
#define S31_AUDIO_STREAM_COUNT          3U
#define S31_AUDIO_PLAYBACK_COUNT        2U
#define S31_AUDIO_LINUX_STREAM          0U
#define S31_AUDIO_FREERTOS_STREAM       1U
#define S31_AUDIO_CAPTURE_STREAM        2U

#define S31_AUDIO_RING0_OFFSET          0x00000000U
#define S31_AUDIO_RING1_OFFSET          0x00004000U
#define S31_AUDIO_RING2_OFFSET          0x00008000U
#define S31_AUDIO_RING_AREA_SIZE        \
	(S31_AUDIO_RING2_OFFSET + S31_AUDIO_RING_BYTES)

#define S31_AUDIO_MAGIC                 0x41313353U /* "S31A" */
#define S31_AUDIO_ABI_VERSION           3U
#define S31_AUDIO_HW_RATE               48000U
#define S31_AUDIO_HW_CHANNELS           2U
#define S31_AUDIO_SAMPLE_BYTES          2U

enum s31_audio_owner {
	S31_AUDIO_OWNER_NONE = 0,
	S31_AUDIO_OWNER_LINUX = 1,
	S31_AUDIO_OWNER_FREERTOS = 2,
};

enum s31_audio_stream_state {
	S31_AUDIO_STATE_CLOSED = 0,
	S31_AUDIO_STATE_PREPARED = 1,
	S31_AUDIO_STATE_RUNNING = 2,
	S31_AUDIO_STATE_XRUN = 3,
};

enum s31_audio_format {
	S31_AUDIO_FORMAT_U16_LE = 1,
};

/* Producer and consumer fields have separate cache-line ownership. */
struct s31_audio_ring_control {
	volatile s31_audio_u32 producer;
	s31_audio_u8 producer_pad[60];
	volatile s31_audio_u32 consumer;
	s31_audio_u8 consumer_pad[60];

	volatile s31_audio_u32 owner;
	volatile s31_audio_u32 state;
	volatile s31_audio_u32 rate;
	volatile s31_audio_u32 channels;
	volatile s31_audio_u32 format;
	volatile s31_audio_u32 period_bytes;
	volatile s31_audio_u32 generation;
	s31_audio_u8 config_pad[36];

	volatile s31_audio_u32 xruns;
	volatile s31_audio_u32 transferred_bytes;
	volatile s31_audio_u32 wakeups;
	volatile s31_audio_u32 reserved;
	s31_audio_u8 stats_pad[48];
} __attribute__((aligned(64)));

struct s31_audio_control {
	volatile s31_audio_u32 magic;
	volatile s31_audio_u32 abi_version;
	volatile s31_audio_u32 generation;
	volatile s31_audio_u32 ready;
	volatile s31_audio_u32 hardware_rate;
	volatile s31_audio_u32 hardware_channels;
	volatile s31_audio_u32 ring_bytes;
	volatile s31_audio_u32 feature_bits;
	s31_audio_u8 header_pad[32];
	struct s31_audio_ring_control stream[S31_AUDIO_STREAM_COUNT];
};

#define S31_AUDIO_FEAT_MIXER            (1U << 0)
#define S31_AUDIO_FEAT_ASRC             (1U << 1)
#define S31_AUDIO_FEAT_PLUGINS          (1U << 2)
#define S31_AUDIO_FEAT_DUCKING          (1U << 3)
#define S31_AUDIO_FEAT_AUTOMATION       (1U << 4)
#define S31_AUDIO_FEAT_OUTPUT_SWITCH    (1U << 5)
#define S31_AUDIO_FEAT_I2S_DMA          (1U << 6)

static inline s31_audio_u32 s31_audio_ring_offset(unsigned int stream)
{
	static const s31_audio_u32 offsets[S31_AUDIO_STREAM_COUNT] = {
		S31_AUDIO_RING0_OFFSET, S31_AUDIO_RING1_OFFSET,
		S31_AUDIO_RING2_OFFSET,
	};

	return offsets[stream];
}

_Static_assert(sizeof(struct s31_audio_ring_control) == 256,
	       "audio ring control ABI changed");
_Static_assert(sizeof(struct s31_audio_control) <= S31_AUDIO_CONTROL_SIZE,
	       "audio control area overflow");
_Static_assert(sizeof(struct s31_audio_control) <= S31_AUDIO_CONTROL_MAP_SIZE,
	       "audio HP-SRAM control area overflow");
_Static_assert(S31_AUDIO_RING2_OFFSET + S31_AUDIO_RING_BYTES <=
	       S31_AUDIO_SRAM_SIZE, "audio ring area overflow");

#endif /* S31_AUDIO_SRAM_H */
