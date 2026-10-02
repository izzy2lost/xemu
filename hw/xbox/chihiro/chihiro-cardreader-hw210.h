/*
 * Tamura (SAXA) HW210 IC card reader (Ghost Squad; Gundam B.O.S. carries one
 * of the same family, on the same wire, speaking the same frames)
 *
 * The cabinet's card drawer is the Sega CTF-1150 "IC RW UNIT". Its parts
 * list, in the Ghost Squad Deluxe operator manual, holds:
 *     839-1204-02   SERIAL I/F BD CTF
 *     601-11132     IC CARD READER HW210 (TAMURA)
 *     610-0680      SOLENOID UNIT
 *     370-5161      PHOTO INTERRUPTER GP1A71A
 * The DX cabinet carries two ("PLAYER 1/2 CARD SLOT"). MAME calls the reader
 * "SAXA HW210" (naomi.cpp); no firmware dump exists. It is not the Sanwa
 * CRP-1231 of Maximum Tune. The protocol here was read off the SC firmware
 * and the games.
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
#ifndef CHIHIRO_CARDREADER_HW210_H
#define CHIHIRO_CARDREADER_HW210_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define CARD_BLOCK_SIZE 8
#define CARD_TOTAL_SIZE 2048

/* A fresh card carries the factory header the game validates in blocks 4 to
 * 6, which it never writes; the cabinet table (chihiro.c) says what it holds.
 * Everything else is zero. */
#define CARD_STOCK_OFFSET 32
#define CARD_STOCK_BYTES  24

typedef struct {
    uint8_t header[CARD_STOCK_BYTES];  /* blocks 4 to 6 of a fresh card */
    /* Whether the maker numbers each card in block 0. Gundam B.O.S. keys
     * the repair cache it keeps in the cabinet's backup on those eight
     * bytes (gs.xbe FUN_000658b0), so two cards with the same block 0 are
     * one card to it. Ghost Squad's cards carry zeros there. */
    bool    numbered;
} CardStock;

typedef struct {
    /* A full-card WRITE: 10 header bytes, 2048 of data and a checksum. */
    uint8_t rx_buf[CARD_TOTAL_SIZE + 32];
    int rx_pos;
    int rx_expected;

    uint8_t tx_buf[2048 + 8];
    int tx_len;
    int tx_pos;

    bool card_present;
    uint8_t card_data[CARD_TOTAL_SIZE];
    bool dirty;
    char card_path[512];
} CardReaderState;

void card_reader_init(CardReaderState *s);
void card_reader_insert(CardReaderState *s, const char *path,
                        const CardStock *stock);
void card_reader_remove(CardReaderState *s);
void card_reader_write_byte(CardReaderState *s, uint8_t byte);
int  card_reader_read(CardReaderState *s, uint8_t *buf, int max_len);
bool card_reader_has_response(CardReaderState *s);
bool card_reader_flush(CardReaderState *s); /* true once the file has it */

/* The two readers (chihiro.c), driven by the SC 8051's UARTs:
 * [0] on MIDI/UART1, [1] on RS-232C/UART0. */
extern CardReaderState *chihiro_card_reader_global;
/* A new card file, in the reader's slot, for a card that has none (chihiro.c). */
bool chihiro_hw210_card_issue(const CardReaderState *s, char *out,
                              size_t out_len);
/* This cabinet carries HW210 slots and the card reader is switched on; the
 * SC MIDI channel (UART1) then belongs to the reader, not a drive board. */
extern bool chihiro_hw210_enabled;

#endif
