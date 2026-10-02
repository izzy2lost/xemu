/*
 * Namco V257 STR PCB (Maximum Tune steering and force feedback)
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
#ifndef CHIHIRO_DRIVEBOARD_V257_H
#define CHIHIRO_DRIVEBOARD_V257_H

#include <stdint.h>
#include <stdbool.h>

#define V257_CMD_LEN   10
#define V257_MSG_LEN   3
#define V257_MSG_SLOTS 4
#define V257_RESP_SIZE 16

/* The Maximum Tune cabinet's Namco V257 STR PCB, on SC UART1 (where OutRun 2
 * has its Sega 838-13683). The same board serves Ridge Racer V and Wangan
 * Midnight on System 246; its firmware (rrv3_str-0a.ic16, Fujitsu MB90242A)
 * gives the status words: 'E00'-'E15' errors, 'C01' power on, 'C06' off.
 *
 * Host -> board: a 10-byte frame (V322.xbe 0x00055E60, sent by 0x00056110),
 * built only when there is a new force, after the previous one was acked:
 *
 *     FF FF | pos_lo pos_hi | force_a 00 | force_b 00 | torque 00
 *
 * pos: the 16-bit little-endian target, 0-0x3FF, slew-limited to 0x35 a
 * frame. force_a: spring, force_b: damping, 0-0x7F. torque: a short push,
 * direction in bit 7, size complemented when set (+v sent as ~v). All three
 * at zero: the idle position update.
 *
 * Board -> host (parsed by 0x00056B10):
 *   'H' hi lo    the wheel position read back, big-endian (0x00056CC0); it
 *                is also the ack that lets the game build its next frame.
 *   'C' '0' '1'  motor powered, 'C' '0' '6' unpowered, each sent twice
 *                (firmware status table at 0x1440: "C01 POWER ON").
 *   'E' x y      error; 'E00' twice once its self-test passed.
 * The boot wheel check (0x00056DD0) raises the motor line (JVS GPO bank 0
 * bit 7), waits for C01, holds 2 s, drops it and waits for C06; 20 s on any
 * step and the game writes its own E20.
 *
 * What the board does with a frame (rrv3_str-0a.ic16, MAME's F2MC-16
 * disassembler; timebase 256 us):
 *
 *   target   taken when within 0x6F of the one it holds, ignored otherwise
 *            (0x000E72); 0x1FF while the motor line is low.
 *   scaling  spring fa' = min(29 fa / 16, 0x7F), damping fb' = min(2 fb,
 *            0x7F), push tq' = min(2 |tq|, 0x7F) (0x000DCD-0x000E0E).
 *            HYPOTHESIS: the push keeps its sign. This dump, Ridge Racer V's,
 *            caps the byte unsigned, but its own push table (0x2680) is built
 *            for the signed encoding Maximum Tune sends.
 *   law      each tick, a motor current out of a 338 full scale (tables
 *            0x2280, 0x2480, 0x2680, 0x1C80, 0x2080 into the product-sum unit
 *            at 0x1900, read at 2^-12; at 2^-10 all three would be four
 *            times larger):
 *              spring   -2.014 fa' tanh(e / 163), e the wheel's distance past
 *                       the target in pot counts: up to 255, 75 %
 *              damping  -(fb' / 127) 0.126 per count/s of wheel speed: full
 *                       scale from about 2700 counts/s
 *              push     0.594 tq': up to 75, 22 %, for at most 0x182 ticks
 *                       (99 ms); a frame without one ends it, one turning
 *                       it around starts it again (0x0011C1-0x001291)
 *   power    line up: 0x3000 ticks (3.15 s) with no answer, then C01; line
 *            down: C06. The first power-up after its own power-on runs a
 *            self-test first and reports E00.
 *
 * What Maximum Tune sends: menus, spring 0x7F (MT1) or 0x64 (MT2) with damping
 * 0x7F; a race, spring 0x60 falling to 0 in a slide, damping 0x7E, pushes of
 * 26-48 on a road joint and 50-126 on a contact.
 */
/* The board's own condition, as its firmware steps through it. */
enum {
    V257_BOOTING = 0,   /* just powered: serial off, waits for the motor line */
    V257_SELF_TEST,     /* line up: powering up, then reports E00 */
    V257_BOOT_WAIT_LOW, /* E00 sent, waits for the line to drop, then C06 */
    V257_OFF,           /* main loop, line low: target 0x1FF, no force */
    V257_POWERING,      /* line up: frames stored, not answered */
    V257_POWERED,       /* C01 sent: the forces act */
};

typedef struct V257DriveBoard {
    uint8_t  cmd[V257_CMD_LEN];
    int      cmd_pos;

    /* Messages waiting for the wire, one per service tick (the game keeps
     * the first three bytes it reads); a status goes out twice (0x001391). */
    uint8_t  pending[V257_MSG_SLOTS][V257_MSG_LEN];
    int      pending_head;
    int      pending_tail;

    uint8_t  resp[V257_RESP_SIZE];
    int      resp_head;
    int      resp_tail;

    uint16_t position;   /* the target the board holds, 0-0x3FF */
    uint16_t wheel;      /* where the wheel actually is, 0-0x3FF */
    bool     motor_on;   /* the cabinet relay, as the JVS output drives it */
    bool     motor_known;
    uint8_t  force_a;    /* the spring, as the board scales it: 0-0x7F */
    uint8_t  force_b;    /* the damping, 0-0x7F */
    int8_t   torque;     /* the push, -0x7F-0x7F, + toward a higher position */

    bool     pending_twice[V257_MSG_SLOTS];
    uint8_t  state;      /* V257_* */
    int      countdown;  /* service ticks left in a power-up or self-test */
    int      torque_ticks;  /* service ticks left in the push */
} V257DriveBoard;

/* What the board drives the wheel with, for the host's force feedback: the
 * target, the spring and damping (0-0x7F) and the push (signed, 0 between
 * pushes). False while it drives nothing: unpowered, or all of them at 0. */
bool    v257_drive(V257DriveBoard *db, int *target, int *spring, int *damping,
                   int *torque);

void    v257_init(V257DriveBoard *db);

/* One step of the board, on the cabinet's 16 ms service tick. */
void    v257_tick(V257DriveBoard *db);

/* The motor line, a JVS general-purpose output. */
void    v257_set_motor(V257DriveBoard *db, bool on);

/* Where the wheel is, 0-0x3FF, from the host input (a real V257 reads the
 * column's encoder). */
void    v257_set_wheel(V257DriveBoard *db, uint16_t pos);
void    v257_receive_byte(V257DriveBoard *db, uint8_t byte);
bool    v257_has_response(V257DriveBoard *db);
uint8_t v257_get_response(V257DriveBoard *db);

extern V257DriveBoard *chihiro_v257_global;

#endif
