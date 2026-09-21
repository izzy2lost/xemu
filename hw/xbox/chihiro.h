#ifndef HW_XBOX_CHIHIRO_H
#define HW_XBOX_CHIHIRO_H

#include "system/block-backend.h"

/* Forward declaration */
typedef struct USBDevice USBDevice;

/*
 * True when the machine is configured as a Sega Chihiro arcade board.
 * This is the single switch the Chihiro-specific paths key off; it is a
 * dedicated setting rather than "RAM == 128 MiB" so that plain 128 MiB
 * debug/homebrew configurations keep the stock Xbox behaviour.
 */
bool xbox_is_chihiro(void);

/* Sectors in the emulated 512 MiB mediaboard DIMM. The mediaboard
 * reports this size over LPC port 0x40F4, and the kernel derives the
 * mbcom sector from it, so the disc geometry has to agree. */
#define CHIHIRO_DIMM_SECTORS 0x100000ULL

/* Tell the Chihiro IDE hook how big the real backing image is, so reads
 * past it (up to the padded DIMM geometry) can be served as zeros. */
void chihiro_set_disc_sectors(uint64_t sectors);
extern uint64_t chihiro_disc_sectors;

/* mbcom IDE hooks — called from IDE DMA path */
void chihiro_mbcom_init(void);
bool chihiro_ide_read_sector(uint32_t lba, void *buffer);
bool chihiro_ide_write_sector(uint32_t lba, const void *buffer);
void chihiro_ide_dma_write_done(BlockBackend *blk, int64_t sector_num);

/* USB delayed hotplug (AN2131 firmware boot simulation) */
void chihiro_usb_set_devices(USBDevice *qc, USBDevice *sc);

/* Load baseboard flash ROM (SEGABOOT) from file */
void chihiro_load_flash_rom(const char *bios_path);

/* Called from SMC when SCRATCH=0x04 (QuickReboot signal) */
void chihiro_on_quickreboot_signal(void);

/* Called from SMC POWER handler to detect QuickReboot */
bool chihiro_intercept_reset(void);

#endif
