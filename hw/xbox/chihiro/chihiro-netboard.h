/*
 * Chihiro Type-3 network board (Sega 837-14341): an AMD Alchemy Au1500
 * running the "Netfirm" firmware, on a PCI bus of its own with the media
 * board's bridge as the only device
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
#ifndef HW_CHIHIRO_NETBOARD_H
#define HW_CHIHIRO_NETBOARD_H

#include <stdint.h>
#include <stdbool.h>

/* Brings the board up from its firmware image (the 2 MB flash, or the 1 MB
 * half of it that dumps carry); its thread starts on first contact (a
 * doorbell, the window, or a frame). Returns false, and leaves no board, when
 * there is no image. */
/* serial: the media board's 16 characters; ip: the cabinet's address as
 * the dotted quad a.b.c.d packed a | b << 8 | c << 16 | d << 24. */
bool chihiro_netboard_init(const char *firmware_path, const char serial[16],
                           uint32_t ip);
void chihiro_netboard_exit(void);
/* Puts the board's MAC0 on xemu's network hub; call once, on the main loop. */
void chihiro_netboard_attach_net(void);
bool chihiro_netboard_present(void);

/* The bridge, as the Xbox side sees it. A command packet is 8 words; the
 * board answers in its own time and rings back through the callback. */
/* The slots are 512 bytes each in the board's SDRAM (0x600200 in, 0x600000
 * out): the 32-byte packet first, then whatever bulk data the command
 * carries. `len` is what the host wrote or wants, up to 512. */
void chihiro_netboard_host_command(const void *data, int len);  /* deposits it */
void chihiro_netboard_host_command_slot(void *data, int len);   /* reads it back */
void chihiro_netboard_host_ring(void);                          /* the doorbell */
void chihiro_netboard_host_response(void *data, int len);
void chihiro_netboard_host_release(void);          /* the response slot read */
void chihiro_netboard_host_ack(void);              /* the Xbox acknowledged */
/* The host-bus window: 1 MB of the board's SDRAM from 0x600000, which the
 * Xbox reaches as IDE sectors 0x9008000..; through the LPC window only the
 * two 8-word packets at 0x91000000/0x91000200 are modelled. The two slots
 * above are its first 1 KB; bulk data (an address string, a sockaddr, a
 * packet) sits further in, at the offsets the command's arguments name. */
#define CHIHIRO_NETBOARD_WINDOW_SIZE 0x100000u
void chihiro_netboard_host_window_write(uint32_t off, const void *data, int len);
void chihiro_netboard_host_window_read(uint32_t off, void *data, int len);
/* Called on the board's thread when it rings the host; the callee raises
 * the Xbox interrupt. */
void chihiro_netboard_set_host_interrupt(void (*fn)(void));

#endif
