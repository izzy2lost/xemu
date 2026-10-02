/*
 * Chihiro media board — the DIMM SPD EEPROMs and the I2C the firmware
 * bit-bangs to reach them
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
 *
 * Measured in the firmware: the board init reaches the DIMM scan only for
 * board types 0 and 1 (straps 4 and 6), through two registers. With the
 * straps xemu presents (3, chihiro-asic.c) the scan never runs: the model is
 * kept for the other board types.
 *
 *   0x0FE00020  bit 0 = SCL, bit 1 = SDA. Written to drive the lines, read to
 *               see them. The bit-read routine at 0x1A7E raises bit 0, samples
 *               bit 1, lowers bit 0 again.
 *   0x0FE00024  bit 1 = the SDA output enable. 2 drives the line, 0 releases
 *               it so a slave can pull it down. Start (0x1AE8) drops SDA while
 *               SCL is high, stop (0x1B60) raises it while SCL is high.
 *
 * The scan then reads byte 31 of the SPD (module bank density, JEDEC) and byte
 * 5 (number of banks) to work out the DIMM size, and checks byte 63, the
 * checksum, against its own sum of bytes 0 to 62.
 */
#ifndef CHIHIRO_SPD_H
#define CHIHIRO_SPD_H

#include <stdint.h>
#include <stdbool.h>

#define CHIHIRO_SPD_SLOTS 2

typedef struct ChihiroSpd {
    uint8_t  rom[CHIHIRO_SPD_SLOTS][256];
    bool     present[CHIHIRO_SPD_SLOTS];

    /* What the master is doing to the two lines. */
    bool     scl_out;           /* 0x0FE00020 bit 0, what the master drives */
    bool     scl;               /* SCL as it was at the previous step */
    bool     sda;               /* SDA as it was at the previous step */
    bool     master_drives;     /* 0x0FE00024 bit 1 */
    bool     master_level;      /* 0x0FE00020 bit 1 */
    bool     slave_pulls;       /* we are holding SDA down */

    int      state;
    int      bit;               /* bits shifted so far */
    uint8_t  shift;             /* the byte being shifted in */
    int      slot;              /* the slot addressed, -1 when none */
    uint8_t  ptr;               /* the byte pointer inside that slot */
    uint8_t  out;               /* the byte being shifted out */
} ChihiroSpd;

/* Fills both slots with a JEDEC SPD for one SDRAM module of the given size in
 * megabytes, checksum included. A size of 0 marks the slot empty. */
void chihiro_spd_init(ChihiroSpd *s, unsigned slot0_mb, unsigned slot1_mb);

/* 0x0FE00020 and 0x0FE00024, as the firmware sees them. */
uint32_t chihiro_spd_read_lines(ChihiroSpd *s);
void     chihiro_spd_write_lines(ChihiroSpd *s, uint32_t val);
void     chihiro_spd_write_enable(ChihiroSpd *s, uint32_t val);

#endif
