/*
 * Chihiro Type-3 media board ASIC — the machine around the V850 core
 *
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#ifndef CHIHIRO_ASIC_H
#define CHIHIRO_ASIC_H

#include <stdint.h>
#include <stdbool.h>

/* Builds the board and starts its thread. */
void chihiro_asic_init(void);

/* Hands the core an image, decrypting an upload; false, with the reason
 * logged, when it is not a V850 image. */
bool chihiro_asic_load_firmware(const uint8_t *img, uint32_t len,
                                const char *origin);

/* True when the board has a firmware and its core runs: out of reset, and not
 * stopped on an opcode it does not know. */
bool chihiro_asic_running(void);

/* True once the host has let the core out of reset, whether it runs or not. */
bool chihiro_asic_released(void);

/* The 0x80000140 latch: bit 0 releases the CPU, clearing it holds it in reset. */
void chihiro_asic_set_running(bool running);

/* The host's 0x84000000-0x8400003F is the board's 0x0FC00000-0x0FC0003F.
 * Each write lands at once: the reply pump posts only once the host has
 * cleared the previous answer. `offset` is a byte offset in the window. */
void chihiro_asic_host_mailbox_write(uint32_t offset, uint32_t val);

/* The host rang the board with the 32-byte message in its command window. A
 * command runs inline until the board posts its next message; a reply to one
 * of the board's own commands (bit 31) runs until the board takes it. True,
 * with msg8 filled, when a message was posted meanwhile; false when the core
 * is not running or has posted nothing yet: whatever it posts later reaches
 * chihiro_dimm_board_posted(). */
bool chihiro_asic_mailbox_exchange(const uint32_t *cmd8, uint32_t *msg8);

#endif
