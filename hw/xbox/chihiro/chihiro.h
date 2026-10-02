/*
 * QEMU Chihiro emulation
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
#ifndef HW_XBOX_CHIHIRO_H
#define HW_XBOX_CHIHIRO_H

#include "system/block-backend.h"
#include "system/dma.h"

/* Forward declaration */
typedef struct USBDevice USBDevice;

/* MemoryRegion-backed IDE interface */
void chihiro_ide_interface_init(void);
void chihiro_ide_load_rom(void);
uint8_t *chihiro_fatx_get_buffer(uint32_t *out_size);
bool chihiro_ide_serve(int dma_cmd, uint32_t lba, int n,
                       QEMUSGList *sg, bool *irq);

/* USB delayed hotplug (AN2131 firmware boot simulation) */
void chihiro_usb_set_devices(USBDevice *qc, USBDevice *sc);

/* True if the file is the Chihiro BIOS listed by MAME. */
bool chihiro_bios_known(const char *path);

/* Load the media board flash (SEGABOOT) from file */
void chihiro_load_flash_rom(const char *bios_path);
bool chihiro_flash_rom_loaded(void);

/* The media board flash as the V850 sees it ("MBDT" at 0xFFE00, the board
 * serial at 0xFFE10). */
const uint8_t *chihiro_flash_rom_bytes(uint32_t *size);

/* Why the last DIMM snapshot save or load failed, NULL when it did not:
 * the vmstate hooks can only return an errno, the UI wants words. */
const char *chihiro_dimm_last_error(void);
/* Forgets it, before a new save or load. */
void chihiro_dimm_clear_error(void);
/* Size and CRC32 of the mounted netboot image as the DIMM delta records
 * them; false when the image cannot be read. */
bool chihiro_dimm_image_identity(uint64_t *size, uint32_t *crc);

/* Load the baseboard EEPROMs (ic10, ic11, pc20): the configured paths first,
 * then the BIOS directory */
void chihiro_load_eeproms(const char *bios_path);
extern uint8_t *chihiro_ic10_data;
extern uint32_t chihiro_ic10_size;
extern uint8_t *chihiro_ic11_data;
extern uint32_t chihiro_ic11_size;
extern uint8_t *chihiro_pc20_data;
extern uint32_t chihiro_pc20_size;

/* The card slots a cabinet can have. */
typedef enum {
    CHIHIRO_CARD_SLOT_CRP1231,   /* Maximum Tune */
    CHIHIRO_CARD_SLOT_GUNDAM,
    CHIHIRO_CARD_SLOT_HW210_P1,  /* Ghost Squad */
    CHIHIRO_CARD_SLOT_HW210_P2,
    CHIHIRO_CARD_SLOTS
} ChihiroCardSlot;

/* Issues a blank card file in <data>/cards/ for the slot, assigned at the
 * UI's next frame; false when the cabinet issues none or it cannot be made. */
bool chihiro_card_issue(ChihiroCardSlot slot, char *out, size_t out_len);

/* The UI thread's side: assigns the cards issued since its last frame and
 * announces them. Called with the big lock held. */
void chihiro_card_ui_sync(void);

/* A line for the player from a card reader, in the warning colour when
 * something did not happen. Called with the big lock held. */
void chihiro_card_note(const char *msg, bool warning);

/* The Card In keys: the card in hand, or a blank, goes into a slot. Called
 * with the big lock held. */
void chihiro_hw210_card_key(int player);
void chihiro_crp1231_card_key(void);
/* Whether a card sits in a HW210 slot (the slot's insertion switch). */
bool chihiro_card_reader_present(int player);

/* Game state */
extern char chihiro_game_dir[1024];
extern char chihiro_game_filename[64];

#include "chihiro-cabinet.h"
/* Sega netboot boot.id: the bytes every source of the game name parses. */
#define CHIHIRO_BOOTID_LEN 0xC0
bool chihiro_bootid_executable(const uint8_t *bid, char *out, size_t out_len);
void chihiro_set_game_executable(const char *name);

/* The media board this machine presents, settled once the game is named (on
 * Auto; the setting otherwise). */
bool chihiro_is_type3(void);
/* A message the Type-3 board posted outside an exchange: a late answer, or a
 * command of its own; on the main loop. */
void chihiro_dimm_board_posted(const uint32_t *msg8);
/* The byte at EEPROM 0x1F00: 1 Japan, 2 USA, 3 Export. */
uint8_t chihiro_region_byte(void);
extern bool chihiro_freeplay_setting;
unsigned chihiro_dimm_factor(void);
int chihiro_detected_game_profile(void);
/* Set when SEGABOOT hands over to the game, cleared by a QuickReboot. */
extern bool chihiro_game_running;

/* Called from OHCI when bus starts */
void chihiro_on_ohci_bus_start(void);
void chihiro_on_ohci_bus_stop(void);
uint32_t chihiro_va_to_pa(uint32_t va);

/* Called from SMC when SCRATCH=0x04 (QuickReboot signal) */
void chihiro_on_quickreboot_signal(void);

/* Save file persistence: ic11 + extmem backup area */
bool chihiro_usb_save_load(const char *path);
bool chihiro_usb_save_flush(const char *path);
bool chihiro_usb_save_dirty(void);
const uint8_t *chihiro_usb_backup_live(void);
/* The 64 KB backup half a save file holds; false when it is not one. */
bool chihiro_usb_save_read_backup(const char *path, uint8_t *backup);
/* The four-character identifier of the game that wrote a save file. */
bool chihiro_usb_save_owner(const char *path, uint8_t *owner);
/* The Gundam test menu's CARD REPAIR, done to the named card file; `why`
 * gets a sentence either way. */
bool chihiro_gundam_card_repair(const char *card_path, char *why, size_t why_len);
void chihiro_save_init(void);
/* Writes the saves now, for the UI's quick exit, which skips the exit
 * notifiers. */
void chihiro_flush_save_now(void);

typedef struct ChihiroFilePart {
    const void *data;
    size_t size;
} ChihiroFilePart;

/* Writes the parts to <path>.tmp and renames it over <path> once every write
 * succeeded, so a crash of xemu or a full disk leaves the previous file whole
 * (no sync to the disk: after a power cut the file may still be lost). */
bool chihiro_file_replace(const char *path, const ChihiroFilePart *parts,
                          int count);

#endif
