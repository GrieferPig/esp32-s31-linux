/* SPDX-License-Identifier: BSD-2-Clause */
/* Compile IDF's open transport here so the Linux TX adapter can use its
 * private forward callback without an address-dependent lookup. This object
 * precedes libbt.a and provides the unchanged VHCI receive/status/ops ABI. */
#include <errno.h>
#include "hci_driver_standard.c"
#include "s31_vhci_acl.inc"
#include "s31_vhci_bredr.inc"
#include "s31_vhci_command.inc"
