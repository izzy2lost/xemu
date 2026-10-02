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
#ifndef CHIHIRO_CARDREADER_CRP1231_H
#define CHIHIRO_CARDREADER_CRP1231_H

#include <stdint.h>
#include <stdbool.h>

/* The longest answer is the 77-byte card read, after its ACK. The longest
 * command is LEN 0xFF plus STX and LEN: a print line (V322.xbe 0x0005BAF0
 * writes LEN = strlen + 10). */
#define CRP1231_CMD_MAX    257
#define CRP1231_RESP_MAX   128
#define CRP1231_CARD_BYTES 69

/* Where the card is, as the reader's five path sensors see it (firmware
 * crp1231lr10 FUN_00002602, reported in s3 by FUN_00009072): '0' nowhere or
 * just pushed into the mouth, '1' drawn in to the read/write position, '4'
 * handed back and waiting at the mouth to be taken. */
enum {
    CRP1231_CARD_NONE = 0,
    CRP1231_CARD_IN,
    CRP1231_CARD_GATE,
    CRP1231_CARD_ENTRY,     /* pushed into the mouth by the player */
};

/* The Maximum Tune cabinet's Sanwa CRP-1231LR-10NAB, on SC UART0 (where Ghost
 * Squad has its second HW210). ASCII framed by STX and ETX:
 *
 *   host -> reader   02 | LEN | CMD | params… | 03 | SUM
 *   reader -> host   06 (ACK), then
 *                    02 | 06 | CMD | s3 | s4 | s5 | 03 | SUM
 *
 * LEN is the frame length minus two; SUM is LEN XOR every byte from CMD
 * through ETX; a bad frame gets 15 (NAK). The game (V322.xbe 0x00057BD0,
 * state machine 0x00054CD0) reads the three digits against the command it
 * sent, so each command has its own answer (table in the .c). Everywhere:
 *   s4 is the error digit: '2' CARD R/W DATA WRITE ERROR, '3' CARD JAM, 'A'
 *      SYSTEM ERROR (operator menu 0x00053B2F), '5' kept as a code too.
 *   s5 = '3' is busy ("PLEASE WAIT A MOMENT"); s3 = '4' is "PLEASE TAKE A
 *      CARD" (health check 0x00057B80).
 *
 * The commands, with the answer length the game expects (object +0x04) and
 * the echo id it matches (+0x14); 0x78 and 0x7D only end a sequence:
 *   0x10 init            0x00057DB0   8 bytes   id 0x0D
 *   0x20 status          0x00058610   8 bytes   id 7
 *   0x33 card            0x00057E70 ('2', take a card in)   8 bytes   id 1
 *                        0x00057F30 ('0', read it)         77 bytes   id 3
 *   0x40 cancel          0x00057D20   8 bytes   id 0x0B
 *   0x53 'S' write       0x00058730   8 bytes   id 2, sends 80 bytes
 *   0x78                 0x00058AA0   8 bytes   id 0x0C, 14 bytes out
 *   0x7D                 0x0005BCB0   8 bytes   id 5
 *   0x80 return          0x000586A0   8 bytes   id 6
 *   0xA0 clean           0x00058000   8 bytes   id 0x0E
 *   0xB0 dispense        0x00058A10   8 bytes   id 8
 *
 * 0x7C (0x0005BAF0 and four more) prints a line of text on the card's face;
 * 0x7D is sent right after a read; 0x10 is the boot check's first word.
 *
 * A card is 69 bytes both ways: the write command carries them after three
 * parameter digits, and the 77-byte answer to 0x33 '0' carries them between
 * the status digits and the ETX (0x00058090 copies 0x45 of them into the game
 * object at +0x18). 0x0006EE00 is the encoder that builds them.
 */
typedef struct CRP1231State {
    uint8_t cmd[CRP1231_CMD_MAX];       /* the frame, whole until the next STX */
    int     cmd_len;        /* bytes gathered so far */
    int     cmd_expect;     /* full frame length, 0 until LEN arrives */

    uint8_t resp[CRP1231_RESP_MAX];
    int     resp_len;
    int     resp_pos;

    uint8_t last_frame[80]; /* the answer, sent when the host asks with ENQ */
    int     last_len;
    bool    have_frame;
    bool    answer_pending; /* built for the last command, not yet sent */

    /* The card held: the 69 bytes the game wrote (its encoder 0x0006EE00
     * checksums them). The card is its file; a blank has none until then. */
    uint8_t card[CRP1231_CARD_BYTES];
    int     card_pos;       /* CRP1231_CARD_*: where the card is */
    bool    card_written;   /* and whether it has data on it */
    char    card_path[512];
    bool    waiting;        /* a take-in command is waiting for a card */

    uint8_t last_cmd;       /* what an ENQ is asking about again */
    uint8_t last_param;
    bool    cleaning;       /* a clean (0xA0) under way */
    int64_t clean_start;    /* when it began, in virtual ms */
} CRP1231State;

void    crp1231_init(CRP1231State *r);

/* The player pushes the card from this file, or a blank one when the path
 * is empty, into the reader's mouth. A reader waiting for a card draws it
 * in at once; otherwise it stays in the mouth until the game asks for one.
 * False, with nothing pushed, when the file is not a card. */
bool    crp1231_offer(CRP1231State *r, const char *path);

/* The player takes back the card still sitting in the mouth. */
void    crp1231_take_back(CRP1231State *r);

void    crp1231_receive_byte(CRP1231State *r, uint8_t byte);
bool    crp1231_has_response(CRP1231State *r);
bool    crp1231_read(CRP1231State *r, uint8_t *out);

/* True once the reader has started sending the answer it owes, so the link
 * can tell a fresh turnaround from a byte already on its way. */
bool    crp1231_mid_response(CRP1231State *r);

/* True when this cabinet has a CRP-1231 and the user left the card reader
 * switched on. */
extern bool chihiro_crp1231_enabled;

extern CRP1231State *chihiro_crp1231_global;

#endif
