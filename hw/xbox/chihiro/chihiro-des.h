/*
 * DES as the Sega media boards use it
 *
 * Copyright (c) Olivier Galibert (MAME, src/mame/sega/naomigd.cpp)
 * Copyright (c) 2026 Réda Chérif-Touil (the C port)
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * The full notice is in chihiro-des.c.
 */
#ifndef CHIHIRO_DES_H
#define CHIHIRO_DES_H

#include <stdint.h>
#include <stdbool.h>

/* Decrypts a media board firmware image in place: 8-byte blocks, ECB, the
 * block read and written little-endian. Length is rounded down to a multiple
 * of eight. */
void chihiro_des_decrypt_buffer(uint8_t *buf, uint32_t len, uint64_t key);

#endif
