/*
 * What a Chihiro cabinet carries
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
#ifndef HW_XBOX_CHIHIRO_CABINET_H
#define HW_XBOX_CHIHIRO_CABINET_H

/* What each cabinet carries, answered from the one table in chihiro.c. */

typedef enum {
    CHIHIRO_CARD_NONE = 0,
    CHIHIRO_CARD_HW210,      /* Tamura/SAXA slots: two on Ghost Squad, one on Gundam */
    CHIHIRO_CARD_CRP1231,    /* one Sanwa reader-printer, Maximum Tune */
} ChihiroCardReaderKind;

/* Which monitor bit of the baseboard DIP byte the cabinet carries.
 * PROVEN, the byte's path: the QC firmware (ic10, 0x06CA) reads port A into
 * byte 1 of a control answer; MJ3 Evolution requests it (mj3.xbe 0x0037823E),
 * keeps it (0x00378457, global 0x00371378) and raises Caution 51 unless
 * (byte & 0x16) is 0x00 or 0x04 (0x0024E556).
 * MEASURED over nine titles: bit 1 alone, every cabinet boots and MJ3 refuses;
 * bit 2 alone, MJ3, MJ2 and Golf boot and the others get SEGABOOT's Caution
 * 51; both, neither or bit 4: SEGABOOT refuses everything.
 * NOT PROVEN: that bit 1 is 15 kHz and bit 2 31 kHz, the reading that fits
 * the message and the cabinets it splits. */
typedef enum {
    CHIHIRO_MONITOR_15KHZ = 0,   /* bit 1: standard resolution, the default */
    CHIHIRO_MONITOR_31KHZ,       /* bit 2: progressive; mahjong, golf */
} ChihiroMonitorKind;

typedef enum {
    CHIHIRO_DRIVE_NONE = 0,
    CHIHIRO_DRIVE_SEGA838,   /* Sega 838-13683, OutRun 2 */
    CHIHIRO_DRIVE_V257,      /* Namco V257 STR PCB, Maximum Tune */
} ChihiroDriveBoardKind;

ChihiroCardReaderKind chihiro_cabinet_card_reader(void);
ChihiroDriveBoardKind chihiro_cabinet_drive_board(void);
ChihiroMonitorKind    chihiro_cabinet_monitor(void);
/* The JVS general-purpose output (bank 0) that locks this slot's card in,
 * 0 when the cabinet has none there. */
uint8_t               chihiro_cabinet_card_lock(int player);

#endif
