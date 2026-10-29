/* SPDX-License-Identifier: BSD-2-Clause */
/* ESP32-S31 pinned controller ABI: bt_lmp_utils.c.o / olm_lmp.c.o.
 * LMP_MAX_SLOT (45) is a notification, not an acknowledged transaction.
 * A peer's NOT_ACCEPTED for it must not be dispatched by last-sent-opcode
 * into a synchronous-link procedure awaiting host acceptance. That state
 * has no incoming-PDU event and the controller otherwise asserts at
 * olm_lmp_sync.c:902. Real SCO/eSCO request rejections retain the SDK handler.
 * See Core Vol 2 Part C 4.1.10.
 */
#include <stddef.h>

typedef int (*s31_bt_lmp_handler_t)(unsigned int link, unsigned int tid,
                                   const uint8_t *pdu);
struct s31_bt_lmp_entry {
	uintptr_t unpack, pack;
	s31_bt_lmp_handler_t handler;
	uint8_t opcode, length;
	uint16_t reserved;
};
_Static_assert(sizeof(struct s31_bt_lmp_entry) == 16, "S31 LMP table entry ABI");
_Static_assert(offsetof(struct s31_bt_lmp_entry, handler) == 8, "S31 LMP handler ABI");
extern int __real_r_orca_lmp_tbl_register(unsigned int extended,
                                        const struct s31_bt_lmp_entry *table,
                                        unsigned int count);
static struct s31_bt_lmp_entry s31_bt_not_accepted_entry;
static s31_bt_lmp_handler_t s31_bt_not_accepted_original;

static int s31_bt_not_accepted(unsigned int link, unsigned int tid,
                               const uint8_t *pdu)
{
	/* The registered decoder has already checked the three-byte PDU length.
	 * Consume only an unsolicited reply to this notification. Do not alter
	 * procedure state, return a fabricated HCI completion, or swallow any
	 * rejection of the actual synchronous-link request. */
	if (pdu[0] == 4 && pdu[1] == 45) {
		return 0;
	}
	return s31_bt_not_accepted_original(link, tid, pdu);
}

int __wrap_r_orca_lmp_tbl_register(unsigned int extended,
                                  const struct s31_bt_lmp_entry *table,
                                  unsigned int count)
{
	const unsigned int slot = 3;

	/* Intercept only the pinned controller's normal 46-entry registration.
	 * Registering an additional override afterward is insufficient: lookup
	 * walks all tables and older matching entries win. Split this one table
	 * around opcode4, preserving every codec, flag and other handler. The
	 * two extra 16-byte registry nodes are init-only allocations and retire
	 * with the controller; the replacement entry lives with this payload.
	 */
	if (extended || !table || count != 46 || table[slot].opcode != 4 ||
	    table[slot].length != 3 || table[slot].reserved ||
	    !table[slot].unpack || !table[slot].pack || !table[slot].handler)
		return __real_r_orca_lmp_tbl_register(extended, table, count);

	s31_bt_not_accepted_entry = table[slot];
	s31_bt_not_accepted_original = table[slot].handler;
	s31_bt_not_accepted_entry.handler = s31_bt_not_accepted;
	if (!__real_r_orca_lmp_tbl_register(0, table, slot) ||
	    !__real_r_orca_lmp_tbl_register(0, &s31_bt_not_accepted_entry, 1) ||
	    !__real_r_orca_lmp_tbl_register(0, table + slot + 1, count - slot - 1))
		return 0;

	return 1;
}
