/*
 * Sega 838-13683 drive board (OutRun 2 force feedback)
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
#include "chihiro-driveboard-sega838.h"

DriveBoardState *chihiro_driveboard_global = NULL;

static void driveboard_push_response(DriveBoardState *db, uint8_t val)
{
    int next = (db->resp_tail + 1) % DRIVEBOARD_RESP_SIZE;
    if (next == db->resp_head) {
        return;  // response FIFO full, drop
    }
    db->resp_buf[db->resp_tail] = val;
    db->resp_tail = next;
}

void driveboard_init(DriveBoardState *db)
{
    memset(db, 0, sizeof(*db));
    db->play_pkg = -1;   /* no package playing */
}

void driveboard_receive_byte(DriveBoardState *db, uint8_t byte)
{
    if (byte & 0x80)
        db->tx_pos = 0;

    db->tx_buf[db->tx_pos++] = byte;
    if (db->tx_pos < 4) return;
    db->tx_pos = 0;

    uint8_t cmd = db->tx_buf[0] & 0x7F;
    uint8_t p1  = db->tx_buf[1];
    uint8_t p2  = db->tx_buf[2];

    switch (cmd) {
    /* --- init / handshake --- The reply is the board status (bits 0-2,
     * mirrored in 4-6): 1 while it initializes, 0 in service. The init
     * state machine needs the 1 after RESET and SET_MODE (with a 0 it
     * re-sends SET_MODE forever); once db_runtime_pump (0x1d950) runs, a
     * status 1 is the ENCODER ALARM fault, and the centering task ends with
     * another SET_MODE (0x1df87). The first poll marks the board in service. */
    case 0x7F:  /* wire 0xFF: motor reset */
        db->initializing = true;
        driveboard_push_response(db, 0x11);
        break;
    case 0x01:  /* wire 0x81: SET_MODE (P1 = mode, P2 = 0x7F). Not power. */
        driveboard_push_response(db, db->initializing ? 0x11 : 0x00);
        break;
    case 0x7C:  /* wire 0xFC: start motor board (game polls, ignores this) */
        db->initializing = false;
        break;
    case 0x7D:  /* wire 0xFD: poll / watchdog */
        db->initializing = false;
        driveboard_push_response(db, 0x00);
        break;

    /* --- SUD effect commands (packages: see chihiro-driveboard-sega838.h) --- */
    case 0x00:  /* wire 0x80: motor power. 00 00 = off, anything else = on. */
        db->motor_active = !(p1 == 0 && p2 == 0);
        if (!db->motor_active) {
            db->spring_active   = false;
            db->damper_level    = 0;
            db->friction_power  = 0;
            db->vibration_power = 0;
            db->movement_power  = 0;
            db->event_pulse     = 0;
            db->play_pkg        = -1;
        }
        driveboard_push_response(db, 0x00);
        break;
    case 0x03:  /* wire 0x83: global power level (test-menu FFB %) */
        db->global_power = p1;
        driveboard_push_response(db, 0x00);
        break;
    case 0x04:  /* wire 0x84: movement (direction + power) */
        db->movement_dir   = p1;
        db->movement_power = p2;
        driveboard_push_response(db, 0x00);
        break;
    case 0x05:  /* wire 0x85: rumble / vibrate */
    case 0x09:  /* wire 0x89: vibrate (same command); P1 = speed, not used */
        db->vibration_power = p2;
        driveboard_push_response(db, 0x00);
        break;
    case 0x06:  /* wire 0x86: constant torque / road resistance. The wire is
                 * [0x86, 0x2F, force]: P1=0x2F is a FIXED range constant and the
                 * actual force is P2 (register-confirmed at db_dispatch_effects
                 * 0x1d8c2: MOV DL,[DAT_002fd0dd] -> P2; [ESP+4]=0x2F -> P1). */
        db->friction_power = p2;
        driveboard_push_response(db, 0x00);
        break;
    case 0x07:  /* wire 0x87: SPRING -- the wheel's main auto-centering force
                 * (P1=direction, P2=magnitude). The host renders it with the
                 * wheel's autocentre (and an SDL spring), so we only latch
                 * that centering is engaged. */
        db->spring_active = true;
        driveboard_push_response(db, 0x00);
        break;
    case 0x08:  /* wire 0x88: DAMPER (P2 = 0x08 slow / 0x04 fast, speed-based). */
        db->damper_level = p2;
        driveboard_push_response(db, 0x00);
        break;
    case 0x0B:  /* wire 0x8B: VIBRATION -- road/engine buzz, NOT centering.
                 * P1 = level*8 (0x20-0x78, rises with speed), P2 = freq*2. OR2's
                 * continuous vibration channel (command 0x0B, outrun2.xbe's
                 * db_dispatch_effects). Not rendered: ack only. */
        driveboard_push_response(db, 0x00);
        break;
    case 0x1D:  /* wire 0x9D: package upload cursor (p1=movement idx, p2=pkg) */
        db->sud_up_idx = p1;
        db->sud_up_pkg = p2;
        driveboard_push_response(db, 0x00);
        break;
    case 0x1E:  /* wire 0x9E: package movement value (p1=direction, p2=power) */
        if (db->sud_up_pkg < 16 && db->sud_up_idx < 16) {
            db->sud_pkg[db->sud_up_pkg][db->sud_up_idx] =
                (uint8_t)((p1 << 7) | (p2 & 0x7F));
        }
        driveboard_push_response(db, 0x00);
        break;
    case 0x7B:  /* wire 0xFB: SUD package playback -- the wall / car / surface
                 * effects. p2 low nibble = package id. Start playing that
                 * package's 16-movement pattern so each event has its own
                 * texture (impact jolt vs surface buzz). */
        db->play_pkg = (int8_t)(p2 & 0x0F);
        db->play_pos = 0;
        db->play_sub = 0;
        driveboard_push_response(db, 0x00);
        break;

    /* 0x02 (0x82), 0x0A (0x8A deadzone), 0x70 (0xF0 target),
     * 0x7A (0xFA echo): ack only. */
    default:
        driveboard_push_response(db, 0x00);
        break;
    }
}

bool driveboard_has_response(DriveBoardState *db)
{
    return db->resp_head != db->resp_tail;
}

uint8_t driveboard_get_response(DriveBoardState *db)
{
    if (db->resp_head == db->resp_tail) return 0x00;
    uint8_t val = db->resp_buf[db->resp_head];
    db->resp_head = (db->resp_head + 1) % DRIVEBOARD_RESP_SIZE;
    return val;
}

uint16_t driveboard_get_rumble(DriveBoardState *db)
{
    if (!db->motor_active) return 0;

    /* A gamepad can only buzz, so it renders the discrete EVENT channel:
     * playback packages (0xFB) and explicit vibration (0x85). It deliberately
     * does NOT render the continuous road vibration (0x8B): on a pad that speed-
     * scaled buzz runs the whole time and eases off in the sand, which reads as
     * an always-on, inverted rumble. Centering/damper are wheel-only forces a
     * pad cannot reproduce. Distinct textures come from the package pattern. */
    uint32_t mag = db->event_pulse;                    /* package power 0-0x7F */
    uint32_t vib = (uint32_t)db->vibration_power * 2;  /* 0x85 power 0-0x3F */
    if (vib > mag) mag = vib;

    /* Gamepad motors are weaker than the arcade wheel, so boost: surface buzz
     * (power ~0x20) is felt while impacts (0x7F) clip to full. The user's FFB-
     * strength slider is the overall control (applied host side). */
    uint32_t rumble = mag * 780;                       /* power ~0x54 -> full */
    if (rumble > 0xFFFF) rumble = 0xFFFF;

    return (uint16_t)rumble;
}

void driveboard_get_ffb(DriveBoardState *db, DriveBoardFFB *out)
{
    if (!db->motor_active) {
        memset(out, 0, sizeof(*out));
        return;
    }
    out->active       = true;
    out->global_power = db->global_power;

    /* SPRING (0x87): the wheel's dominant auto-centering. The board computes the
     * restoring force from wheel position; on the host the wheel's autocentre
     * (and an SDL spring) does that, so we just expose "engaged" scaled by the
     * game's global power. */
    out->centering_power = db->spring_active
                             ? (db->global_power ? db->global_power : 0x60) : 0;

    /* DAMPER: the constant road-resistance torque (0x86 P2 -- the real force, NOT
     * the inverted P1; P1=0x2F is a fixed constant) combined with the speed-based
     * damper (0x88 P2). Higher = stronger; the host layer only scales. */
    {
        int torque = db->friction_power;          /* 0x86 P2 force (small, direct) */
        int damper = (int)db->damper_level * 8;   /* 0x88 P2: 0x04 -> 0x20, 0x08 -> 0x40 */
        out->friction_power = (uint8_t)(torque > damper ? torque : damper);
    }

    out->movement_dir   = db->movement_dir;
    out->movement_power = db->movement_power;

    /* Advance SUD package playback: one movement every 2 calls, its direction
     * and power as the game uploaded them (0x9E). This accessor is the tick:
     * the host calls it once per UI frame while force feedback is on and a
     * device steers, so playback follows the display's rate. */
    uint8_t power = 0, dir = 0;
    if (db->play_pkg >= 0 && db->play_pkg < 16) {
        uint8_t step = db->sud_pkg[db->play_pkg][db->play_pos & 0x0F];
        power = step & 0x7F;
        dir = step >> 7;
        if (++db->play_sub >= 2) {
            db->play_sub = 0;
            if (++db->play_pos >= 16) {
                db->play_pkg = -1;   /* sequence complete */
            }
        }
    }
    db->event_pulse = power;   /* package pulse -> gamepad rumble */

    /* Per A.Geezer the SUD packages are OR2's real tactile feedback. */
    out->event_power = power;  /* this frame's package movement */
    out->event_dir = dir;
}
