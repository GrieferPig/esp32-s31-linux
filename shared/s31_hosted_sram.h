/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
/*
 * ESP32-S31 in-package ESP-Hosted transport.
 *
 * Both harts access this layout at the same HP SRAM physical address.  Linux
 * maps it as device memory.  ESP-IDF accesses can remain in the shared L1
 * D-cache, so hart0 maintains lines when exchanging ownership with Linux.
 * Producer and consumer words remain 64-byte separated so each side owns a
 * distinct synchronization line and the ABI stays safe if a cached alias is
 * introduced later.
 */
#ifndef S31_HOSTED_SRAM_H
#define S31_HOSTED_SRAM_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 s31_u8;
typedef u16 s31_u16;
typedef u32 s31_u32;
#else
#include <stdint.h>
typedef uint8_t s31_u8;
typedef uint16_t s31_u16;
typedef uint32_t s31_u32;
#endif

#define S31_HOSTED_SRAM_BASE		0x2f062f80U
#define S31_HOSTED_SRAM_SIZE		0x00010000U
#define S31_HOSTED_CTRL_SIZE		0x00001000U
#define S31_HOSTED_SLOT_SIZE		1920U
#define S31_HOSTED_SLOT_COUNT		16U
#define S31_HOSTED_SLOT_DATA_SIZE	(S31_HOSTED_SLOT_SIZE - 8U)
#define S31_HOSTED_H0_TO_H1_OFFSET	S31_HOSTED_CTRL_SIZE
#define S31_HOSTED_H1_TO_H0_OFFSET	\
	(S31_HOSTED_H0_TO_H1_OFFSET + S31_HOSTED_SLOT_COUNT * S31_HOSTED_SLOT_SIZE)

#define S31_HOSTED_MAGIC		0x53334846U /* "FH3S" little endian */
#define S31_HOSTED_ABI_VERSION		1U

#define S31_HOSTED_H0_READY		(1U << 0)
#define S31_HOSTED_H1_READY		(1U << 1)

#define S31_HOSTED_FEAT_WIFI		(1U << 0)
#define S31_HOSTED_FEAT_BT		(1U << 1)
#define S31_HOSTED_FEAT_COEX		(1U << 2)

/* Values match esp_hosted_fg/common/include/adapter.h. */
enum s31_hosted_if_type {
	S31_HOSTED_STA_IF = 0,
	S31_HOSTED_AP_IF,
	S31_HOSTED_SERIAL_IF,
	S31_HOSTED_HCI_IF,
	S31_HOSTED_PRIV_IF,
	S31_HOSTED_TEST_IF,
	S31_HOSTED_MAX_IF,
};

enum s31_hosted_control_type {
	S31_HOSTED_CTRL_PING = 1,
	S31_HOSTED_CTRL_PONG,
	S31_HOSTED_CTRL_LINK,
	S31_HOSTED_CTRL_RADIO_READY,
	/* OpenSBI asks hart0 to enter IDF's chip-wide deep-sleep path. */
	S31_HOSTED_CTRL_POWER_OFF,
	/* OpenSBI asks hart0 to execute IDF's complete restart sequence. */
	S31_HOSTED_CTRL_RESTART,
};

enum s31_hosted_link_state {
	S31_HOSTED_LINK_DOWN = 0,
	S31_HOSTED_LINK_UP = 1,
};

/*
 * Wire header from ESP-Hosted-FG.  SRAM slots contain this header followed by
 * the payload at @offset.  Both harts are RV32 little-endian.
 */
struct s31_esp_payload_header {
	s31_u8 if_type:4;
	s31_u8 if_num:4;
	s31_u8 flags;
	s31_u16 len;
	s31_u16 offset;
	s31_u16 checksum;
	s31_u16 seq_num;
	s31_u8 reserved2;
	union {
		s31_u8 reserved3;
		s31_u8 hci_pkt_type;
		s31_u8 priv_pkt_type;
	};
} __attribute__((packed));

struct s31_hosted_control_msg {
	s31_u8 type;
	s31_u8 value;
	s31_u16 length;
	s31_u32 generation;
	s31_u8 data[16];
} __attribute__((packed));

struct s31_hosted_ring_state {
	volatile s31_u32 producer;
	s31_u8 producer_pad[60];
	volatile s31_u32 consumer;
	s31_u8 consumer_pad[60];
	volatile s31_u32 drops;
	s31_u8 drops_pad[60];
} __attribute__((aligned(64)));

struct s31_hosted_control {
	volatile s31_u32 magic;
	volatile s31_u32 abi_version;
	volatile s31_u32 generation;
	volatile s31_u32 state;
	volatile s31_u32 features;
	volatile s31_u32 wifi_state;
	s31_u8 sta_mac[6];
	s31_u8 bt_mac[6];
	volatile s31_u32 h0_irq_count;
	volatile s31_u32 h0_rx_polls;
	volatile s31_u32 h0_seen_h1_producer;
	volatile s31_u32 h0_seen_h1_sequence;
	volatile s31_u32 h0_apm_status;
	volatile s31_u32 h0_h1_doorbell;
	s31_u8 header_pad[4];
	struct s31_hosted_ring_state h0_to_h1;
	struct s31_hosted_ring_state h1_to_h0;
};

struct s31_hosted_slot {
	volatile s31_u32 sequence;
	volatile s31_u16 length;
	volatile s31_u8 flags;
	volatile s31_u8 reserved;
	s31_u8 data[S31_HOSTED_SLOT_DATA_SIZE];
};

_Static_assert(sizeof(struct s31_esp_payload_header) == 12,
	       "ESP-Hosted payload header ABI changed");
_Static_assert(sizeof(struct s31_hosted_control_msg) == 24,
	       "hosted control message ABI changed");
_Static_assert(sizeof(struct s31_hosted_ring_state) == 192,
	       "hosted ring state must occupy three cache lines");
_Static_assert(sizeof(struct s31_hosted_control) == 448,
	       "hosted control block ABI changed");
_Static_assert(sizeof(struct s31_hosted_slot) == S31_HOSTED_SLOT_SIZE,
	       "hosted slot ABI changed");
_Static_assert(S31_HOSTED_H1_TO_H0_OFFSET +
	       S31_HOSTED_SLOT_COUNT * S31_HOSTED_SLOT_SIZE ==
	       S31_HOSTED_SRAM_SIZE,
	       "hosted SRAM layout does not fill its reservation");

#endif /* S31_HOSTED_SRAM_H */
