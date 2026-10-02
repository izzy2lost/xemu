/*
 * Tamura (SAXA) HW210 IC card reader (see chihiro-cardreader-hw210.h)
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chihiro-cardreader-hw210.h"
#include "chihiro-log.h"
#include "chihiro.h"

static uint8_t compute_checksum(const uint8_t *buf, int len)
{
    uint8_t chk = 0;
    for (int i = 0; i < len; i++)
        chk ^= buf[i];
    return chk;
}

static void build_response(CardReaderState *s, uint8_t cmd)
{
    memset(s->tx_buf, 0, sizeof(s->tx_buf));
    s->tx_buf[0] = 0x10;
    s->tx_buf[1] = cmd;

    uint16_t payload_len = 0;

    switch (cmd) {
    case 0x10: /* INIT */
        payload_len = 0x02;
        s->tx_buf[5] = 0x20;
        break;

    case 0x11: /* GET STATUS / RESET */
        payload_len = 0x02;
        s->tx_buf[5] = 0x20;
        break;

    case 0x14: /* reader init, second step (vsg.xbe FUN_000d2a50) */
    case 0x15: /* STANDBY */
    case 0x26:
    case 0x27:
        payload_len = 0x02;
        break;

    case 0x20: /* DETECT CARD */
        payload_len = 0x02;
        if (!s->card_present)
            s->tx_buf[4] = 0x80;
        break;

    case 0x21: /* GET CARD UID */
        payload_len = 0x0A;
        s->tx_buf[6] = 0x81;
        s->tx_buf[8] = 0x59;
        s->tx_buf[9] = 0xDA;
        break;

    case 0x22: /* GET CARD TYPE */
        payload_len = 0x0A;
        s->tx_buf[7] = 0x08;
        break;

    case 0x33: /* GET CAPACITY */
        payload_len = 0x0A;
        s->tx_buf[6] = 0xFF;
        s->tx_buf[7] = 0xFF;
        break;

    case 0x34: { /* READ BLOCKS */
        uint16_t block_start = (s->rx_buf[6] << 8) | s->rx_buf[7];
        uint16_t block_count = (s->rx_buf[8] << 8) | s->rx_buf[9];
        uint32_t byte_offset = block_start * CARD_BLOCK_SIZE;
        uint32_t byte_count = block_count * CARD_BLOCK_SIZE;
        if (byte_offset + byte_count <= CARD_TOTAL_SIZE &&
            byte_count + 6 < sizeof(s->tx_buf)) {
            payload_len = byte_count + 2;
            memcpy(&s->tx_buf[6], &s->card_data[byte_offset], byte_count);
        } else {
            payload_len = 0x02;
            s->tx_buf[4] = 0x80;
            CHIHIRO_LOGF(CARD, "HW210: read past the card (block %u, %u "
                         "blocks)\n", block_start, block_count);
        }
        break;
    }

    case 0x35: { /* WRITE BLOCKS */
        uint16_t block_start = (s->rx_buf[6] << 8) | s->rx_buf[7];
        uint16_t block_count = (s->rx_buf[8] << 8) | s->rx_buf[9];
        uint32_t byte_offset = block_start * CARD_BLOCK_SIZE;
        uint32_t byte_count = block_count * CARD_BLOCK_SIZE;
        if (byte_offset + byte_count <= CARD_TOTAL_SIZE &&
            byte_count + 10 <= (uint32_t)sizeof(s->rx_buf)) {
            memcpy(&s->card_data[byte_offset], &s->rx_buf[10], byte_count);
            s->dirty = true;
        }
        payload_len = 0x02;
        break;
    }

    default:
        payload_len = 0x02;
        break;
    }

    s->tx_buf[2] = (uint8_t)(payload_len >> 8);
    s->tx_buf[3] = (uint8_t)(payload_len & 0xFF);
    s->tx_len = payload_len + 5;
    s->tx_pos = 0;
    s->tx_buf[s->tx_len - 1] = compute_checksum(s->tx_buf, s->tx_len - 1);
}

void card_reader_init(CardReaderState *s)
{
    memset(s, 0, sizeof(*s));
}

void card_reader_insert(CardReaderState *s, const char *path,
                        const CardStock *stock)
{
    if (!path || !path[0]) return;

    snprintf(s->card_path, sizeof(s->card_path), "%s", path);

    FILE *f = qemu_fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        rewind(f);
        size_t n = size == CARD_TOTAL_SIZE
                       ? fread(s->card_data, 1, CARD_TOTAL_SIZE, f) : 0;
        fclose(f);
        if (n == CARD_TOTAL_SIZE) {
            s->card_present = true;
            s->dirty = false;
            return;
        }
        /* Any other size is not a card: taking it in would overwrite it with
         * a 2 KB card. Only an empty file, or none, is a blank. */
        if (size != 0) {
            fprintf(stderr, "Chihiro: not a card, %ld bytes where a card is "
                    "%d: %s\n", size, CARD_TOTAL_SIZE, path);
            s->card_path[0] = '\0';
            return;
        }
    }

    /* Missing or empty file: a fresh card from the cabinet's stock, which the
     * game takes as NEW (an all-zero card fails its header check). */
    memset(s->card_data, 0, CARD_TOTAL_SIZE);
    if (stock) {
        memcpy(&s->card_data[CARD_STOCK_OFFSET], stock->header,
               CARD_STOCK_BYTES);
        if (stock->numbered) {
            /* HEURISTIC: the card number is its creation time in
             * microseconds; the game only asks that no two cards share it. */
            uint64_t number = (uint64_t)g_get_real_time();
            memcpy(&s->card_data[0], &number, sizeof(number));
        }
    }
    s->card_present = true;
    s->dirty = true;
    card_reader_flush(s);
    CHIHIRO_LOGF(CARD, "HW210: fresh card written to %s\n", path);
}

void card_reader_remove(CardReaderState *s)
{
    if (s->dirty)
        card_reader_flush(s);
    s->card_present = false;
    s->rx_pos = 0;
    s->rx_expected = 0;
    s->tx_len = 0;
    s->tx_pos = 0;
}

void card_reader_write_byte(CardReaderState *s, uint8_t byte)
{
    /* A command opens with 0x00; the reader answers with 0x10. */
    if (s->rx_pos == 0 && byte != 0x00)
        return;

    if (s->rx_pos == 1 && byte == 0x00) {
        s->rx_pos = 0;
        return;
    }

    if (s->rx_pos >= (int)sizeof(s->rx_buf))
        s->rx_pos = 0;

    s->rx_buf[s->rx_pos++] = byte;

    if (s->rx_pos == 4) {
        s->rx_expected = ((s->rx_buf[2] << 8) | s->rx_buf[3]) + 5;
        if (s->rx_expected > (int)sizeof(s->rx_buf))
            s->rx_expected = sizeof(s->rx_buf);
    }

    if (s->rx_pos >= 5 && s->rx_pos == s->rx_expected) {
        build_response(s, s->rx_buf[1]);
        CHIHIRO_LOGF(CARD, "HW210: <- %02X (%d bytes) -> status %02X%02X, "
                     "%d bytes\n", s->rx_buf[1], s->rx_expected,
                     s->tx_buf[4], s->tx_buf[5], s->tx_len);

        s->rx_pos = 0;
        s->rx_expected = 0;

        if (s->dirty)
            card_reader_flush(s);
    }
}

int card_reader_read(CardReaderState *s, uint8_t *buf, int max_len)
{
    int avail = s->tx_len - s->tx_pos;
    if (avail <= 0) return 0;

    int n = avail < max_len ? avail : max_len;
    memcpy(buf, &s->tx_buf[s->tx_pos], n);
    s->tx_pos += n;
    return n;
}

bool card_reader_has_response(CardReaderState *s)
{
    return s->tx_pos < s->tx_len;
}

bool card_reader_flush(CardReaderState *s)
{
    /* A card a snapshot brought back has no file: it becomes a card of its
     * own the first time it is written. With no card in the slot there is
     * nothing to keep. */
    if (!s->card_path[0] && !s->card_present) {
        s->dirty = false;
        return true;
    }
    if (!s->card_path[0] &&
        !chihiro_hw210_card_issue(s, s->card_path, sizeof(s->card_path)))
        return false;

    ChihiroFilePart part = { s->card_data, CARD_TOTAL_SIZE };
    if (!chihiro_file_replace(s->card_path, &part, 1)) return false;
    s->dirty = false;
    return true;
}
