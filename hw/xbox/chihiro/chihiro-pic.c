/*
 * Chihiro media board — the security PIC and the three-wire link to it
 *
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * The command and answer table and the default key come from MAME's
 * src/mame/sega/naomi.cpp (license LGPL-2.1+; copyright holders Samuele
 * Zannoli, R. Belmont, ElSemi, David Haywood, Angelo Salese, Olivier
 * Galibert, MetalliC).
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
 */
#include "qemu/osdep.h"
#include "chihiro-pic.h"

#define PIC_CLOCK 0x08u
#define PIC_DATA  0x07u

static const uint8_t pic_default_key[16] = {       /* "NAOMIGDROMSYSTEM" */
    'N', 'A', 'O', 'M', 'I', 'G', 'D', 'R', 'O', 'M', 'S', 'Y', 'S', 'T', 'E', 'M'
};

void chihiro_pic_reset(ChihiroPic *p)
{
    memset(p, 0, sizeof(*p));
    memcpy(p->key, pic_default_key, sizeof(p->key));
}

/* The scrambling, both ways. */
static uint8_t pic_veil(const ChihiroPic *p, uint8_t v, int n)
{
    return (uint8_t)((v ^ p->key[8 + n]) + p->key[n]);
}

static uint8_t pic_unveil(const ChihiroPic *p, uint8_t v, int n)
{
    return (uint8_t)((v - p->key[n]) ^ p->key[8 + n]);
}

/* Eight bytes become twenty-four transfers of three bits, plus one the board
 * clocks out and ignores. */
static void pic_frame(ChihiroPic *p, const uint8_t *msg)
{
    for (int i = 0; i < 8; i++) {
        uint8_t b = pic_veil(p, msg[i], i);
        uint8_t parity = 0;
        for (int j = 0; j < 8; j++) parity ^= (uint8_t)(b >> j);
        p->out[i * 3 + 0] = b & 7;
        p->out[i * 3 + 1] = (b >> 3) & 7;
        p->out[i * 3 + 2] = (uint8_t)(((b >> 6) & 3) | ((parity & 1) << 2));
    }
    p->out[24] = 0;
    p->out_n = CHIHIRO_PIC_TRANSFERS;
    p->out_i = 0;
    p->out_clk = false;
}

/* Everything this chip has to say, from naomi.cpp's table. */
static void pic_answer(ChihiroPic *p, const uint8_t *cmd)
{
    static const struct { const char *ask; const char *say; } table[] = {
        { "bsec_ver", "8VER0001" },
        { "atestpic", "7TEST_OK" },
        { "kayjyo!?", ":\x70\x1f\x71\x1f\x00\x00" },
        { "kaijyo!?", ":\x70\x1f\x71\x1f\x00\x00" },
        { "AKEYCODE", "3\x00\x00\x00\x00\x00\x00" },
        { "Bkeycode", "4\x00\x00\x00\x00\x00\x00" },
        { "C1strdf0", "5\x00\x00\x00\x00\x00\x00" },
        { "D1strdf1", "6\x00\x00\x00\x00\x00\x00" },
    };
    uint8_t say[8];

    memset(say, 0, sizeof(say));

    /* Three commands carry a new session key instead of asking anything. The
     * answer still travels under the old key; the new one takes over after. */
    if (cmd[0] == '!' || cmd[0] == '"' || cmd[0] == '#') {
        uint8_t next[16];
        memcpy(next, p->key, sizeof(next));
        if (cmd[0] == '!') {
            memcpy(next + 0, cmd + 1, 7);
            say[0] = '0'; memcpy(say + 1, "DIMMID0", 7);
        }
        if (cmd[0] == '"') {
            memcpy(next + 7, cmd + 1, 7);
            say[0] = '1'; memcpy(say + 1, "DIMMID1", 7);
        }
        if (cmd[0] == '#') {
            memcpy(next + 14, cmd + 1, 2);
            say[0] = '2'; memcpy(say + 1, "DIMMID2", 7);
        }
        pic_frame(p, say);
        memcpy(p->key, next, sizeof(p->key));
        return;
    }

    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (!memcmp(cmd, table[i].ask, 8)) {
            memcpy(say, table[i].say, 8);
            pic_frame(p, say);
            return;
        }
    }
    /* Not one we know: stay silent, which is what an absent chip does. */
    p->out_n = 0;
}

static void pic_message_in(ChihiroPic *p)
{
    uint8_t msg[8];

    for (int i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)(p->in[i * 3 + 0]
                              | (p->in[i * 3 + 1] << 3)
                              | ((p->in[i * 3 + 2] & 3) << 6));
        msg[i] = pic_unveil(p, b, i);
    }
    pic_answer(p, msg);
}

void chihiro_pic_write_dir(ChihiroPic *p, uint32_t val)
{
    bool drives = (val & 0x0F) != 0;

    if (drives && !p->board_drives) { p->in_n = 0; p->out_n = 0; p->out_i = 0; }
    p->board_drives = drives;
}

void chihiro_pic_write_lines(ChihiroPic *p, uint32_t val)
{
    bool clk = (val & PIC_CLOCK) != 0;

    if (p->board_drives && clk && !p->clk) {        /* the board clocks a group in */
        if (p->in_n < CHIHIRO_PIC_TRANSFERS) p->in[p->in_n++] = val & PIC_DATA;
        if (p->in_n == CHIHIRO_PIC_TRANSFERS) pic_message_in(p);
    }
    p->clk = clk;
}

uint32_t chihiro_pic_read_lines(ChihiroPic *p)
{
    uint32_t v;

    if (p->board_drives || p->out_i >= p->out_n) return 0;

    /* The board samples the three data lines on the rising edge of the clock,
     * so each transfer is presented low then high. */
    v = p->out[p->out_i];
    if (p->out_clk) {
        v |= PIC_CLOCK;
        p->out_clk = false;
        p->out_i++;
    } else {
        p->out_clk = true;
    }
    return v;
}
