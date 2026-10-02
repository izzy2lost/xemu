/*
 * Chihiro media board — the security PIC and the three-wire link to it
 *
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * The board asks this chip who it is, and refuses to declare itself ready
 * until it answers. Measured in the firmware the game uploads:
 *
 *   0x00400008  direction: 0x0F while the board drives the lines, 0 to listen
 *   0x00400004  bits 0-2 data, bit 3 clock, bit 5 held high throughout
 *
 * A message is eight bytes and travels as twenty-five transfers of three bits:
 * per byte, its low three bits, its middle three, then its top two with the
 * byte's parity in bit 2. Every byte is scrambled with the session key, which
 * starts as "NAOMIGDROMSYSTEM" and which three of the commands redefine:
 *
 *   sent(v, n) = (v ^ key[8 + n]) + key[n]
 *
 * The command set and the answers are MAME's, documented in
 * src/mame/sega/naomi.cpp, itself crediting Elsemi for the scrambling.
 */
#ifndef CHIHIRO_PIC_H
#define CHIHIRO_PIC_H

#include <stdint.h>
#include <stdbool.h>

#define CHIHIRO_PIC_TRANSFERS 25

typedef struct ChihiroPic {
    uint8_t key[16];
    bool    board_drives;               /* 0x00400008 says the board is talking */
    bool    clk;                        /* the clock as the board last left it */
    uint8_t in[CHIHIRO_PIC_TRANSFERS];
    int     in_n;
    uint8_t out[CHIHIRO_PIC_TRANSFERS];
    int     out_n, out_i;
    bool    out_clk;                    /* the clock this chip presents */
} ChihiroPic;

void     chihiro_pic_reset(ChihiroPic *p);
void     chihiro_pic_write_dir(ChihiroPic *p, uint32_t val);
void     chihiro_pic_write_lines(ChihiroPic *p, uint32_t val);
uint32_t chihiro_pic_read_lines(ChihiroPic *p);

#endif
