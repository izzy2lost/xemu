/*
 * Sanwa CRP-1231LR-10NAB card reader-printer emulation (Maximum Tune)
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
#include "chihiro-cardreader-crp1231.h"
#include "chihiro-log.h"
#include "chihiro.h"
#include <string.h>
#include <stdio.h>

#define STX 0x02
#define ETX 0x03
#define ENQ 0x05
#define ACK 0x06
#define NAK 0x15

/* The commands the reader acts on, as V322.xbe builds them; the whole set is
 * in the .h. */
#define CRP1231_CMD_CANCEL    0x40  /* 0x00057D20, see crp1231_status */
#define CRP1231_CMD_PRINT_TEXT 0x7C /* 0x0005BAF0 */
#define CRP1231_CMD_RETURN    0x80  /* 0x000586A0, hand the card back */
#define CRP1231_CMD_DISPENSE  0xB0  /* 0x00058A10 */
#define CRP1231_CMD_CARD      0x33  /* 0x00057E70 and 0x00057F30, see below */
#define CRP1231_CMD_WRITE     0x53  /* 'S', 0x00058730 */
#define CRP1231_CMD_CLEAN     0xA0  /* 0x00058000, see crp1231_clean */

/* A card starts with the signature of the game that wrote it. Maximum Tune 1
 * (V307.xbe FUN_00073A30) takes 0x8431, 0x7650-0x7652 and 0x1228; Maximum
 * Tune 2 (V322.xbe FUN_00058BA0) takes 0x8127, 0x7651 and 0x1097. So 2 reads
 * 1's cards, not the reverse, and a refused card is handed back and asked
 * for again (V322.xbe 0x00134680, V307.xbe 0x000D2A20). */

static void crp1231_respond(CRP1231State *r, uint8_t cmd, uint8_t param);

CRP1231State *chihiro_crp1231_global = NULL;

static uint8_t crp1231_sum(const uint8_t *frame, int len)
{
    /* Seeded with LEN, then every byte from the command through the ETX. */
    uint8_t sum = frame[1];

    for (int i = 2; i <= len - 2; i++)
        sum ^= frame[i];
    return sum;
}

static void crp1231_out(CRP1231State *r, const uint8_t *buf, int len)
{
    if (r->resp_len + len > CRP1231_RESP_MAX)
        return;
    memcpy(&r->resp[r->resp_len], buf, len);
    r->resp_len += len;
}

/* The reader answers the games' commands in one shape; only the echoed
 * command and the three status digits change. */
static void crp1231_answer(CRP1231State *r, uint8_t cmd,
                           uint8_t s3, uint8_t s4, uint8_t s5)
{
    uint8_t f[8] = { STX, 0x06, cmd, s3, s4, s5, ETX, 0 };

    f[7] = crp1231_sum(f, 8);
    memcpy(r->last_frame, f, sizeof(f));
    r->last_len = sizeof(f);
    r->have_frame = true;
}

/* The one long answer: the card's 69 bytes, in the frame the read command
 * expects. LEN is 0x4B, which is also how the game tells this answer from the
 * short one (it reads the length byte at 0x00054CD0 state 5). */
static void crp1231_answer_card(CRP1231State *r)
{
    uint8_t f[77];

    f[0] = STX;
    f[1] = 0x4B;
    f[2] = CRP1231_CMD_CARD;
    f[3] = '1';   /* a card is in the reader */
    f[4] = '0';   /* no error */
    f[5] = '0';   /* done */
    memcpy(&f[6], r->card, CRP1231_CARD_BYTES);
    f[75] = ETX;
    f[76] = crp1231_sum(f, sizeof(f));

    memcpy(r->last_frame, f, sizeof(f));
    r->last_len = sizeof(f);
    r->have_frame = true;
}

/* The card has left the reader: it is back in the player's hand, where it
 * is its file again. */
static void crp1231_card_gone(CRP1231State *r)
{
    r->card_pos = CRP1231_CARD_NONE;
    r->card_written = false;
    memset(r->card, 0, sizeof(r->card));
    r->card_path[0] = '\0';
}

/* Take up the card a file holds, a blank one for an empty path or a file
 * that does not exist yet. False when the file is not a card. */
static bool crp1231_load(CRP1231State *r, const char *path)
{
    crp1231_card_gone(r);
    if (!path || !path[0])
        return true;

    FILE *f = qemu_fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        rewind(f);
        /* 69 bytes is a card, an empty file a blank; any other size is not a
         * card and is left alone. */
        size_t n = size == CRP1231_CARD_BYTES
                       ? fread(r->card, 1, CRP1231_CARD_BYTES, f) : 0;
        fclose(f);
        if (size != 0 && n != CRP1231_CARD_BYTES) {
            fprintf(stderr, "Chihiro: not a card, %ld bytes where a card is "
                    "%d: %s\n", size, CRP1231_CARD_BYTES, path);
            memset(r->card, 0, sizeof(r->card));
            return false;
        }
        /* A blank stripe reads as nothing (crp1231lr10 FUN_00004182 finds no
         * start sentinel), so a file of zeroes is a blank; any other content
         * goes to the game untouched. */
        for (size_t i = 0; i < n; i++) {
            if (r->card[i]) {
                r->card_written = true;
                break;
            }
        }
        if (!r->card_written)
            memset(r->card, 0, sizeof(r->card));
    }
    snprintf(r->card_path, sizeof(r->card_path), "%s", path);
    return true;
}

bool crp1231_offer(CRP1231State *r, const char *path)
{
    if (!crp1231_load(r, path))
        return false;
    r->card_pos = CRP1231_CARD_ENTRY;
    /* A take-in waiting for a card draws it in the moment it covers the
     * mouth sensor (crp1231lr10 FUN_0000B2E2), and its answer is ready for
     * the game's next ENQ. */
    if (r->waiting)
        crp1231_respond(r, r->last_cmd, r->last_param);
    return true;
}

void crp1231_take_back(CRP1231State *r)
{
    if (r->card_pos == CRP1231_CARD_ENTRY)
        crp1231_card_gone(r);
}

static void crp1231_save(CRP1231State *r)
{
    /* A blank, from the dispenser or pushed in with an empty hand, becomes a
     * card of its own the first time the game writes it. */
    if (!r->card_path[0] &&
        !chihiro_card_issue(CHIHIRO_CARD_SLOT_CRP1231, r->card_path,
                            sizeof(r->card_path)))
        return;

    ChihiroFilePart part = { r->card, CRP1231_CARD_BYTES };
    chihiro_file_replace(r->card_path, &part, 1);
}

/* What the reader answers, command by command. The game's card state machine
 * (V322.xbe 0x00054CD0, state in DAT_002A7114; V307.xbe alike) leaves each
 * state on its own combination:
 *
 *   command 0x10 -> state 9     leaves on echo 0x10 and s5 = '0'
 *   command 0x78 -> state 10    leaves on echo 0x78 and s5 = '0'
 *   command 0x33 -> state 0x0C  leaves on s3 = '1' with s5 = '0'
 *   command 0x40 -> state 0x0E  leaves on '0' and '0', or on s3 = '1' with
 *                               s5 = '2'
 *   command 0x80 -> state 0x0D  leaves on s3 = s4 = s5 = '0'
 *   command 0xA0 -> state 0x11  leaves on s3 = '0' and s5 = '0'
 *   command 0xB0 -> state 0x12  leaves on s3 = '1' with s5 = '0' or '2'
 *   command 0x20 -> state 0x13  leaves on s3 = '0' or '1' with s5 = '0'
 *
 * The reader's firmware (crp1231lr10) gives each answer its meaning; s3 is
 * where the card is (CRP1231_CARD_* in the .h).
 *   0x33 takes a card in ('2' hold, '0' read too, FUN_0000592A). With none it
 *      waits, ENQ answered '0' '0' '4' (FUN_0000892C), which the card prompt
 *      (V322.xbe 0x00134680) needs; a card covering the mouth sensor is drawn
 *      in (FUN_0000B2E2), s3 = '1' (FUN_00008850) or the 69 bytes for '0'. A
 *      blank stripe reads s4 = '1' (FUN_000085EC). Outside a 0x33, a pushed
 *      card stays in the mouth.
 *   0x40 at rest is unknown, s5 = '2' (FUN_00008510); during a 0x33 it
 *      cancels the wait with '0' '0' '0' (FUN_00005C68).
 *   0x80 hands the card back, s3 = '4' until taken (FUN_0000B492); 0x20
 *      reports and moves nothing (FUN_00005310); 0xB0 feeds a blank,
 *      refused with s5 = '2' when one is inside (FUN_0000AE50). The stacker
 *      never runs out here.
 *   Write and print need the card inside, else s5 = '2'. */

/* The weekly clean. Maximum Tune 1 and 2 refuse to start once the reader has
 * gone 7 days without one, or 100 cards if so set (V322.xbe 0x00046E10,
 * V307.xbe 0x00063510), and restart the count when a clean ends: the game
 * (V322.xbe 0x0004CB00, V307.xbe 0x00068200) shows PLEASE INSERT CLEANING
 * CARD on s5 = '4', NOW CLEANING on '3', CLEANING COMPLETED on s3 = '4' with
 * '0', and ends on '0' once it has seen '3'.
 * SIMPLIFIED: there is no cleaning card to push in; the reader cleans as if
 * one had been, 1 s to take it, 3 s to clean, 1 s at the mouth. */
static void crp1231_clean(CRP1231State *r, uint8_t *s3, uint8_t *s5)
{
    int64_t t = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) - r->clean_start;

    if (t < 1000) {
        *s5 = '4';
    } else if (t < 4000) {
        *s5 = '3';
    } else if (t < 5000) {
        *s3 = '4';
    } else {
        r->cleaning = false;
    }
}

static void crp1231_status(CRP1231State *r, uint8_t cmd, uint8_t param,
                           uint8_t *s3, uint8_t *s4, uint8_t *s5)
{
    bool in = r->card_pos == CRP1231_CARD_IN;

    switch (cmd) {
    case CRP1231_CMD_CARD:
        if (param != '0' && param != '2')
            *s5 = '2';       /* a parameter the games never send */
        else if (r->waiting)
            *s5 = '4';
        else if (param == '0' && !r->card_written)
            *s4 = '1';       /* nothing on the stripe */
        break;

    case CRP1231_CMD_CANCEL:
        if (r->waiting || r->cleaning) {
            r->waiting = false;
            r->cleaning = false;
        } else {
            *s5 = '2';
        }
        break;

    case CRP1231_CMD_CLEAN:
        if (!r->cleaning) {
            r->cleaning = true;
            r->clean_start = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        }
        break;

    case CRP1231_CMD_DISPENSE:
        if (in) {
            *s5 = '2';
            break;
        }
        /* SIMPLIFIED: a card left in the mouth does not stop a real
         * dispenser, whose sensors skip the mouth (FUN_00002700), but the
         * two cards then read as an impossible pattern, which the reader
         * reports as a jam and the game turns into a fatal error. Here the
         * card in the mouth goes back to the player's hand instead. */
        if (r->card_pos == CRP1231_CARD_ENTRY)
            chihiro_card_note("Card out", false);
        crp1231_card_gone(r);
        r->card_pos = CRP1231_CARD_IN;
        break;

    case CRP1231_CMD_RETURN:
        if (r->card_pos != CRP1231_CARD_NONE)
            r->card_pos = CRP1231_CARD_GATE;
        break;

    case CRP1231_CMD_WRITE:
    case CRP1231_CMD_PRINT_TEXT:
        if (!in)
            *s5 = '2';
        break;

    default:
        break;
    }

    switch (r->card_pos) {
    case CRP1231_CARD_IN:
        *s3 = '1';
        break;
    case CRP1231_CARD_GATE:
        *s3 = '4';
        break;
    default:
        *s3 = '0';
        break;
    }

    if (r->cleaning)
        crp1231_clean(r, s3, s5);
}

/* Say where things stand, for a command or for an ENQ asking again. */
static void crp1231_respond(CRP1231State *r, uint8_t cmd, uint8_t param)
{
    /* A card handed back sits at the gate for exactly one answer: by the
     * time the host asks anything else the player has taken it. */
    if (r->card_pos == CRP1231_CARD_GATE) {
        crp1231_card_gone(r);
        chihiro_card_note("Card out", false);
    }

    r->last_cmd = cmd;
    r->last_param = param;

    /* A take-in draws in the card sitting in the mouth, and waits when
     * there is none; any other command ends the wait but the one that
     * cancels it (crp1231_status). */
    if (cmd == CRP1231_CMD_CARD && (param == '0' || param == '2')) {
        if (r->card_pos == CRP1231_CARD_ENTRY)
            r->card_pos = CRP1231_CARD_IN;
        r->waiting = r->card_pos == CRP1231_CARD_NONE;
    } else if (cmd != CRP1231_CMD_CANCEL) {
        r->waiting = false;
    }

    /* A read of a written card answers with its 69 bytes. */
    if (cmd == CRP1231_CMD_CARD && param == '0' &&
        r->card_pos == CRP1231_CARD_IN && r->card_written) {
        crp1231_answer_card(r);
        return;
    }

    uint8_t s3 = '0', s4 = '0', s5 = '0';

    crp1231_status(r, cmd, param, &s3, &s4, &s5);
    crp1231_answer(r, cmd, s3, s4, s5);
}

void crp1231_init(CRP1231State *r)
{
    memset(r, 0, sizeof(*r));
}

void crp1231_receive_byte(CRP1231State *r, uint8_t byte)
{
    /* Outside a frame the host only ever sends ENQ, asking where things
     * stand, or opens a frame with STX. Anything else is line noise. */
    if (r->cmd_len == 0) {
        if (byte == ENQ || byte == NAK) {
            /* crp1231lr10 (serial handler 0x74DC): a valid command gets an
             * ACK only; the answer goes out on the host's ENQ (state 5,
             * 0x7DE6). At rest, ENQ and NAK replay the last answer (0x8418),
             * except for a card at the gate or a clean under way: the game
             * polls ENQ while it waits, so that answer is worked out again. */
            if (!r->have_frame)
                return;
            CHIHIRO_LOGF(CARD, "CRP-1231: <- %s\n", byte == ENQ ? "ENQ" : "NAK");
            /* Still shifting the previous bytes out: a real reader does not
             * restart a transmission because the host asked again. */
            if (crp1231_has_response(r))
                return;
            if (!r->answer_pending &&
                (r->card_pos == CRP1231_CARD_GATE || r->cleaning))
                crp1231_respond(r, r->last_cmd, r->last_param);
            r->answer_pending = false;
            crp1231_out(r, r->last_frame, r->last_len);
            return;
        }
        if (byte != STX)
            return;
    }

    if (r->cmd_len >= CRP1231_CMD_MAX) {
        r->cmd_len = 0;
        r->cmd_expect = 0;
        return;
    }
    r->cmd[r->cmd_len++] = byte;

    if (r->cmd_len == 2) {
        r->cmd_expect = byte + 2;
        if (r->cmd_expect > CRP1231_CMD_MAX || r->cmd_expect < 4) {
            r->cmd_len = 0;
            r->cmd_expect = 0;
        }
        return;
    }
    if (r->cmd_expect == 0 || r->cmd_len < r->cmd_expect)
        return;

    int len = r->cmd_expect;
    uint8_t cmd = r->cmd[2];
    bool ok = r->cmd[len - 2] == ETX &&
              r->cmd[len - 1] == crp1231_sum(r->cmd, len);

    r->cmd_len = 0;
    r->cmd_expect = 0;

    CHIHIRO_LOG_HEX(CARD, r->cmd, len, "CRP-1231: <- %02X %s:", cmd,
                    ok ? "ok" : "BAD FRAME");

    if (!ok) {
        uint8_t nak = NAK;
        crp1231_out(r, &nak, 1);
        return;
    }

    /* One answer buffer: a new reply abandons what was left to shift out. */
    r->resp_len = 0;
    r->resp_pos = 0;

    uint8_t ack = ACK;
    crp1231_out(r, &ack, 1);
    r->answer_pending = true;

    /* The 69 bytes follow the three parameter digits (0x00058730). */
    if (cmd == CRP1231_CMD_WRITE && len >= 9 + CRP1231_CARD_BYTES &&
        r->card_pos == CRP1231_CARD_IN) {
        memcpy(r->card, &r->cmd[9], CRP1231_CARD_BYTES);
        r->card_written = true;
        crp1231_save(r);
        CHIHIRO_LOGF(CARD, "CRP-1231: card written to %s\n",
                     r->card_path[0] ? r->card_path : "(no file)");
    }

    crp1231_respond(r, cmd, len >= 7 ? r->cmd[6] : 0);

    CHIHIRO_LOG_HEX(CARD, r->last_frame, r->last_len,
                    "CRP-1231: -> 06, then on ENQ:");
}

bool crp1231_has_response(CRP1231State *r)
{
    return r->resp_pos < r->resp_len;
}

bool crp1231_mid_response(CRP1231State *r)
{
    return r->resp_pos > 0;
}

bool crp1231_read(CRP1231State *r, uint8_t *out)
{
    if (r->resp_pos >= r->resp_len)
        return false;

    *out = r->resp[r->resp_pos++];
    if (r->resp_pos >= r->resp_len) {
        CHIHIRO_LOGF(CARD, "CRP-1231: %d bytes delivered\n", r->resp_len);
        r->resp_pos = 0;
        r->resp_len = 0;
    }
    return true;
}
