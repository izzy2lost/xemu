/*
 * Chihiro JVS I/O Board Emulation (Sega 837-13551)
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
#ifndef CHIHIRO_JVS_H
#define CHIHIRO_JVS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JVS_MAX_PLAYERS    2
#define JVS_MAX_COINS      2
#define JVS_MAX_ANALOG     8

#define JVS_SYNC           0xE0
#define JVS_BROADCAST      0xFF
#define JVS_HOST_ADDR      0x00

#define JVS_STATUS_OK      0x01

#define JVS_REPORT_OK      0x01
#define JVS_REPORT_PARAM   0x02

typedef struct {
    uint8_t  device_id;
    uint8_t  sense;                    /* 3=unassigned, 0=addressed */
    uint16_t coin_count[JVS_MAX_COINS];
    uint8_t  reset_count;              /* must reach 2 for actual reset */

    uint8_t  system_switches;          /* test=0x80, tilt1=0x40 ... */
    uint8_t  player_switches[JVS_MAX_PLAYERS][2];
    uint16_t analog[JVS_MAX_ANALOG];   /* 16-bit, 0x8000 = center */
} ChihiroJVSState;

extern ChihiroJVSState *chihiro_jvs_global;
/* The card drawers' solenoids on an HW210 cabinet, one per slot: while one
 * is on its card cannot come out, and the game ejects the card by turning
 * it off (the drawer's spring pushes it out). */
extern bool chihiro_jvs_card_lock[2];
/* How many times the game has read the switches: the card slots count in
 * these rather than in time, so a slow host gives the game the same number
 * of looks at a card. */
extern uint32_t chihiro_jvs_switch_reads;

void chihiro_jvs_init(ChihiroJVSState *s);

/*
 * Process a raw JVS frame (starting with 0xE0 sync).
 * Returns the number of bytes written to out[], or 0 if no response
 * (broadcast commands like Reset produce no response).
 */
int  chihiro_jvs_process(ChihiroJVSState *s,
                          const uint8_t *in, int in_len,
                          uint8_t *out, int out_max);

#ifdef __cplusplus
}
#endif

#endif
