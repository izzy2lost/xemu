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
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "chihiro-driveboard-v257.h"
#include "chihiro-log.h"
#include <string.h>
#include <stdio.h>

V257DriveBoard *chihiro_v257_global = NULL;

/* The board's firmware, rrv3_str-0a.ic16 (MB90242A, "V257 STR PCB FOR NAI
 * Ver.1.01"), read with MAME's F2MC-16 disassembler: its timebase ticks every
 * 256 us, and this model steps on the cabinet's 16 ms service tick, so its
 * counts are converted. A power-up, and the self-test's, is 0x3000 ticks,
 * 3.15 s (0x000F94); a push 0x182 ticks, 99 ms (0x001267). */
#define V257_POWERUP_TICKS 197
#define V257_TORQUE_TICKS  6

/* How the board paces itself. One message per service tick: the game reads
 * every byte the SC has buffered (V322.xbe 0x00056B10), and Maximum Tune 2
 * drops a status that follows an 'H' in the same read. A status therefore
 * starts on an empty line, and one queued while an 'H' still waits takes its
 * place: the firmware holds that 'H' back too (0x000C28). */
static void v257_queue(V257DriveBoard *db, uint8_t a, uint8_t b, uint8_t c,
                       bool twice)
{
    int next;

    if (twice) {
        /* A status beats an answer still waiting to go out. */
        while (db->pending_head != db->pending_tail &&
               db->pending[(db->pending_tail + V257_MSG_SLOTS - 1) %
                           V257_MSG_SLOTS][0] == 'H')
            db->pending_tail = (db->pending_tail + V257_MSG_SLOTS - 1) %
                               V257_MSG_SLOTS;
    }
    next = (db->pending_tail + 1) % V257_MSG_SLOTS;
    if (next == db->pending_head)
        return;  /* nowhere left to put it: the newest news is dropped */
    db->pending[db->pending_tail][0] = a;
    db->pending[db->pending_tail][1] = b;
    db->pending[db->pending_tail][2] = c;
    db->pending_twice[db->pending_tail] = twice;
    db->pending_tail = next;
}

/* A status goes out twice in a row, as the firmware's transmitter repeats it
 * (0x001391): "C01C01". Maximum Tune collects six bytes of it. */
static void v257_status(V257DriveBoard *db, const char *code)
{
    CHIHIRO_LOGF(FFB, "V257 drive board: %s\n", code);
    v257_queue(db, code[0], code[1], code[2], true);
}

static void v257_push(V257DriveBoard *db, uint8_t val)
{
    int next = (db->resp_tail + 1) % V257_RESP_SIZE;
    if (next == db->resp_head)
        return;  /* response FIFO full, drop */
    db->resp[db->resp_tail] = val;
    db->resp_tail = next;
}

/* Put the next waiting message on the wire, if the wire is free. */
static void v257_emit(V257DriveBoard *db)
{
    int n;

    if (db->pending_head == db->pending_tail)
        return;
    if (db->resp_head != db->resp_tail)
        return;  /* the previous message is still going out */

    n = db->pending_twice[db->pending_head] ? 2 : 1;
    for (int k = 0; k < n; k++)
        for (int i = 0; i < V257_MSG_LEN; i++)
            v257_push(db, db->pending[db->pending_head][i]);
    db->pending_head = (db->pending_head + 1) % V257_MSG_SLOTS;
}

/* Line low in the main loop: no target but the centre, no force (0x000F41,
 * every tick). */
static void v257_rest(V257DriveBoard *db)
{
    db->position = 0x1FF;
    db->force_a = db->force_b = 0;
    db->torque = 0;
    db->torque_ticks = 0;
}

void v257_tick(V257DriveBoard *db)
{
    switch (db->state) {
    case V257_BOOTING:
        /* Serial off until the boot is over: it waits for the line. */
        if (db->motor_on) {
            db->state = V257_SELF_TEST;
            db->countdown = V257_POWERUP_TICKS;
        }
        break;
    case V257_SELF_TEST:
        /* SIMPLIFIED: the self-test also turns the motor by an open-loop ramp
         * and checks that it moves (0x00051C); here it passes. A line dropped
         * meanwhile hangs the real board on E14; here it starts again. */
        if (!db->motor_on) {
            db->state = V257_BOOTING;
        } else if (--db->countdown <= 0) {
            v257_status(db, "E00");
            db->state = V257_BOOT_WAIT_LOW;
        }
        break;
    case V257_BOOT_WAIT_LOW:
        if (!db->motor_on) {
            v257_status(db, "C06");
            v257_rest(db);
            db->state = V257_OFF;
        }
        break;
    case V257_OFF:
        v257_rest(db);
        if (db->motor_on) {
            db->state = V257_POWERING;
            db->countdown = V257_POWERUP_TICKS;
        }
        break;
    case V257_POWERING:
        if (!db->motor_on) {
            v257_rest(db);
            db->state = V257_OFF;
        } else if (--db->countdown <= 0) {
            db->state = V257_POWERED;
            v257_status(db, "C01");
        }
        break;
    case V257_POWERED:
        if (!db->motor_on) {
            v257_status(db, "C06");
            v257_rest(db);
            db->state = V257_OFF;
        }
        break;
    }

    /* A push ends by itself, and the board clears the one it was given
     * (0x00127B). */
    if (db->torque_ticks > 0 && --db->torque_ticks == 0)
        db->torque = 0;

    v257_emit(db);
}

void v257_init(V257DriveBoard *db)
{
    memset(db, 0, sizeof(*db));
    db->state = V257_BOOTING;
    db->position = 0x1FF;
}

/* The cabinet's motor line, as the JVS general-purpose output drives it. The
 * board acts on it at its next tick. */
void v257_set_motor(V257DriveBoard *db, bool on)
{
    if (db->motor_known && db->motor_on == on)
        return;
    db->motor_known = true;
    db->motor_on = on;
    CHIHIRO_LOGF(FFB, "V257 drive board: motor %s\n", on ? "on" : "off");
}

void v257_set_wheel(V257DriveBoard *db, uint16_t pos)
{
    db->wheel = pos > 0x3FF ? 0x3FF : pos;
}

void v257_receive_byte(V257DriveBoard *db, uint8_t byte)
{
    uint16_t raw;
    int16_t delta;
    int fa, fb, tq;

    /* Until its boot is over the board's serial port is off (0x000021). */
    if (db->state < V257_OFF)
        return;

    /* The frame opens on two 0xFF; nothing else can start one, so a byte that
     * is not 0xFF while the header is still incomplete resynchronises us. */
    if (db->cmd_pos < 2) {
        db->cmd_pos = (byte == 0xFF) ? db->cmd_pos + 1 : 0;
        db->cmd[0] = db->cmd[1] = 0xFF;
        return;
    }

    db->cmd[db->cmd_pos++] = byte;
    if (db->cmd_pos < V257_CMD_LEN)
        return;
    db->cmd_pos = 0;

    raw = (uint16_t)(db->cmd[2] | (db->cmd[3] << 8));
    CHIHIRO_LOGF(FFB, "V257 drive board: pos %04X force %02X/%02X torque %02X\n",
                 raw, db->cmd[4], db->cmd[6], db->cmd[8]);

    if (db->state == V257_OFF) {
        /* Answered, but whatever it asked is gone at the next tick. */
        v257_queue(db, 'H', (uint8_t)(db->wheel >> 8), (uint8_t)db->wheel,
                   false);
        v257_emit(db);
        return;
    }

    /* The board scales what it is given (0x000DCD-0x000E0E): the spring by
     * 29/16, the damping by 2, both capped at 0x7F. */
    fa = db->cmd[4] * 29 / 16;
    fb = db->cmd[6] * 2;
    db->force_a = fa > 0x7F ? 0x7F : fa;
    db->force_b = fb > 0x7F ? 0x7F : fb;

    /* The push keeps its sign (HYPOTHESIS, see the header). */
    tq = (db->cmd[8] & 0x80) ? (uint8_t)~db->cmd[8] : -(int)db->cmd[8];
    tq *= 2;
    tq = tq > 0x7F ? 0x7F : tq < -0x7F ? -0x7F : tq;
    /* A push lasts 99 ms from its start, whatever the frames that follow
     * say, unless one turns it around or ends it (0x0011C1-0x001291). */
    if (!tq)
        db->torque_ticks = 0;
    else if (!db->torque_ticks || (tq > 0) != (db->torque > 0))
        db->torque_ticks = V257_TORQUE_TICKS;
    db->torque = tq;

    /* A target more than 0x6F from the one the board holds is refused and
     * the old one kept (0x000E72-0x000EAE). */
    delta = (int16_t)(raw - db->position);
    if (delta > -0x70 && delta < 0x70)
        db->position = raw;

    /* Powering up, the board takes the frame but answers nothing. */
    if (db->state != V257_POWERED)
        return;

    /* Where the wheel is; it doubles as the ack the game waits for, so it is
     * answered at once. */
    v257_queue(db, 'H', (uint8_t)(db->wheel >> 8), (uint8_t)db->wheel, false);
    v257_emit(db);
}

bool v257_drive(V257DriveBoard *db, int *target, int *spring, int *damping,
                int *torque)
{
    bool powered = db->state == V257_POWERED;

    *target = db->position;
    *spring = powered ? db->force_a : 0;
    *damping = powered ? db->force_b : 0;
    *torque = powered && db->torque_ticks ? db->torque : 0;
    return *spring || *damping || *torque;
}

bool v257_has_response(V257DriveBoard *db)
{
    return db->resp_head != db->resp_tail;
}

uint8_t v257_get_response(V257DriveBoard *db)
{
    if (db->resp_head == db->resp_tail)
        return 0;
    uint8_t val = db->resp[db->resp_head];
    db->resp_head = (db->resp_head + 1) % V257_RESP_SIZE;
    return val;
}
