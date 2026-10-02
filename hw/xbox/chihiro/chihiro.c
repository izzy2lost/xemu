/*
 * QEMU Chihiro emulation
 *
 * Copyright (c) 2013 espes
 * Copyright (c) 2018-2021 Matt Borgerson
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
#include <glib/gstdio.h>
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/isa/isa.h"
#include "hw/boards.h"
#include "system/memory.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#define TS_MS ((long long)(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)))
#include "chihiro-log.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "chihiro.h"
#include "chihiro-asic.h"
#include "system/blockdev.h"
#include "system/system.h"
#include "block/blkmemory.h"
#include "block/block-global-state.h"
#include "block/block_int-global-state.h"
#include "migration/vmstate.h"
#include <zlib.h>
#include "qemu/main-loop.h"
#include "hw/usb.h"
#include "ui/xemu-settings.h"
#include "target/i386/cpu.h"
#include "exec/watchpoint.h"
#include "chihiro-jvs.h"
#include "chihiro-cardreader-hw210.h"
#include "chihiro-cardreader-crp1231.h"
#include "chihiro-driveboard-v257.h"
#include "chihiro-netboard.h"
#include "ui/xemu-notifications.h"

bool xbox_is_chihiro(void)
{
    return g_config.sys.chihiro;
}

/* The game to load: sys.files.dvd_path, a netboot FATX image or a game
 * folder, fixed for the session like upstream-chihiro's. */
const char *xemu_chihiro_image(void)
{
    static char *image;
    if (!image) {
        const char *path = g_config.sys.files.dvd_path;
        image = g_strdup(path ? path : "");
    }
    return image;
}

/*
 * Chihiro Mediaboard LPC I/O
 *
 * The Chihiro baseboard exposes a set of I/O registers at 0x4000-0x40FF
 * on the LPC/ISA bus. These are used by SEGABOOT to detect the baseboard,
 * query firmware version, DIMM size, and board type.
 *
 * Register map (from MAME chihiro.cpp + CXBX MediaBoard.cpp + RE of 0x3DF40):
 *   0x1E: SEGABOOT: DIMM base low word | Game: "XB" (0x4258) for XBAM check
 *   0x20: SEGABOOT: DIMM base high word | Game: "AM" (0x4D41) for XBAM check
 *   0x22: XBAM string "BX" (0x4258) — checked by SEGABOOT
 *   0x24: XBAM string "MA" (0x4D41) — checked by SEGABOOT
 *   0xE0: IRQ10 acknowledge (write clears IRQ10)
 *   0xF0: chip ID in the high byte (FPGA = Type-1, ASIC = Type-3), revision
 *         in the low byte
 *   0xF4: DIMM size (0=128M, 1=256M, 2=512M, 3=1024M)
 *
 * SEGABOOT checks "XBAM" at 0x4022-0x4024 and uses 0x401E/0x4020 for DIMM
 * base address. Game XBE checks "XBAM" at 0x401E/0x4020 instead.
 *
 * WORKAROUND: the two readers are told apart by counting reads of 0x401E
 * (first read answers the DIMM base, later ones answer "XBAM") rather than by
 * anything the hardware exposes. Real hardware has no such counter.
 */

#define SEGA_DIMM_BASE_LO                   0x1E
#define SEGA_XBAM_STRING_0                  0x20
#define SEGA_XBAM_STRING_1                  0x22
#define SEGA_XBAM_STRING_2                  0x24
#define SEGA_IRQ10_ACK                      0xE0
#define SEGA_CHIP_REVISION                  0xF0
#   define SEGA_CHIP_REVISION_CHIP_ID            0xFF00
#       define SEGA_CHIP_REVISION_FPGA_CHIP_ID      0x0000
#       define SEGA_CHIP_REVISION_ASIC_CHIP_ID      0x0100
#   define SEGA_CHIP_REVISION_REVISION_ID_MASK   0x00FF
#define SEGA_DIMM_SIZE                      0xF4
#   define SEGA_DIMM_SIZE_128M                  0
#   define SEGA_DIMM_SIZE_256M                  1
#   define SEGA_DIMM_SIZE_512M                  2
#   define SEGA_DIMM_SIZE_1024M                 3

/* mbcom command IDs — acMediaCmd names from acLib SDK (GXTX/GXTX).
 * Full command map: 0x001-0x0FF init/events, 0x100-0x1FF info queries,
 * 0x200-0x2FF unknown, 0x300-0x3FF tests, 0x400-0x4FF network sockets,
 * 0x500-0x7FF unknown groups. */
#define MB_CMD_INIT                 0x0001  /* acMediaCmd_InitAsync — returns DIMM size */
#define MB_CMD_STATUS               0x0100  /* boot phase + completion% */
#define MB_CMD_GET_VERSION          0x0101  /* acMediaCmd_GetVersionAsync — fw version */
#define MB_CMD_SYSTEM_TYPE          0x0102  /* board_type | fw_ver<<8 */
#define MB_CMD_GET_SERIAL           0x0103  /* acMediaCmd_GetSerialIdAsync */
#define MB_CMD_GET_NET_PROPERTY     0x0104  /* acMediaCmd_GetNetworkPropertyAsync */
#define MB_CMD_HARDWARE_TEST        0x0301  /* writes "TEST OK" to result ptr */

#define MB_STATUS_READY             5

/* Media board state — single source of truth for all mbcom responses.
 * On real hardware: jumpers (JP1/JP2) + firmware on the media board provide
 * these values. The kernel reads DIMM factor via port 0x40F4, and the media
 * board firmware responds to mbcom commands using the same underlying state.
 * Serial comes from flash ROM MBDT header at 0xFFE10.
 * See: https://newastrocity.wordpress.com/2013/08/04/sega-chihiro/ (jumpers)
 *      GXTX: "there's a IO port which when queried returns the jumpers" */
static struct {
    uint8_t  dimm_factor;    /* 0=128M, 1=256M, 2=512M, 3=1024M (JP1/JP2 jumpers) */
    uint32_t dimm_size;      /* computed: 0x08000000 << factor (bytes) */
    uint16_t fw_version;     /* firmware version reported by mbcom 0x0101 */
    uint8_t  board_type;     /* 0=NAOMI, 3=GD-ROM, 4=Chihiro */
    uint8_t  status;         /* boot phase: 0-4=loading, 5=READY */
    uint8_t  progress;       /* loading completion: 0-100 */
    char     serial[17];     /* from flash ROM MBDT+0x10, or "0000000000000000" */
    uint32_t net_ip;         /* the cabinet's IPv4, a | b<<8 | c<<16 | d<<24:
                              * 10.0.0.<cabinet> (0x0100000A for 10.0.0.1) */
} mediaboard;

static void mediaboard_init(void);

/* The board's address before the game gives it one (mbcom 0x0415, 0x0608):
 * 10.0.0.<cabinet>, from which the network board makes its MAC address. */
static uint32_t chihiro_cabinet_ip(void)
{
    int cabinet = g_config.chihiro.link.enable ? g_config.chihiro.link.cabinet : 1;
    if (cabinet < 1 || cabinet > 4) cabinet = 1;
    return 0x0000000A | ((uint32_t)cabinet << 24);
}

/* #define DEBUG_CHIHIRO */

typedef struct ChihiroLPCState {
    ISADevice dev;
    MemoryRegion ioport;

    bool host_seen;   /* the guest has talked to us at least once */
    uint32_t lpc_reg_addr;        /* MediaBoard register address (set via port 0x4004) */

    /* IRQ10 for baseboard → SEGABOOT communication */
    qemu_irq irq10;
    QEMUTimer *irq10_timer;

    /* USB hotplug timers (simulates staggered AN2131 I2C firmware boot) */
    QEMUTimer *usb_hotplug_timer;     /* QC, 50 ms after each BUS START */
    QEMUTimer *usb_hotplug_sc_timer;  /* SC, 100 ms after each BUS START */
    QEMUTimer *diag_arm_timer;   /* arms the periodic tick */
    bool diag_armed;

    /* The periodic tick (chihiro_diag_timer_cb): SEGABOOT's state before the
     * game, then the cabinet's boards */
    QEMUTimer *diag_timer;

    /* LPC port state kept between accesses */
    uint32_t lpc_401e_reads;   /* reads of port 0x401E: DIMM base, then "XB" */
    uint16_t lpc_scratch_4026;    /* Port 0x4026 read-write scratch register */
    uint8_t  mbcom_e0_status;     /* Port 0x40E0 interrupt source: bit0=ASIC (0x29),
                                   * bit2=Ether/NetDIMM (0xA9) — per acLib */
    bool     mbcom_resp_ready;    /* a reply waits for the next E1=0 after an arm */

    /* Baseboard DMA register state (indirect access via 0x4004/0x4000) */
    uint32_t bb_reg_addr;       /* 0xA0000020: indirect address pointer */
    uint32_t bb_reg_status;     /* 0xA0000040: DMA status/enable */
    bool     bb_dma_active;     /* true when 0xA0000040 bit31 set (burst mode) */
    uint32_t bb_dma_count;      /* dwords written in current burst */

    /* Type-3 ASIC control registers (written by SEGABOOT after firmware upload) */
    uint32_t asic_cpu_ctrl;     /* 0x80000140: ASIC CPU start/ready latch */

    /* The firmware.asic the game copies to 0x84800000, word by word. */
    uint8_t *fw_upload;         /* allocated on the first word, FW_UPLOAD_SIZE */
    uint32_t fw_upload_hi;      /* highest byte offset written, + 4 */

    /* DIMM board mailbox: commands at 0x84000020, responses at 0x84000000 */
    uint32_t dimm_cmd[8];      /* 8-dword command block written by SEGABOOT */
    uint32_t dimm_resp[8];     /* 8-dword response block read by SEGABOOT */
    QEMUTimer *dimm_resp_timer;  /* delayed IRQ10 after execute trigger (Type-3) */

    /* Migration shim for the machine-wide latches (chihiro_game_running &
     * co.): pre_save copies the globals here so they are serialized with
     * the device; post_load restores them. Without this a load into a
     * fresh session leaves game_running=false and the SEGABOOT IRQ10
     * poll keeps firing into a running game — harmless while the
     * mediaboard is idle, fatal while it is streaming (cinematics). */
    bool mig_game_running;
    /* The game's name picks the cabinet (card reader, drive board, input),
     * so a snapshot carries it. */
    uint8_t mig_game_filename[64];
    bool mig_active;
} ChihiroLPCState;

#define CHIHIRO_LPC_DEVICE(obj) \
    OBJECT_CHECK(ChihiroLPCState, (obj), "chihiro-lpc")

static bool chihiro_active;
bool chihiro_game_running;  /* the game has taken over: no SEGABOOT DMA scan */
static int game_mode_bus_starts;  /* BUS START count since game_running became true */
static void chihiro_patch_running_game(void);
static bool chihiro_mbcom_bootstrap_done; /* Reset on QuickReboot so game gets fresh DIMM_SIZE */
static bool chihiro_e1_armed; /* Reset on QuickReboot to prevent premature response delivery */
char chihiro_game_filename[64]; /* set by chihiro_set_game_executable */
char chihiro_game_dir[1024];   /* Game directory path (from dvd_path) */

static char chihiro_save_path[2048];
static bool chihiro_resolve_save_path(void);
/* The DIMM's system area follows the save file (see the IDE map below). */
static void chihiro_dimm_sys_load(void);
static bool chihiro_dimm_sys_flush(void);
static bool chihiro_dimm_sys_dirty;
static bool chihiro_read_image_bootid(uint8_t *bid);
static bool chihiro_game_name(char *out, size_t out_len);
static void chihiro_resolve_card_path(int player, char *out, size_t out_len);
static const CardStock *chihiro_cabinet_card_stock(void);
static bool chihiro_cabinet_is(const char *xbe);
static void chihiro_backup_tick(void);
static QEMUBH *chihiro_netboard_bh;
static void chihiro_netboard_irq_bh(void *opaque);
static void chihiro_netboard_irq(void);
bool lpc_log_verbose = false;
static bool chihiro_board_type3; /* true = ASIC (Type-3), false = FPGA (Type-1) */
static bool chihiro_board_type_known;
static bool chihiro_cabinet_is_type1(void);

/* Which board the machine presents: the setting, or on Auto the cabinet
 * table, once the game is known. */
static void chihiro_resolve_board_type(void)
{
    const char *why;

    if (chihiro_board_type_known) return;

    if (g_config.chihiro.settings.board_type ==
        CONFIG_CHIHIRO_SETTINGS_BOARD_TYPE_TYPE1) {
        chihiro_board_type3 = false;
        why = "chosen in the settings";
    } else if (g_config.chihiro.settings.board_type ==
               CONFIG_CHIHIRO_SETTINGS_BOARD_TYPE_TYPE3) {
        chihiro_board_type3 = true;
        why = "chosen in the settings";
    } else {
        if (!chihiro_game_filename[0]) {
            /* Not named yet: Type-1 until the image has been read. */
            chihiro_board_type3 = false;
            return;
        }
        /* The cabinet table's type1 rows; everything else, known or not,
         * is a Type-3. */
        chihiro_board_type3 = !chihiro_cabinet_is_type1();
        why = "Auto, from the game";
    }
    chihiro_board_type_known = true;
    fprintf(stderr, "Chihiro: media board presented as %s (%s)\n",
            chihiro_board_type3 ? "Type-3 (ASIC)" : "Type-1 (FPGA)", why);
}

bool chihiro_is_type3(void)
{
    chihiro_resolve_board_type();
    return chihiro_board_type3;
}
/* The Net-DIMM board's mailbox, which a networked game asks its firmware
 * version before it starts: answers at 0x91000000, commands at 0x91000200.
 * The older acLib (Ollie King, acMediaDeviceInit at 0x000F6C20) reaches the
 * same blocks as the "mbcom:" IDE sectors 0x9008000 (answers) and 0x9008001
 * (commands), rings by reading 0x90000000 and waits for bit 2 of port
 * 0x40E0. */
#define NETDIMM_RESP_BASE 0x91000000u
#define NETDIMM_CMD_BASE  0x91000200u
#define NETDIMM_RESP_LBA  0x9008000u
#define NETDIMM_CMD_LBA   0x9008001u
/* Those two sectors head a window of 1 MB the same way: sector n of it is
 * the board's SDRAM at 0x600000 + n * 512, where the bulk data of a command
 * travels (an address string for 0x0415, a sockaddr, a packet). */
#define NETDIMM_WINDOW_LBA  NETDIMM_RESP_LBA
#define NETDIMM_WINDOW_SECTORS (CHIHIRO_NETBOARD_WINDOW_SIZE / 512)
static uint32_t chihiro_netdimm_resp[8];
static uint32_t chihiro_netdimm_cmd[8];
static int chihiro_netdimm_cmd_idx;
static int chihiro_netdimm_resp_idx;

static void chihiro_netdimm_signal(void);

/* The game writes the eight command words in a row to the same port, the way
 * it does for the Type-3 board, so the index walks on its own. */
static void chihiro_netdimm_answer(void)
{
    static int logged = 0;

    uint16_t op = (chihiro_netdimm_cmd[0] >> 16) & 0xFFFF;

    memset(chihiro_netdimm_resp, 0, sizeof(chihiro_netdimm_resp));
    chihiro_netdimm_resp[0] = chihiro_netdimm_cmd[0] | 0x80000000u;
    switch (op) {
    case 0x0001:                        /* CONNECT, answered with the DIMM size */
        chihiro_netdimm_resp[1] = mediaboard.dimm_size;
        break;
    case 0x0100:                        /* state and progress */
        chihiro_netdimm_resp[1] = mediaboard.status;
        chihiro_netdimm_resp[2] = mediaboard.progress;
        break;
    case 0x0101:        /* firmware version, 0x0001 above it and nothing
                         * after: the real 13.05 firmware answers 00011305
                         * 00000000 (MEASURED) */
        chihiro_netdimm_resp[1] = 0x00010000u | mediaboard.fw_version;
        break;
    case 0x0103:        /* serial: the media board's, where the real board
                         * answers its own EEPROM's */
        memcpy(&chihiro_netdimm_resp[1], mediaboard.serial, 16);
        break;
    default:
        if ((op & 0xFF00) == 0x0400) {
            /* The 0x04xx sockets without a network board: a board with no
             * cable. Each word is the call's return value (MEASURED on Golf
             * 2006 and the mahjong titles): socket, setsockopt, bind, listen
             * succeed, gethostbyname finds no host, anything needing a peer
             * fails. */
            switch (op) {
            case 0x040B:            /* socket: the board's own, no cable needed */
            case 0x040E:            /* setsockopt */
            case 0x0402:            /* bind */
            case 0x0408:            /* listen */
            /* gethostbyname: no name server behind an unplugged port */
            case 0x0405:
                chihiro_netdimm_resp[1] = 0;
                break;
            /* accept: nobody there; whatever needs a peer fails */
            default:
                chihiro_netdimm_resp[1] = 0xFFFFFFFFu;
                break;
            }
        }
        break;
    }
    chihiro_netdimm_resp_idx = 0;
    /* The game waits for the network board's completion: bit 2 of port
     * 0x40E0 and IRQ 10. */
    chihiro_netdimm_signal();
    if (lpc_log_verbose && logged < 8) { logged++;
        fprintf(stderr, "[%07lld] NETDIMM command %04X seq %04X arg %08X -> %08X\n",
                TS_MS, (chihiro_netdimm_cmd[0] >> 16) & 0xFFFF,
                chihiro_netdimm_cmd[0] & 0xFFFF, chihiro_netdimm_cmd[1],
                chihiro_netdimm_resp[0]); }
}

bool chihiro_freeplay_setting; /* ic11 byte 0x23/0x63 = freeplay in ACBU coin struct */
/* The media board flash (2 MB): the SEGABOOT builds, read through mbrom0 and
 * mbrom1, and the MBDT header at 0xFFE00 with the board's serial. */
static uint8_t *chihiro_flash_rom;
static uint32_t chihiro_flash_rom_size;

/* What the board reports when no network firmware image is named. */
#define MB_FIRM_VERSION_DEFAULT 0x0317
#define MB_FIRM_VERSION_MARK    "FIRM_VERSION is here!"

/* The version network titles check, read from the network firmware's flash
 * image (netboard Ver13.05): the little-endian word after the marker above,
 * on the next 4-byte boundary, which the firmware prints high byte first
 * (VA 0x80021620): 0x13050621 is 13.05(06.21). Games compare with >=. */
static uint16_t mediaboard_net_firmware_version(void)
{
    const char *path = g_config.chihiro.roms.net_firmware_path;
    if (!path || !path[0])
        return MB_FIRM_VERSION_DEFAULT;

    gchar *img = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &img, &len, NULL)) {
        fprintf(stderr, "Chihiro: cannot read the network firmware %s\n", path);
        return MB_FIRM_VERSION_DEFAULT;
    }

    uint16_t ver = MB_FIRM_VERSION_DEFAULT;
    const gsize marklen = strlen(MB_FIRM_VERSION_MARK);
    for (gsize i = 0; i + marklen + 24 < len; i++) {
        if (memcmp(img + i, MB_FIRM_VERSION_MARK, marklen) != 0)
            continue;
        gsize at = (i + marklen + 1 + 3) & ~(gsize)3;   /* past the NUL, aligned */
        for (gsize j = at; j < at + 16 && j + 4 <= len; j += 4) {
            uint32_t w = ldl_le_p(img + j);
            if (!w)
                continue;
            ver = (uint16_t)(w >> 16);
            fprintf(stderr, "Chihiro: network firmware reports version "
                    "%X.%02X(%02X.%02X)\n",
                    (w >> 24) & 0xFF, (w >> 16) & 0xFF,
                    (w >> 8) & 0xFF, w & 0xFF);
            break;
        }
        break;
    }
    if (ver == MB_FIRM_VERSION_DEFAULT)
        fprintf(stderr, "Chihiro: %s carries no version field, the board keeps "
                "%X.%02X\n", path, MB_FIRM_VERSION_DEFAULT >> 8,
                MB_FIRM_VERSION_DEFAULT & 0xFF);
    g_free(img);
    return ver;
}

/* What the FATX builder will need for a game folder: every file's data
 * plus a 16 KiB cluster per entry (chihiro-fatx-builder.c's sizing). */
static uint64_t chihiro_dir_fatx_bytes(const char *dir)
{
    uint64_t total = 0;
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *name;

    if (!d) {
        return 0;
    }
    while ((name = g_dir_read_name(d))) {
        char *child = g_build_filename(dir, name, NULL);
        struct stat st;
        if (stat(child, &st) == 0) {
            total += 16384;
            if (S_ISDIR(st.st_mode)) {
                total += chihiro_dir_fatx_bytes(child);
            } else {
                total += st.st_size;
            }
        }
        g_free(child);
    }
    g_dir_close(d);
    return total;
}

/* The JP1/JP2 jumpers the kernel reads at SEGA_DIMM_SIZE: 0 is 128 MB, 3 is
 * 1024 MB (the setting's enum index). Past those, Auto: the smallest module
 * that holds the game's image (Gundam's 576 MiB needs 1024; the others fit
 * 512). Settled once and logged. */
unsigned chihiro_dimm_factor(void)
{
    static unsigned factor;
    static bool known;

    if (known)
        return factor;
    known = true;

    int set = g_config.chihiro.settings.dimm_size;
    if (set >= SEGA_DIMM_SIZE_128M && set <= SEGA_DIMM_SIZE_1024M) {
        factor = (unsigned)set;
        fprintf(stderr, "Chihiro: media board DIMM %u MB, from the setting\n",
                128u << factor);
        return factor;
    }

    factor = SEGA_DIMM_SIZE_512M;
    const char *path = xemu_chihiro_image();
    int64_t size = -1;
    if (path && path[0] && g_file_test(path, G_FILE_TEST_IS_DIR)) {
        /* A folder is built into FATX in place; fopen() would "succeed" on
         * it and size it at a few bytes. Allow for the superblock, the FAT
         * and the builder's slack. */
        size = (int64_t)chihiro_dir_fatx_bytes(path) + (8 << 20);
    } else if (path && path[0]) {
        FILE *f = qemu_fopen(path, "rb");
        if (f) {
            if (fseek(f, 0, SEEK_END) == 0)
                size = ftell(f);
            fclose(f);
        }
    }
    if (size > 0) {
        /* The smallest DIMM whose mbfs (all but the top 16 MB) holds the
         * image, the largest if none. */
        factor = SEGA_DIMM_SIZE_1024M;
        for (unsigned i = SEGA_DIMM_SIZE_128M; i < SEGA_DIMM_SIZE_1024M; i++) {
            if (size <= (((int64_t)128 << 20) << i) - (16 << 20)) {
                factor = i;
                break;
            }
        }
        fprintf(stderr, "Chihiro: media board DIMM %u MB, worked out from the "
                "%lld MiB image\n", 128u << factor,
                (long long)(size >> 20));
    } else {
        fprintf(stderr, "Chihiro: media board DIMM %u MB — no image to size it "
                "against\n", 128u << factor);
    }
    return factor;
}

/* Refuses every snapshot, taken or loaded, while a linked cabinet's network
 * board runs. */
static const VMStateDescription vmstate_chihiro_cabinet_link = {
    .name = "chihiro-cabinet-link",
    .unmigratable = 1,
};

static void mediaboard_init(void)
{
    mediaboard.dimm_factor = (uint8_t)chihiro_dimm_factor();
    mediaboard.dimm_size   = 0x08000000u << mediaboard.dimm_factor;
    /* The network firmware's version, or 0x0317 (3.17, a real Chihiro's
     * SYSTEM INFO per GXTX) when none is named. */
    mediaboard.fw_version  = mediaboard_net_firmware_version();
    mediaboard.board_type  = 4; /* Chihiro */
    /* Instant READY/100%: a real board goes through phases 0→5 and 0→100%.
     * MEDIA BOARD TEST in the service menu may then show "CHECKING 0%" /
     * "STATUS ----" instead of the progression. Games only check the final
     * state. */
    mediaboard.status      = MB_STATUS_READY;
    mediaboard.progress    = 100;
    mediaboard.net_ip      = chihiro_cabinet_ip();
    memset(mediaboard.serial, 0, sizeof(mediaboard.serial));

    /* Read serial from flash ROM MBDT header if available */
    if (chihiro_flash_rom && chihiro_flash_rom_size > 0xFFE20 &&
        memcmp(chihiro_flash_rom + 0xFFE00, "MBDT", 4) == 0) {
        memcpy(mediaboard.serial, chihiro_flash_rom + 0xFFE10, 16);
    }

    /* The network board itself, running that firmware, when the cabinet is
     * linked (Settings > Network > Cabinet Link). Alone, the C stubs above
     * answer the mailbox. The board is not in a snapshot and the other
     * cabinets would not come back with one, so none is taken or loaded
     * while it runs. */
    if (g_config.chihiro.link.enable) {
        const char *firmware = g_config.chihiro.roms.net_firmware_path;
        if (!firmware || !firmware[0]) {
            xemu_queue_error_message("Cabinet link: the network board needs its "
                                     "firmware (ver1305.bin), see Settings > "
                                     "System > Chihiro Files");
        } else if (chihiro_netboard_init(firmware, mediaboard.serial,
                                         mediaboard.net_ip)) {
            chihiro_netboard_bh = qemu_bh_new(chihiro_netboard_irq_bh, NULL);
            chihiro_netboard_set_host_interrupt(chihiro_netboard_irq);
            chihiro_netboard_attach_net();
            vmstate_register(NULL, 0, &vmstate_chihiro_cabinet_link, NULL);
        } else {
            xemu_queue_error_message("Cabinet link: the network firmware could "
                                     "not be read");
        }
    }
}

static ChihiroLPCState *chihiro_lpc_global;

/* The network board's source only, as its firmware raises it
 * (chihiro_netboard_irq_bh): bit 0 is the media board's. */
static void chihiro_netdimm_signal(void)
{
    if (!chihiro_lpc_global) return;
    chihiro_lpc_global->mbcom_e0_status |= 0x04;
    qemu_irq_raise(chihiro_lpc_global->irq10);
}

/* The network board rings the Xbox from its own thread; the interrupt is
 * raised on the main loop, where the LPC state lives: bit 2 of port
 * 0x40E0 (the acLib's source 3, SegaEther) and IRQ 10. */
static void chihiro_netboard_irq_bh(void *opaque)
{
    chihiro_netdimm_signal();
}

static void chihiro_netboard_irq(void)
{
    if (chihiro_netboard_bh)
        qemu_bh_schedule(chihiro_netboard_bh);
}

/* The game opens its link (mbcom 0x0606) with its test menu's cabinet count
 * and number (the second argument's low bytes); a mismatch with the link's
 * stops the link forming, so it is said once. Maximum Tune counts itself. */
static void chihiro_link_watch_command(const uint32_t *pkt)
{
    static bool said;
    int nodes = pkt[2] & 0xFF, index = (pkt[2] >> 8) & 0xFF;
    char msg[192];

    if (said || !g_config.chihiro.link.enable || (pkt[0] >> 16) != 0x0606)
        return;
    if (nodes < 2 || (nodes == g_config.chihiro.link.cabinets &&
                      index + 1 == g_config.chihiro.link.cabinet))
        return;
    said = true;
    snprintf(msg, sizeof(msg), "The game is set as cabinet %d of %d, the link as "
             "cabinet %d of %d: set GAME ASSIGNMENTS in the game's test menu",
             index + 1, nodes, g_config.chihiro.link.cabinet,
             g_config.chihiro.link.cabinets);
    xemu_queue_notification_warning(msg);
}

unsigned chihiro_log_mask;

static uint8_t chihiro_mbcom_command[512];
static uint8_t chihiro_mbcom_response[512];

/* SEGABOOT parks each media board command in a slot and spins until it is
 * marked done. The slot table is read out of the lookup routine, the same 46
 * bytes in both SEGABOOT builds of the flash:
 *   B9 <first slot> | 33 D2 56 EB 06 ... | 83 C2 <stride> 83 C1 <stride>
 *   81 FA <table size>
 * (fpr21042: 16 slots of 0x60 at 0x000AA7B0 for the build at 0x0003E250, 64
 * at 0x00112820 for the one at 0x00041010, which boots). A reply is 128 bytes
 * of the board's memory, fetched through the 0x4000 register window. */
typedef struct ChihiroMbcomSlots {
    uint32_t slots;     /* first slot */
    uint32_t meta;      /* slot header, 0x20 below the slot itself */
    uint32_t stride;
    uint32_t count;
    bool     known;
} ChihiroMbcomSlots;

static ChihiroMbcomSlots chihiro_mbcom_slots;

#define CHIHIRO_MBCOM_SLOT_MAX       64
/* The bus answers in milliseconds, so a slot still untouched after a second
 * is one nobody is going to answer. */
#define CHIHIRO_MBCOM_SLOT_GRACE_MS  1000

/* USB devices for delayed hotplug (simulates AN2131 I2C firmware boot) */
static USBDevice *chihiro_usb_qc = NULL;
static USBDevice *chihiro_usb_sc = NULL;

/*
 * Called from ohci_bus_start() when OHCI goes OPERATIONAL.
 * Schedule USB device attachment after BUS START (QC +50 ms, SC +100 ms) so
 * that:
 * 1. The kernel has already enabled RHSC interrupts
 * 2. Fresh CSC events will trigger the RHSC handler
 * 3. The handler will do full enumeration: PortReset → GET_DESC → SET_ADDRESS
 *    → GET_CONFIG_DESC → SET_CONFIG (unconditional, per standard USB flow)
 *
 * On real hardware, AN2131 chips boot in ~200ms and are present BEFORE the
 * kernel starts OHCI. The kernel's first port scan sees CSC=1 and enumerates.
 * In our emulation, we attach AFTER BUS START to ensure the RHSC handler
 * sees fresh CSC=1 events (not stale ones cleared during OHCI init).
 */
static int ohci_bus_start_count = 0;
static bool ohci_bus_running;

/* After a QuickReboot, whatever it starts, SEGABOOT again or the game: the
 * watch is armed again and the mbcom and DMA state starts over. */
static void chihiro_quickreboot_reset(ChihiroLPCState *s)
{
    chihiro_game_running = false;
    chihiro_mbcom_bootstrap_done = false;
    chihiro_mbcom_slots.known = false;
    chihiro_e1_armed = false;
    memset(chihiro_mbcom_command, 0, 32);
    memset(chihiro_mbcom_response, 0, 32);

    s->diag_armed = false;
    timer_mod(s->diag_arm_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
    s->lpc_401e_reads = 0;
    s->lpc_scratch_4026 = 0;
    s->mbcom_e0_status = 0;
    s->mbcom_resp_ready = false;
    s->bb_dma_active = false;
    s->bb_dma_count = 0;
    s->bb_reg_addr = 0;
    s->bb_reg_status = 0;
    memset(s->dimm_cmd, 0, sizeof(s->dimm_cmd));
    memset(s->dimm_resp, 0, sizeof(s->dimm_resp));
    timer_del(s->dimm_resp_timer);
    timer_mod(s->diag_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
}

void chihiro_on_ohci_bus_start(void)
{
    ohci_bus_running = true;

    if (!chihiro_active) {
        return;
    }

    ChihiroLPCState *s = chihiro_lpc_global;
    if (!s) {
        return;
    }

    ohci_bus_start_count++;
    fprintf(stderr, "[%07lld] Chihiro USB: BUS START #%d\n", TS_MS, ohci_bus_start_count);

    /* Detect QuickReboot back to SEGABOOT: when in game mode, the game's own
     * BUS START is the first one (game_mode_bus_starts goes 0→1). The second
     * BUS START means SEGABOOT reloaded after a soft reinit (QuickReboot). */
    if (chihiro_game_running) {
        game_mode_bus_starts++;
        if (game_mode_bus_starts == 1) {
            chihiro_patch_running_game();
        }
        if (game_mode_bus_starts >= 2) {
            fprintf(stderr, "[%07lld] QUICKREBOOT (BUS START #%d in game mode)\n",
                    TS_MS, game_mode_bus_starts);
            chihiro_quickreboot_reset(s);
        }
    }

    /* First BUS START: schedule initial hotplug (devices not yet attached) */
    /* Subsequent BUS STARTs (game kernel): detach + re-attach for fresh CSC */
    if (chihiro_usb_qc && chihiro_usb_qc->attached) {
        usb_device_detach(chihiro_usb_qc);
    }
    if (chihiro_usb_sc && chihiro_usb_sc->attached) {
        usb_device_detach(chihiro_usb_sc);
    }

    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    /* Schedule QC hotplug at BUS_START + 50ms */
    timer_mod(s->usb_hotplug_timer, now + 50);

    /* Schedule SC hotplug at BUS_START + 100ms */
    timer_mod(s->usb_hotplug_sc_timer, now + 100);
}

/* SEGABOOT tears the bus down to hand the machine over to the game. */
void chihiro_on_ohci_bus_stop(void)
{
    bool was_running = ohci_bus_running;
    ohci_bus_running = false;

    ChihiroLPCState *s = chihiro_lpc_global;
    if (!was_running || !chihiro_active || chihiro_game_running || !s) {
        return;
    }

    chihiro_game_running = true;
    game_mode_bus_starts = 0;
    memset(chihiro_mbcom_command, 0, 32);
    s->mbcom_resp_ready = false;
    s->mbcom_e0_status = 0;
    CHIHIRO_LOGF(BOOT, "game started (bus stopped by SEGABOOT)\n");
}


void chihiro_usb_set_devices(USBDevice *qc, USBDevice *sc)
{
    chihiro_usb_qc = qc;
    chihiro_usb_sc = sc;
}

/*
 * Simulates AN2131 firmware boot from I2C EEPROM.
 * On real hardware, the AN2131 chips take ~200-500ms to load firmware
 * from ic10/pc20 EEPROMs before appearing on the USB bus.
 * This timer fires after the kernel's initial USB scan is complete,
 * causing a hot-plug event that triggers re-enumeration.
 */
/*
 * QC hotplug — fires first (BUS START + 50 ms).
 * On real hardware, QC (ic10 EEPROM, 6864 bytes firmware) boots first.
 */
static void chihiro_usb_hotplug_qc_cb(void *opaque)
{
    if (chihiro_usb_qc && !chihiro_usb_qc->attached) {
        usb_device_attach(chihiro_usb_qc, &error_abort);
    }
}

/*
 * SC hotplug — fires 50 ms after QC (BUS START + 100 ms).
 * On real hardware, SC (pc20 EEPROM, 6731 bytes firmware) boots independently.
 * The gap lets the kernel process QC's RHSC event completely
 * (port reset → GET_DESC → SET_ADDRESS → GET_DESC config → SET_CONFIG)
 * before SC's RHSC event arrives as a separate enumeration cycle.
 */
static void chihiro_usb_hotplug_sc_cb(void *opaque)
{
    if (chihiro_usb_sc && !chihiro_usb_sc->attached) {
        usb_device_attach(chihiro_usb_sc, &error_abort);
    }
}

/*
 * Walk x86 page tables (non-PAE) to translate VA → PA.
 * Returns physical address, or 0xFFFFFFFF on failure.
 */
uint32_t chihiro_va_to_pa(uint32_t va)
{
    CPUState *cpu = first_cpu;
    if (!cpu) return 0xFFFFFFFF;

    X86CPU *x86 = X86_CPU(cpu);
    uint32_t cr3 = x86->env.cr[3] & 0xFFFFF000;
    uint32_t pde_addr = cr3 + ((va >> 22) * 4);
    uint32_t pde;
    cpu_physical_memory_read(pde_addr, &pde, 4);
    if (!(pde & 1)) return 0xFFFFFFFF;  /* not present */

    if (pde & 0x80) {
        /* 4MB page (PS bit set) */
        return (pde & 0xFFC00000) | (va & 0x003FFFFF);
    }

    uint32_t pte_addr = (pde & 0xFFFFF000) + (((va >> 12) & 0x3FF) * 4);
    uint32_t pte;
    cpu_physical_memory_read(pte_addr, &pte, 4);
    if (!(pte & 1)) return 0xFFFFFFFF;  /* not present */

    return (pte & 0xFFFFF000) | (va & 0xFFF);
}

/* The Tamura HW210 readers, one per player, driven by the SC 8051 UARTs,
 * and their slots (see chihiro_hw210_card_key). A snapshot keeps them with
 * the two JVS values the slots follow. */
typedef struct {
    CardReaderState reader[2];
    int32_t  slot[2];
    uint32_t pushed_at[2];
    bool     lock[2];           /* chihiro_jvs_card_lock, at a save */
    uint32_t switch_reads;      /* chihiro_jvs_switch_reads, likewise */
} ChihiroHW210;
static ChihiroHW210 hw210;
static bool card_reader_initialized;

/* Exposed to the SC (AN2131) layer so the 8051 UART drives the readers
 * directly. [0] = MIDI/UART1 reader, [1] = RS-232C/UART0 reader. */
CardReaderState *chihiro_card_reader_global = hw210.reader;
/* An HW210 reader owns the SC's UART1 (MIDI): with the reader on, no drive
 * board answers there. The MIDI peer is otherwise the cabinet's drive board
 * (chihiro-an2131.c). */
bool chihiro_hw210_enabled;
bool chihiro_crp1231_enabled;

/* Physical card-insertion microswitch state for the JVS input path. */
bool chihiro_card_reader_present(int player)
{
    if (player < 0 || player > 1) return false;
    return hw210.reader[player].card_present;
}

/* A file the reader refused (the reason is in the log), named on screen. */
static void chihiro_card_refused(const char *path)
{
    char *name = g_path_get_basename(path);
    char msg[300];
    snprintf(msg, sizeof(msg), "Not a card: %.250s", name);
    g_free(name);
    xemu_queue_notification_warning(msg);
}

static void chihiro_hw210_insert(int player, const char *path)
{
    card_reader_insert(&hw210.reader[player], path, chihiro_cabinet_card_stock());
    if (path[0] && !hw210.reader[player].card_present)
        chihiro_card_refused(path);
}

/* The setting that names a slot's card. Maximum Tune 1 and 2 have one each:
 * 2 rewrites a card of 1 in its own format, which 1 then refuses. */
static const char **chihiro_card_slot_setting(ChihiroCardSlot slot)
{
    switch (slot) {
    case CHIHIRO_CARD_SLOT_CRP1231:
        return chihiro_cabinet_is("V307")
                   ? &g_config.chihiro.card_reader.crp1231.mt1
                   : &g_config.chihiro.card_reader.crp1231.mt2;
    case CHIHIRO_CARD_SLOT_GUNDAM:
        return &g_config.chihiro.card_reader.hw210.gundam;
    case CHIHIRO_CARD_SLOT_HW210_P1:
        return &g_config.chihiro.card_reader.hw210.slot1;
    default:
        return &g_config.chihiro.card_reader.hw210.slot2;
    }
}

/* The Card In key: the card in hand, or a blank, goes into the reader's
 * mouth and is drawn in when the game asks for a card (new cards come from the
 * dispenser); a second press takes back a card still in the mouth. */
void chihiro_crp1231_card_key(void)
{
    CRP1231State *r = chihiro_crp1231_global;
    const char *hand = *chihiro_card_slot_setting(CHIHIRO_CARD_SLOT_CRP1231);

    if (!chihiro_crp1231_enabled || !r)
        return;
    switch (r->card_pos) {
    case CRP1231_CARD_IN:
        xemu_queue_notification_warning("The card is in the reader");
        return;
    case CRP1231_CARD_GATE:
        /* Handed back and taken at the game's next look (crp1231_respond);
         * nothing else goes in meanwhile. */
        xemu_queue_notification_warning("The card is coming out");
        return;
    case CRP1231_CARD_ENTRY:
        crp1231_take_back(r);
        xemu_queue_notification("Card out");
        return;
    }
    if (!crp1231_offer(r, hand)) {
        chihiro_card_refused(hand);
        return;
    }
    fprintf(stderr, "Chihiro: CRP-1231 card offered: %s\n",
            hand && hand[0] ? hand : "(blank)");
    xemu_queue_notification(hand && hand[0] ? "Card in" : "Blank card in");
}

/* Where each HW210 slot's card is. Card In pushes the card in hand, or a blank
 * from the cabinet's stock. A game waiting for a card locks it (vsg.xbe
 * FUN_0002da90 state 1, gs.xbe FUN_00066a30) and ejects it by releasing the
 * lock, when the drawer's spring pushes it out; a card no game waits for
 * springs back after the push. A locked card is never pulled (both games
 * break). HEURISTIC: a push lasts 30 of the game's switch reads, half a
 * second. */
enum { HW210_EMPTY, HW210_PUSHED, HW210_LOCKED };
#define HW210_PUSH_READS 30

static ChihiroCardSlot chihiro_hw210_card_slot(int player)
{
    if (chihiro_cabinet_is("gs"))
        return CHIHIRO_CARD_SLOT_GUNDAM;
    return player ? CHIHIRO_CARD_SLOT_HW210_P2 : CHIHIRO_CARD_SLOT_HW210_P1;
}

bool chihiro_hw210_card_issue(const CardReaderState *s, char *out,
                              size_t out_len)
{
    int player = s - hw210.reader;

    return player >= 0 && player < 2 &&
           chihiro_card_issue(chihiro_hw210_card_slot(player), out, out_len);
}

void chihiro_hw210_card_key(int player)
{
    char path[1200];

    if (!chihiro_hw210_enabled || !card_reader_initialized ||
        !chihiro_cabinet_card_lock(player))
        return;
    if (hw210.slot[player] == HW210_LOCKED) {
        xemu_queue_notification_warning("Card locked, the game is using it");
        return;
    }
    if (hw210.slot[player] == HW210_PUSHED)
        return;

    chihiro_resolve_card_path(player, path, sizeof(path));
    if (!path[0] &&
        !chihiro_card_issue(chihiro_hw210_card_slot(player), path,
                            sizeof(path))) {
        xemu_queue_notification_warning("No blank card could be made");
        return;
    }
    chihiro_hw210_insert(player, path);
    if (!hw210.reader[player].card_present)
        return;
    hw210.slot[player] = HW210_PUSHED;
    hw210.pushed_at[player] = chihiro_jvs_switch_reads;
    fprintf(stderr, "Chihiro: card P%d pushed in: %s\n", player + 1, path);
}

/*
 * Card reader support. The serial data path is fully LLE: the game's USB
 * vendor requests reach the real SC 8051 firmware, which talks to the
 * HW210 readers over its two UARTs (chihiro-an2131.c). This tick runs the
 * slot's PUSHED/LOCKED states around the game's card lock; the UI assignment
 * is chihiro_card_ui_sync.
 */
static void chihiro_card_reader_tick(void)
{
    if (!card_reader_initialized) {
        card_reader_init(&hw210.reader[0]);
        card_reader_init(&hw210.reader[1]);
        hw210.slot[0] = hw210.slot[1] = HW210_EMPTY;
        card_reader_initialized = true;
        fprintf(stderr, "Chihiro: card reader enabled\n");
    }

    for (int p = 0; p < 2; p++) {
        uint32_t reads = chihiro_jvs_switch_reads - hw210.pushed_at[p];
        bool lock = chihiro_jvs_card_lock[p];
        const char *out = NULL;

        switch (hw210.slot[p]) {
        case HW210_PUSHED:
            if (lock) {
                hw210.slot[p] = HW210_LOCKED;
                CHIHIRO_LOGF(CARD, "P%d card locked %u switch reads after "
                             "the push\n", p + 1, reads);
                chihiro_card_note("Card in", false);
            } else if (reads >= HW210_PUSH_READS) {
                out = "Card not taken";
            }
            break;
        case HW210_LOCKED:
            if (!lock)
                out = "Card out";
            break;
        }
        if (out) {
            card_reader_remove(&hw210.reader[p]);
            fprintf(stderr, "Chihiro: card P%d %s\n", p + 1,
                    hw210.slot[p] == HW210_LOCKED ? "ejected" : "not taken");
            chihiro_card_note(out, hw210.slot[p] == HW210_PUSHED);
            hw210.slot[p] = HW210_EMPTY;
        }
    }
}

static const VMStateDescription vmstate_chihiro_hw210_reader = {
    .name = "chihiro-cardreader-hw210/reader",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(rx_buf, CardReaderState, CARD_TOTAL_SIZE + 32),
        VMSTATE_INT32(rx_pos, CardReaderState),
        VMSTATE_INT32(rx_expected, CardReaderState),
        VMSTATE_UINT8_ARRAY(tx_buf, CardReaderState, 2048 + 8),
        VMSTATE_INT32(tx_len, CardReaderState),
        VMSTATE_INT32(tx_pos, CardReaderState),
        VMSTATE_BOOL(card_present, CardReaderState),
        VMSTATE_UINT8_ARRAY(card_data, CardReaderState, CARD_TOTAL_SIZE),
        VMSTATE_BOOL(dirty, CardReaderState),
        VMSTATE_END_OF_LIST()
    }
};

static int hw210_pre_save(void *opaque)
{
    memcpy(hw210.lock, chihiro_jvs_card_lock, sizeof(hw210.lock));
    hw210.switch_reads = chihiro_jvs_switch_reads;
    return 0;
}

/* A card that comes back may be an older state of a card whose file has
 * moved on since, and writing it there would undo those games. It comes
 * back with no file, so its next write makes a new one, as the CRP-1231's
 * does; dropped before the section is read, a load that fails half-way
 * keeps no file either. */
static int hw210_pre_load(void *opaque)
{
    for (int p = 0; p < 2; p++)
        hw210.reader[p].card_path[0] = '\0';
    return 0;
}

static int hw210_post_load(void *opaque, int version_id)
{
    bool refused = false;

    for (int p = 0; p < 2; p++) {
        CardReaderState *r = &hw210.reader[p];
        if (r->rx_pos < 0 || r->rx_pos > (int)sizeof(r->rx_buf) ||
            r->rx_expected < 0 || r->rx_expected > (int)sizeof(r->rx_buf) ||
            r->tx_len < 0 || r->tx_len > (int)sizeof(r->tx_buf) ||
            r->tx_pos < 0 || r->tx_pos > r->tx_len) {
            /* Cleared too: a failed load can still be resumed. */
            r->rx_pos = r->rx_expected = 0;
            r->tx_len = r->tx_pos = 0;
            refused = true;
        }
    }
    if (refused) {
        return -EINVAL;
    }
    memcpy(chihiro_jvs_card_lock, hw210.lock, sizeof(hw210.lock));
    chihiro_jvs_switch_reads = hw210.switch_reads;
    card_reader_initialized = true;
    return 0;
}

/* Half a command in and a card locked in its drawer: a snapshot that lost
 * either would bring back a phantom card or a false "Card out". */
static const VMStateDescription vmstate_chihiro_hw210 = {
    .name = "chihiro-cardreader-hw210",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = hw210_pre_save,
    .pre_load = hw210_pre_load,
    .post_load = hw210_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(reader, ChihiroHW210, 2, 1,
                             vmstate_chihiro_hw210_reader, CardReaderState),
        VMSTATE_INT32_ARRAY(slot, ChihiroHW210, 2),
        VMSTATE_UINT32_ARRAY(pushed_at, ChihiroHW210, 2),
        VMSTATE_BOOL_ARRAY(lock, ChihiroHW210, 2),
        VMSTATE_UINT32(switch_reads, ChihiroHW210),
        VMSTATE_END_OF_LIST()
    }
};

/* One row per cabinet: input profile, card reader, drive board, media board.
 * A row matches a case-insensitive prefix of the XBE's name, directory
 * stripped ("A\V322.xbe" as "V322.xbe"), so revisions match their title. */
typedef struct {
    const char           *xbe;      /* case-insensitive prefix of the basename */
    int                   profile;  /* CONFIG_CHIHIRO_JVS_PROFILE_*, -1 = none */
    ChihiroCardReaderKind card;
    ChihiroDriveBoardKind drive;
    bool                  type1;    /* shipped on a Type-1 media board */
    ChihiroMonitorKind    monitor;  /* the scan frequency it is wired for */
    const CardStock      *stock;    /* what a fresh card from its stock carries */
    const char           *cards;    /* its cards' name, the game's MAME name */
    uint8_t               locks[2]; /* card lock outputs, JVS GPO bank 0 */
} ChihiroCabinet;

/* Blocks 4 to 6 of a fresh card, as each game's header check wants them.
 * Ghost Squad (vsg.xbe): 95 71, type 36 4X, serial 0, version 03 F2 in block
 * 6, read off real cards. Gundam B.O.S. (gs.xbe FUN_00065730, FUN_000657c0):
 * type 45 2X, X the BCD serial's digit sum modulo 10, serial at least 2001;
 * with no Banpresto card dumped, the smallest accepted serial. Block 0 is the
 * card's number (see CardStock). */
static const CardStock chihiro_card_stock_vsg = {
    .header = { 0x95, 0x71, 0x36, 0x40, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0x03, 0xF2 },
    .numbered = false,
};
static const CardStock chihiro_card_stock_gs = {
    .header = { 0, 0, 0x45, 0x23, 0x00, 0x00, 0x20, 0x01,
                0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0 },
    .numbered = true,
};

/* The card locks. Ghost Squad (vsg.xbe) drives one solenoid per slot from
 * JVS bank 0: 0x20 for player 1 and 0x01 for player 2 (tables 0x20D52C,
 * 0x21D010, 0x20CD5C; 0x40 is player 1's gun recoil there, PLAYER 1 GUN
 * REACTION in the OUTPUT TEST). It holds a card it finds in a slot
 * (FUN_0002da90, state 1) and lets go to eject it (state 2): read errors,
 * the player's choice, the end of a game. Gundam B.O.S. has one, on 0x40
 * (the OUTPUT TEST of gs_gtest.xbe names it CARD LOCK): gs.xbe
 * FUN_00064760(1) as the card is detected (FUN_00066a30) and at the start
 * of each read (FUN_00065590), FUN_00064760(0) at rest and at the end of a
 * game (FUN_000667a0, FUN_000669e0). */
static const ChihiroCabinet chihiro_cabinets[] = {
    { "hod3xb",    CONFIG_CHIHIRO_JVS_PROFILE_HOTD3, CHIHIRO_CARD_NONE,    CHIHIRO_DRIVE_NONE,    true,  CHIHIRO_MONITOR_15KHZ, NULL, NULL, { 0, 0 } },
    { "ctx_ac",    CONFIG_CHIHIRO_JVS_PROFILE_CTX,   CHIHIRO_CARD_NONE,    CHIHIRO_DRIVE_NONE,    true,  CHIHIRO_MONITOR_15KHZ, NULL, NULL, { 0, 0 } },
    { "vc3",       CONFIG_CHIHIRO_JVS_PROFILE_VC3,   CHIHIRO_CARD_NONE,    CHIHIRO_DRIVE_NONE,    false, CHIHIRO_MONITOR_15KHZ, NULL, NULL, { 0, 0 } },
    { "vsg",       CONFIG_CHIHIRO_JVS_PROFILE_GS,    CHIHIRO_CARD_HW210,   CHIHIRO_DRIVE_NONE,    false, CHIHIRO_MONITOR_15KHZ, &chihiro_card_stock_vsg, "ghostsqu", { 0x20, 0x01 } },
    { "outrun2",   CONFIG_CHIHIRO_JVS_PROFILE_OR2,   CHIHIRO_CARD_NONE,    CHIHIRO_DRIVE_SEGA838, false, CHIHIRO_MONITOR_15KHZ, NULL, NULL, { 0, 0 } },
    { "OllieKing", CONFIG_CHIHIRO_JVS_PROFILE_OK,    CHIHIRO_CARD_NONE,    CHIHIRO_DRIVE_NONE,    false, CHIHIRO_MONITOR_15KHZ, NULL, NULL, { 0, 0 } },
    /* Maximum Tune 1 and 2. Namco numbered these discs V3xx; the test-mode
     * image V322TEST.xbe is the same cabinet. */
    { "V307",      CONFIG_CHIHIRO_JVS_PROFILE_WMMT2, CHIHIRO_CARD_CRP1231, CHIHIRO_DRIVE_V257,    false, CHIHIRO_MONITOR_15KHZ, NULL, "wangmid", { 0, 0 } },
    { "V322",      CONFIG_CHIHIRO_JVS_PROFILE_WMMT2, CHIHIRO_CARD_CRP1231, CHIHIRO_DRIVE_V257,    false, CHIHIRO_MONITOR_15KHZ, NULL, "wangmid2", { 0, 0 } },
    /* Gundam B.O.S. (gs.xbe, test gs_gtest.xbe): one HW210-family reader on
     * SC UART1, Ghost Squad's player 1 wire, same frames and blocks (gs.xbe
     * FUN_00079a90, FUN_0007ae00); UART0 is never opened. */
    { "gs",        CONFIG_CHIHIRO_JVS_PROFILE_GUNDAM, CHIHIRO_CARD_HW210,  CHIHIRO_DRIVE_NONE,    false, CHIHIRO_MONITOR_15KHZ, &chihiro_card_stock_gs, "gundamos", { 0x40, 0x00 } },
    /* 31 kHz sit-down cabinets: only the monitor is stated (MJ3 Evolution
     * stops on Caution 51 at 15 kHz). */
    { "mj3",       -1,                               CHIHIRO_CARD_NONE,    CHIHIRO_DRIVE_NONE,    false, CHIHIRO_MONITOR_31KHZ, NULL, NULL, { 0, 0 } },
    { "mj2",       -1,                               CHIHIRO_CARD_NONE,    CHIHIRO_DRIVE_NONE,    false, CHIHIRO_MONITOR_31KHZ, NULL, NULL, { 0, 0 } },
    { "golf",      -1,                               CHIHIRO_CARD_NONE,    CHIHIRO_DRIVE_NONE,    false, CHIHIRO_MONITOR_31KHZ, NULL, NULL, { 0, 0 } },
};

static void chihiro_cabinet_announce(const ChihiroCabinet *c);

/* The cabinet is settled once the game names itself, and kept: the 8051
 * asks for it every instruction. */
static const ChihiroCabinet *chihiro_cabinet_known;
static bool chihiro_cabinet_settled;

/* The game executable changed, so the answer has to be worked out again. */
static void chihiro_cabinet_forget(void)
{
    chihiro_cabinet_settled = false;
}

static const ChihiroCabinet *chihiro_cabinet(void)
{
    /* The name carries the directory it was booted from ("A\\V322.xbe"). */
    const char *n = chihiro_game_filename;
    const char *sep;

    if (chihiro_cabinet_settled)
        return chihiro_cabinet_known;

    if (!n[0])
        return NULL;   /* the game has not named itself yet */

    for (sep = n; *sep; sep++)
        if (*sep == '\\' || *sep == '/')
            n = sep + 1;

    for (size_t i = 0; i < ARRAY_SIZE(chihiro_cabinets); i++) {
        const char *pfx = chihiro_cabinets[i].xbe;
        if (strncasecmp(n, pfx, strlen(pfx)) == 0) {
            chihiro_cabinet_announce(&chihiro_cabinets[i]);
            chihiro_cabinet_known = &chihiro_cabinets[i];
            chihiro_cabinet_settled = true;
            return chihiro_cabinet_known;
        }
    }
    chihiro_cabinet_announce(NULL);
    chihiro_cabinet_known = NULL;
    chihiro_cabinet_settled = true;
    return NULL;
}

static bool chihiro_cabinet_is_type1(void)
{
    const ChihiroCabinet *c = chihiro_cabinet();
    return c && c->type1;
}

/* Said once, when the game first names itself: what the table decided this
 * cabinet is. A line in the log beats reading the table and guessing. */
static void chihiro_cabinet_announce(const ChihiroCabinet *c)
{
    static const char *cards[] = { "no card reader", "Tamura HW210", "CRP-1231" };
    static const char *drives[] = { "no drive board", "Sega 838", "Namco V257" };
    static const ChihiroCabinet *said;

    if (c == said) return;
    said = c;
    if (!c) {
        fprintf(stderr, "Chihiro: cabinet not in the table, nothing fitted\n");
        return;
    }
    fprintf(stderr, "Chihiro: cabinet %s — input profile %d, %s, %s%s\n",
            c->xbe, c->profile, cards[c->card], drives[c->drive],
            c->type1 ? ", Type-1 media board" : "");
}

ChihiroCardReaderKind chihiro_cabinet_card_reader(void)
{
    const ChihiroCabinet *c = chihiro_cabinet();
    return c ? c->card : CHIHIRO_CARD_NONE;
}

/* The header a fresh card from this cabinet's stock carries, NULL when it
 * sells none. */
static const CardStock *chihiro_cabinet_card_stock(void)
{
    const ChihiroCabinet *c = chihiro_cabinet();
    return c ? c->stock : NULL;
}

/* Whether the running title is the one this row names. For the few things
 * that are a game's own RAM, not its cabinet. */
static bool chihiro_cabinet_is(const char *xbe)
{
    const ChihiroCabinet *c = chihiro_cabinet();
    return c && strcmp(c->xbe, xbe) == 0;
}

ChihiroDriveBoardKind chihiro_cabinet_drive_board(void)
{
    const ChihiroCabinet *c = chihiro_cabinet();
    return c ? c->drive : CHIHIRO_DRIVE_NONE;
}

uint8_t chihiro_cabinet_card_lock(int player)
{
    const ChihiroCabinet *c = chihiro_cabinet();
    return c && player >= 0 && player < 2 ? c->locks[player] : 0;
}

ChihiroMonitorKind chihiro_cabinet_monitor(void)
{
    const ChihiroCabinet *c = chihiro_cabinet();
    return c ? c->monitor : CHIHIRO_MONITOR_15KHZ;
}

/* Execution VAs of the SEGABOOT the board boots: the second megabyte of the
 * flash. Other dumps carry other builds. */
#define SEGABOOT_VERSION    "2.13.0"
#define SEGABOOT_LOGO_VTABLE 0x0001F60C  /* CLogo vtable */
#define SEGABOOT_LOGO_UPDATE 0x000259D0  /* its second entry, CLogo::Update */
#define SEGABOOT_APP_VTABLE  0x0001F0C0  /* application object vtable */
#define SEGABOOT_APP_LOGO    0x440       /* app field holding the CLogo pointer */

static bool sb_read_va(uint32_t va, void *buf, unsigned len)
{
    uint8_t *p = buf;

    while (len) {
        uint32_t pa = chihiro_va_to_pa(va);
        if (pa == 0xFFFFFFFF)
            return false;
        unsigned n = 0x1000 - (va & 0xFFF);
        if (n > len)
            n = len;
        cpu_physical_memory_read(pa, p, n);
        va += n;
        p += n;
        len -= n;
    }
    return true;
}

static bool sb_resident(void)
{
    uint32_t update;

    return sb_read_va(SEGABOOT_LOGO_VTABLE + 4, &update, 4) &&
           update == SEGABOOT_LOGO_UPDATE;
}

/* Located by vtable, not by address: operator new moves it. The two hops
 * back through the app object rule out a stray copy of the constant. */
static uint32_t sb_find_logo(void)
{
    for (uint32_t va = 0x00010000; va < 0x00800000; va += 0x1000) {
        uint32_t pa = chihiro_va_to_pa(va);
        if (pa == 0xFFFFFFFF)
            continue;

        uint32_t page[1024];
        cpu_physical_memory_read(pa, page, sizeof(page));

        for (unsigned i = 0; i < ARRAY_SIZE(page); i++) {
            if (page[i] != SEGABOOT_LOGO_VTABLE)
                continue;

            uint32_t obj = va + i * 4, app, app_vtable, back;
            if (!sb_read_va(obj + 4, &app, 4) ||
                !sb_read_va(app, &app_vtable, 4) ||
                app_vtable != SEGABOOT_APP_VTABLE ||
                !sb_read_va(app + SEGABOOT_APP_LOGO, &back, 4) || back != obj)
                continue;

            return obj;
        }
    }
    return 0;
}

/* SEGABOOT's own table, VA 0x1CEE0. */
static const char *sb_error_message(uint32_t code)
{
    static const char *messages[] = {
        [1]  = "This game is not acceptable by main board.",
        [2]  = "Main board malfunctioning.",
        [3]  = "Bad serial number on main board.",
        [4]  = "Bad serial number on media board.",
        [5]  = "This game is not acceptable by main board.",
        [6]  = "This game is not available on this system.",
        [11] = "JVS I/O board is not connected to main board.",
        [12] = "JVS I/O board does not fulfill the game spec.",
        [13] = "Communication error occurred between main board and JVS I/O board.",
        [14] = "Network firmware version does not fulfill the game spec.",
        [21] = "This game is not acceptable by main board.",
        [22] = "Communication error occurred between main board and media board.",
        [23] = "GD-ROM drive cover is open.",
        [24] = "GD-ROM is not found.",
        [25] = "Cannot access GD-ROM drive.",
        [26] = "Media board malfunctioning.",
        [27] = "DIMM memory is not enough.",
        [31] = "This game is not acceptable by main board.",
        [32] = "DIMM memory is not enough.",
        [33] = "Gateway is not found.",
        [34] = "Gateway cannot be found.",
        [51] = "Wrong video output setting of horizontal scanning frequency.",
        [52] = "Wrong video output setting of horizontal/vertical screen.",
        [53] = "Wrong DIMM memory size setting.",
    };

    if (code < ARRAY_SIZE(messages) && messages[code])
        return messages[code];
    return "Unknown error occurred.";
}

/* Report SEGABOOT state transitions and the error it puts on screen. */
static void chihiro_segaboot_poll(void)
{
    static const char *const states[] = {
        "logo fade in", "init", "logo hold", "waiting for media board",
        "verifying game", "fade out", "launching", "ERROR DISPLAY", "done"
    };
    static uint32_t logo_va, prev_state = UINT32_MAX, prev_error;
    static unsigned attempts;

    if (!sb_resident()) {
        logo_va = 0;
        attempts = 0;
        return;
    }
    if (!logo_va) {
        if (attempts++ > 100)
            return;
        logo_va = sb_find_logo();
        if (!logo_va)
            return;
        CHIHIRO_LOGF(BOOT, "SEGABOOT: state machine at VA %#x\n", logo_va);
        prev_state = UINT32_MAX;
        prev_error = 0;
    }

    uint32_t f[2];  /* CLogo +0x10: error code then state */
    if (!sb_read_va(logo_va + 0x10, f, sizeof(f))) {
        logo_va = 0;
        return;
    }
    uint32_t error = f[0], state = f[1];

    if (state != prev_state && state < ARRAY_SIZE(states)) {
        CHIHIRO_LOGF(BOOT, "SEGABOOT: state %u — %s\n", state, states[state]);
        prev_state = state;
    }
    if (error && error != prev_error) {
        CHIHIRO_ERRF("SEGABOOT: *** %s %02u — %s ***\n",
                     error < 50 ? "Error" : "Caution", error,
                     sb_error_message(error));
        prev_error = error;
    }
}

/* The name ends up in file paths (saves/<name>.sav): a ".." component or a
 * drive colon, which no game uses, would lead them out of the data folder. */
static bool chihiro_executable_name_safe(const char *name)
{
    if (strchr(name, ':')) {
        return false;
    }
    for (const char *p = name; *p;) {
        size_t n = strcspn(p, "\\/");
        if (n == 2 && p[0] == '.' && p[1] == '.') {
            return false;
        }
        p += n;
        if (*p) {
            p++;
        }
    }
    return true;
}

/* Sega netboot boot.id: "BTID" at 0, "XBAM" at 0x20, game executable at
 * 0xA0 (31 chars, backslash-prefixed). The one parser for every source. */
bool chihiro_bootid_executable(const uint8_t *bid, char *out, size_t out_len)
{
    if (memcmp(bid, "BTID", 4) != 0 || memcmp(bid + 0x20, "XBAM", 4) != 0) {
        return false;
    }
    char exec[32];
    memcpy(exec, bid + 0xA0, 31);
    exec[31] = 0;
    const char *name = exec;
    while (*name == '\\' || *name == '/') name++;
    if (!*name || !chihiro_executable_name_safe(name)) {
        return false;
    }
    g_strlcpy(out, name, out_len);
    return true;
}

/* The single writer of the game executable name; every consumer (save
 * file, JVS profile, drive board) reads chihiro_game_filename. */
void chihiro_set_game_executable(const char *name)
{
    if (!chihiro_executable_name_safe(name) ||
        strcmp(chihiro_game_filename, name) == 0) {
        return;
    }
    g_strlcpy(chihiro_game_filename, name, sizeof(chihiro_game_filename));
    chihiro_cabinet_forget();
    printf("Chihiro: game → '%s'\n", chihiro_game_filename);
}

/* A game launched from a directory carries its boot.id as a file. */
static void chihiro_capture_game_filename_from_dir(void)
{
    if (chihiro_game_filename[0] || !chihiro_game_dir[0]) {
        return;
    }
    char bootid_path[1100];
    snprintf(bootid_path, sizeof(bootid_path), "%s/boot.id", chihiro_game_dir);
    FILE *f = qemu_fopen(bootid_path, "rb");
    if (!f) {
        return;
    }
    uint8_t bid[CHIHIRO_BOOTID_LEN];
    char name[64];
    if (fread(bid, 1, sizeof(bid), f) == sizeof(bid) &&
        chihiro_bootid_executable(bid, name, sizeof(name))) {
        chihiro_set_game_executable(name);
    }
    fclose(f);
}

/*
 * Periodic tick: drives the cabinet's boards while a game runs, and watches
 * the SEGABOOT state machine before that.
 */
static void chihiro_diag_timer_cb(void *opaque)
{
    ChihiroLPCState *s = (ChihiroLPCState *)opaque;

    if (chihiro_game_running) {
        /* What hangs off the SC UARTs (the cabinet table), re-evaluated every
         * tick so the reader toggle takes effect live. */
        bool cards_on = g_config.chihiro.card_reader.enable;
        chihiro_hw210_enabled = cards_on &&
            chihiro_cabinet_card_reader() == CHIHIRO_CARD_HW210;
        chihiro_crp1231_enabled = cards_on &&
            chihiro_cabinet_card_reader() == CHIHIRO_CARD_CRP1231;
        /* The V257 keeps its own cadence: this tick is its clock. */
        if (chihiro_v257_global &&
            chihiro_cabinet_drive_board() == CHIHIRO_DRIVE_V257)
            v257_tick(chihiro_v257_global);
        if (chihiro_hw210_enabled)
            chihiro_card_reader_tick();
        chihiro_backup_tick();
        timer_mod(s->diag_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 16);
        return;
    }

    chihiro_segaboot_poll();
    timer_mod(s->diag_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
}

/* Starts the periodic tick, created here when a restored snapshot skipped
 * SEGABOOT, which normally creates it. */
static void chihiro_diag_timer_start(ChihiroLPCState *s, int64_t ms)
{
    if (!s->diag_timer)
        s->diag_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                     chihiro_diag_timer_cb, s);
    s->diag_armed = true;
    timer_mod(s->diag_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + ms);
}

/*
 * SEGABOOT initialisation (Ver.2.13.0, the build in the second half of the
 * flash, which is the one that boots)
 *
 * baseboard_init runs this sequence:
 *   1. UsbEnumPoll()         → polls OHCI for QC/SC USB devices → Error 02 if fail
 *   2. RegisterClassDriver() → registers AN2131 class driver    → Error 02 if fail
 *   3. ReadEEPROM(0)         → reads ic10 EEPROM via vendor 0x16
 *   4. ReadEEPROM(1)         → reads ic11 EEPROM via vendor 0x17
 *   5. InitMbcom()           → initializes mediaboard DMA communication
 *   6. return 0              → SUCCESS
 *
 * Boot state machine (CLogo::Update at VA 0x259D0; 0x2EC00 in Ver.2.00.0):
 *   0 logo fade in → 1 init → 2 logo hold → 3 wait for the media board
 *   → 4 verify the game → 5 fade out → 6 launch → 8 done.
 *   Any error jumps to 7, which is the screen that shows the code.
 *   State 3 gives up after 0x95F ticks with error 22; state 4 runs the
 *   serial, region and boot.id checks. See chihiro_segaboot_poll().
 *
 * Serial format: "%%%@-##@########" (e.g. "BEER-01A00000001")
 *   Main serial from ic10 EEPROM [0x1F10], media serial from mbcom CMD 0x0103.
 *
 * SEGABOOT runs unpatched.
 *
 * AV / video mode:
 *   SEGABOOT checks the NV2A video mode. The EEPROM's video_standard carries
 *   AV_FLAGS_HDTV_480p (0x00080000), so the kernel sets the NV2A up for
 *   31 kHz progressive scan, which the check accepts.
 *
 * === Why UsbEnumPoll must run ===
 *
 * Without UsbEnumPoll the kernel never completes the USB enumeration
 * (PortReset → GET_DESC → SET_ADDRESS → GET_CONFIG_DESC → SET_CONFIG). With no
 * SET_CONFIG, RegisterClassDriver never sets bit 0x20 in the baseboard_dev
 * flags and no JVS traffic flows: SEGABOOT runs unmodified, and the USB
 * devices of chihiro-usb.c enumerate through OHCI.
 */
static void chihiro_arm_diag_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;
    if (s->diag_armed) return;
    if (chihiro_game_running) return;

    /* Before the game: the periodic tick watches SEGABOOT, 1 s from now.
     * After a QuickReboot it is already armed and keeps its deadline. */
    s->diag_armed = true;
    if (!s->diag_timer) {
        s->diag_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                     chihiro_diag_timer_cb, s);
    }
    if (!timer_pending(s->diag_timer)) {
        timer_mod(s->diag_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
    }
}

/* Called from SMC SCRATCH write handler when value=0x04 (QuickReboot).
 * HalReturnToFirmware(QuickReboot) writes SCRATCH=0x04 before system reset.
 * Multiple QuickReboots happen: service menu exit, then game boot.
 * game_running is set again when SEGABOOT tears the USB bus down
 * (chihiro_on_ohci_bus_stop). */
void chihiro_on_quickreboot_signal(void)
{
    if (!chihiro_active) return;

    fprintf(stderr, "[%07lld] === QUICKREBOOT === game_running=%d\n",
            TS_MS, chihiro_game_running);

    /* diag_armed drops for 50 ms after each QuickReboot, which folds a
     * repeated SCRATCH=0x04 into one. The kernel's own write during its first
     * boot (about 2 s) is not told apart: it comes through here too. */
    if (!chihiro_lpc_global || !chihiro_lpc_global->diag_armed) {
        return;
    }

    chihiro_capture_game_filename_from_dir();
    chihiro_quickreboot_reset(chihiro_lpc_global);
}

/* Dolphin's window for the same board: 0x84800000-0x84818000, which is exactly
 * the 98304 bytes of every known firmware.asic. */
#define FW_UPLOAD_BASE 0x84800000u
#define FW_UPLOAD_SIZE 0x00018000u

static void chihiro_fw_upload_word(ChihiroLPCState *s, uint32_t addr, uint32_t val)
{
    uint32_t off = addr - FW_UPLOAD_BASE;
    if (off > FW_UPLOAD_SIZE - 4) return;
    if (!s->fw_upload)
        s->fw_upload = g_malloc0(FW_UPLOAD_SIZE);
    if (off == 0) {
        /* Offset 0 opens a new image. */
        memset(s->fw_upload, 0, FW_UPLOAD_SIZE);
        s->fw_upload_hi = 0;
        fprintf(stderr, "Chihiro: ASIC firmware upload begins at 0x%08X\n",
                addr);
    }
    stl_le_p(s->fw_upload + off, val);
    if (off + 4 > s->fw_upload_hi) s->fw_upload_hi = off + 4;
}

/* The host released the ASIC's CPU: the board decrypts and runs the upload,
 * SEGABOOT's copy of the flash firmware or a game's own firmware.asic. */
static void chihiro_fw_upload_hand_over(ChihiroLPCState *s)
{
    if (!s->fw_upload) {
        CHIHIRO_ERRF("ASIC CPU released with no firmware uploaded\n");
        return;
    }
    chihiro_asic_load_firmware(s->fw_upload, s->fw_upload_hi,
                               "the host upload");
}

/* The Type-1 board's answer to a DIMM command (a Type-3's V850 answers for
 * itself): 16-bit words, resp[0] = seq, resp[1] = cmd | 0x8000, then data. */
static void chihiro_dimm_process_cmd(ChihiroLPCState *s)
{
    uint16_t seq = s->dimm_cmd[0] & 0xFFFF;
    uint16_t cmd = (s->dimm_cmd[0] >> 16) & 0xFFFF;

    memset(s->dimm_resp, 0, sizeof(s->dimm_resp));
    s->dimm_resp[0] = seq;
    s->dimm_resp[1] = cmd | 0x8000;

    switch (cmd) {
    case MB_CMD_INIT:
        s->dimm_resp[2] = mediaboard.dimm_size;
        break;
    case MB_CMD_STATUS:
        s->dimm_resp[2] = mediaboard.status;
        s->dimm_resp[3] = mediaboard.progress;
        break;
    case MB_CMD_GET_VERSION:
        s->dimm_resp[2] = mediaboard.fw_version;
        break;
    case MB_CMD_SYSTEM_TYPE: /* low byte must be >=2 to pass board check */
        /* Not read on the boots logged (HOTD3, Crazy Taxi): SEGABOOT asks
         * through the IDE mailbox, answered board_type | fw << 8 by
         * chihiro_mbcom_process, and the games do not ask. */
        s->dimm_resp[2] = 0x8002;
        break;
    case MB_CMD_GET_SERIAL:
        memcpy(&s->dimm_resp[2], mediaboard.serial, 16);
        break;
    default:
        break;
    }
}


/* 0x80000140, the ASIC's CPU latch. SEGABOOT and acLib upload the firmware
 * only while bit 0 reads 0, then write 1 to release the core; nothing ever
 * writes 0 back (PROVEN, code). While the V850 is held the SADDR window
 * reads the media board flash, whose word at 0x140 has bit 0 clear
 * (0x000E9EA0 in fpr21042, as a probe on a real board read; LIKELY taken
 * before SEGABOOT). Maximum Tune 1 and 2, Gundam and MJ3 ship no
 * firmware.asic (MEASURED), so the core runs on across the game handover and
 * QuickReboot (LIKELY). The latch is the host's: a core that stops after its
 * release still reads 1 (LIKELY). Type-1 has no V850. */
static uint32_t chihiro_asic_latch(ChihiroLPCState *s)
{
    if (!chihiro_is_type3() || chihiro_asic_released()) {
        return s->asic_cpu_ctrl;
    }
    if (chihiro_flash_rom && chihiro_flash_rom_size >= 0x144) {
        return ldl_le_p(chihiro_flash_rom + 0x140);
    }
    return 0;
}

/* The network device on the media board: present (bit 0 clear); the NetDIMM
 * mailbox answers it. */
static uint32_t chihiro_pci_stat_value(void)
{
    return 0x00;
}

static uint64_t chihiro_lpc_io_read(void *opaque, hwaddr addr,
                                    unsigned size)
{
    uint64_t r = 0;

    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(opaque);
    s->host_seen = true;

    if (chihiro_game_running) {
        static int game_lpc_read_log = 0;
        if (lpc_log_verbose && game_lpc_read_log < 500) {
            game_lpc_read_log++;
            fprintf(stderr, "[%07lld] GAME LPC READ port=0x%04X (reg=0x%08X)\n",
                   TS_MS, (int)(0x4000 + addr), s->lpc_reg_addr);
        }
    }

    switch (addr) {
    case 0x00: /* Port 0x4000: read baseboard register at lpc_reg_addr */
        switch (s->lpc_reg_addr) {
        case 0x80000140:
            r = chihiro_asic_latch(s);
            if (chihiro_game_running) {
                static int cpu_rdy_log = 0;
                if (lpc_log_verbose && cpu_rdy_log < 30) { cpu_rdy_log++;
                    fprintf(stderr, "[%07lld] GAME READ 0x80000140 → 0x%08X (cpu_ctrl)\n",
                            TS_MS, (unsigned)r); }
            }
            break;
        case 0x80000160: r = chihiro_pci_stat_value(); break;
        case 0x80000164: r = 0x01; break;
        /* DMA control register (acLib indexes the table {0,1,2,4,8,0x10,0x20});
         * the game writes 8 here. We answer a constant instead of latching. */
        case 0xA0001E60: r = 0x00000002; break;
        case 0xA0000000: {
            /* Indirect read: return value at address in bb_reg_addr (0xA0000020) */
            uint32_t target = s->bb_reg_addr;
            if (target == 0x80000140) {
                r = chihiro_asic_latch(s);
            } else if (target == 0x80000160) {
                r = chihiro_pci_stat_value();
                if (chihiro_game_running) {
                    static int pcistat_log = 0;
                    if (lpc_log_verbose && pcistat_log < 50) { pcistat_log++;
                        fprintf(stderr, "[%07lld] GAME SADDR READ 0x80000160 → 0x%02X "
                                "(network %s)\n", TS_MS, (unsigned)r,
                                (r & 1) ? "absent" : "present"); }
                }
            } else if (target == 0x80000164) {
                r = 0x01;
            } else if (target >= 0x84000000 && target <= 0x8400001C) {
                static int saddr_resp_read_count = 0;
                saddr_resp_read_count++;
                if (lpc_log_verbose && saddr_resp_read_count <= 500) {
                    fprintf(stderr, "[%07lld] SADDR READ 0x%08X (resp buf) → 0x%08X\n",
                            TS_MS, target, s->dimm_resp[(target - 0x84000000) / 4]);
                }
                uint32_t idx = (target - 0x84000000) / 4;
                r = s->dimm_resp[idx];
            } else if (target == 0xA0001E60) {
                r = 0x00000002; /* DMA control register — see above */
                static int a1e60_log = 0;
                if (lpc_log_verbose && a1e60_log < 20) { a1e60_log++;
                    fprintf(stderr, "[%07lld] SADDR READ 0xA0001E60 → 0x%08X (DMA ctrl)\n", TS_MS, (unsigned)r); }
            } else {
                static int unknown_saddr_log = 0;
                if (lpc_log_verbose && unknown_saddr_log < 200) {
                    fprintf(stderr, "[%07lld] SADDR READ UNKNOWN: bb_reg=0x%08X → 0\n",
                            TS_MS, target);
                    unknown_saddr_log++;
                }
                r = 0;
            }
            s->bb_reg_addr += 4;  /* auto-increment */
            break;
        }
        case 0xA0000020: r = s->bb_reg_addr; break;
        case 0xA0000040: r = s->bb_reg_status; break;
        case 0x90000000:
            /* shared memory status = ready; the read is also the acLib's
             * doorbell for the network board (setIntReadStatus) */
            r = 0x01;
            chihiro_netboard_host_ring();
            break;
        default:
            if (s->lpc_reg_addr == NETDIMM_RESP_BASE) {
                static int nd_r = 0;
                if (chihiro_netboard_present()) {
                    uint32_t pkt[8];
                    chihiro_netboard_host_response(pkt, sizeof(pkt));
                    r = pkt[chihiro_netdimm_resp_idx & 7];
                } else {
                    r = chihiro_netdimm_resp[chihiro_netdimm_resp_idx & 7];
                }
                if (lpc_log_verbose && nd_r < 20) { nd_r++;
                    fprintf(stderr, "[%07lld] NETDIMM response read [%d] -> %08X\n",
                            TS_MS, chihiro_netdimm_resp_idx, (unsigned)r); }
                if (chihiro_netdimm_resp_idx < 7) chihiro_netdimm_resp_idx++;
            } else if (s->lpc_reg_addr == NETDIMM_CMD_BASE) {
                /* The game reads the command window to see it is free, then
                 * writes the eight words. That read is the start of a message.
                 * With the board there, the word is the slot as it stands:
                 * the firmware zeroes it when it has taken the command. */
                if (chihiro_netboard_present()) {
                    uint32_t pkt[8];
                    chihiro_netboard_host_command_slot(pkt, sizeof(pkt));
                    r = pkt[0];
                } else {
                    r = 0;
                }
                chihiro_netdimm_cmd_idx = 0;
            } else {
                r = 0;
            }
            break;
        }
        if (lpc_log_verbose) {
            static int lpc_read_log = 0;
            if (lpc_read_log < 500) {
                fprintf(stderr, "[%07lld] LPC REG READ [0x%08X] -> 0x%08X\n", TS_MS,
                       s->lpc_reg_addr, (unsigned)r);
                lpc_read_log++;
            }
        }
        return r;
    case SEGA_DIMM_BASE_LO:
        /* SEGABOOT reads these once for DIMM base address calculation.
         * Game XBE reads them again and checks for "XBAM" signature.
         * First read pair returns DIMM base, subsequent reads return XBAM. */
        if (s->lpc_401e_reads > 0) {
            r = 0x4258;     /* "XB" — game checks CONCAT22(0x4020,0x401E) == "XBAM" */
        } else {
            r = 0x0000;     /* DIMM base address low word for SEGABOOT */
        }
        s->lpc_401e_reads++;
        break;
    case SEGA_XBAM_STRING_0:
        if (s->lpc_401e_reads > 1) {
            r = 0x4D41;     /* "AM" — completes "XBAM" at 0x401E-0x4020 */
        } else {
            r = 0x0100;     /* DIMM base address high word for SEGABOOT */
        }
        break;
    case SEGA_XBAM_STRING_1:
        r = 0x4258;     /* "BX" */
        break;
    case SEGA_XBAM_STRING_2:
        r = 0x4D41;     /* "MA" → full string reads as "XBAM" */
        break;
    case SEGA_CHIP_REVISION:
        /* SEGABOOT checks the high byte: 0 (FPGA) = Type-1, non-zero (ASIC) =
         * Type-3. acLib v0.71+ (WMMT1, Type-3) picks device type '!' or ')'
         * from it; ')' triggers the SC search and the EEPROM version read.
         * The FPGA answers revision 1. */
        r = chihiro_is_type3() ? SEGA_CHIP_REVISION_ASIC_CHIP_ID
                               : (SEGA_CHIP_REVISION_FPGA_CHIP_ID | 0x01);
        { static int f0_log = 0; if (lpc_log_verbose && f0_log < 500) { f0_log++;
            fprintf(stderr, "[%07lld] F0 READ → 0x%04X (type3=%d)\n",
                    TS_MS, (unsigned)r, chihiro_board_type3); } }
        break;
    case SEGA_DIMM_SIZE:
        r = mediaboard.dimm_factor;     /* JP1/JP2 jumpers. The kernel lays the disk
                                         * out from this, and so do we: see
                                         * chihiro_mbcom_base() and the IDENTIFY. */
        if (chihiro_is_type3())
            r |= 0x40;  /* Type-3: dip switch register on media board */
        break;
    case 0x26:  /* Port 0x4026: scratch register (read-write) */
        r = s->lpc_scratch_4026;
        break;
    case 0xE0: {
        r = s->mbcom_e0_status;
        static int e0_log = 0; if (lpc_log_verbose && e0_log < 2000) { e0_log++;
            fprintf(stderr, "[%07lld] E0 READ → 0x%02X (bit0=%d bit2=%d)\n",
                    TS_MS, (unsigned)r, (int)(r & 1), (int)((r >> 2) & 1)); }
        break;
    }
    case 0x84:  /* Port 0x4084 — baseboard status word (acLib names it Status) */
        r = 0x0000;
        break;
    default: {
        static int unknown_port_log = 0;
        if (lpc_log_verbose && unknown_port_log < 30) {
            fprintf(stderr, "[%07lld] LPC READ UNKNOWN port 0x%04X → 0\n",
                    TS_MS, (unsigned)(addr + 0x4000));
            unknown_port_log++;
        }
        break;
    }
    }

    return r;
}

static void chihiro_mbcom_init(void);
static void chihiro_mbcom_process(void);

/* A message from the board is in 0x84000000: E0 bit 0 and IRQ 10, 2 ms later
 * once the game runs. */
static void chihiro_dimm_raise_message(ChihiroLPCState *s)
{
    if (chihiro_game_running && s->dimm_resp_timer) {
        timer_mod(s->dimm_resp_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
    } else {
        s->mbcom_e0_status |= 0x01;
        qemu_irq_lower(s->irq10);
        qemu_irq_raise(s->irq10);
    }
}

void chihiro_dimm_board_posted(const uint32_t *msg8)
{
    ChihiroLPCState *s = chihiro_lpc_global;
    static int late_log;

    if (!s) {
        return;
    }
    if ((msg8[0] & 0x80000000u) && late_log < 8) {
        late_log++;
        CHIHIRO_ERRF("cmd %04X: the V850 answered late\n",
                     (msg8[0] >> 16) & 0x7FFF);
    }
    memcpy(s->dimm_resp, msg8, sizeof(s->dimm_resp));
    chihiro_dimm_raise_message(s);
}

/* The host rang the media board: a command, or its reply to one of the
 * board's own. The board's next message, when it comes at once, is raised
 * here; later ones come through chihiro_dimm_board_posted(). A silent board
 * raises nothing, as on a cabinet (SEGABOOT: Error 22 after about 40 s, or a
 * frozen logo while it waits on 0x0100). */
static bool chihiro_dimm_board_answer(ChihiroLPCState *s)
{
    static int miss_log;
    uint32_t opcode = (s->dimm_cmd[0] >> 16) & 0xFFFF;
    uint32_t msg[8];

    if (chihiro_asic_mailbox_exchange(s->dimm_cmd, msg)) {
        /* State 5 is ready; any other means the board waits for the game to
         * be loaded into the DIMM, which is not modelled. Said once. */
        static bool busy_said;
        if (opcode == 0x0100 && msg[0] == (s->dimm_cmd[0] | 0x80000000u) &&
            msg[1] != 5 && !busy_said) {
            busy_said = true;
            CHIHIRO_ERRF("the media board reports state %u at %u%%: it waits "
                         "for the game to be loaded into the DIMM, not "
                         "modelled\n", msg[1], msg[2]);
        }
        memcpy(s->dimm_resp, msg, sizeof(msg));
        return true;
    }
    /* A reply to one of the board's own commands has no answer. */
    if (!(s->dimm_cmd[0] & 0x80000000u) && miss_log < 8) {
        miss_log++;
        CHIHIRO_ERRF("cmd %04X: the V850 %s\n", opcode,
                     chihiro_asic_running() ? "has not answered yet" :
                                              "is not running");
    }
    return false;
}

/* A word the game writes through the SADDR window, in a burst or alone, into
 * the media board's mailbox or its firmware upload. A command is only stored:
 * the game runs it with a write to 0x84000040 once all of it is in. */
static void chihiro_saddr_store(ChihiroLPCState *s, uint32_t wa, uint32_t val)
{
    /* The board shares this window: it has to see the write now. */
    if (wa >= 0x84000000 && wa <= 0x8400003C) {
        chihiro_asic_host_mailbox_write(wa - 0x84000000, val);
    }
    if (wa >= 0x84000020 && wa <= 0x8400003C) {
        s->dimm_cmd[(wa - 0x84000020) / 4] = val;
    } else if (wa >= 0x84000000 && wa <= 0x8400001C) {
        uint32_t idx = (wa - 0x84000000) / 4;
        s->dimm_resp[idx] = val;
        if (idx == 0 && val == 0) {
            memset(s->dimm_resp, 0, sizeof(s->dimm_resp));
        }
    } else if (wa >= FW_UPLOAD_BASE && wa < FW_UPLOAD_BASE + FW_UPLOAD_SIZE) {
        chihiro_fw_upload_word(s, wa, val);
    }
}

static void chihiro_lpc_io_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(opaque);
    s->host_seen = true;

    if (chihiro_game_running) {
        static int game_lpc_write_log = 0;
        if (lpc_log_verbose && game_lpc_write_log < 300) {
            game_lpc_write_log++;
            fprintf(stderr, "[%07lld] GAME LPC WRITE port=0x%04X val=0x%08X\n",
                   TS_MS, (int)(0x4000 + addr), (unsigned)val);
        }
    }

    switch (addr) {
    case 0x00: /* Port 0x4000: write to baseboard register at lpc_reg_addr */
        if (s->lpc_reg_addr == NETDIMM_CMD_BASE) {
            if (chihiro_netdimm_cmd_idx < 8) {
                chihiro_netdimm_cmd[chihiro_netdimm_cmd_idx++] = (uint32_t)val;
                if (chihiro_netdimm_cmd_idx == 8) {
                    if (chihiro_netboard_present()) {
                        chihiro_link_watch_command(chihiro_netdimm_cmd);
                        chihiro_netboard_host_command(chihiro_netdimm_cmd,
                                                      sizeof(chihiro_netdimm_cmd));
                    } else {
                        chihiro_netdimm_answer();
                    }
                }
            }
        } else if (s->lpc_reg_addr == NETDIMM_RESP_BASE) {
            memset(chihiro_netdimm_resp, 0, sizeof(chihiro_netdimm_resp));
            chihiro_netdimm_resp_idx = 0;
            chihiro_netdimm_cmd_idx = 0;
            chihiro_netboard_host_release();
        }
        switch (s->lpc_reg_addr) {
        case 0xA0000020: /* Indirect address pointer */
            s->bb_reg_addr = (uint32_t)val;
            {
                static int addr_log = 0;
                if (chihiro_game_running) {
                    static int gaddr_log = 0;
                    if (lpc_log_verbose && gaddr_log < 500) { gaddr_log++;
                        fprintf(stderr, "[%07lld] GAME SADDR ADDR: bb_reg_addr=0x%08X\n",
                                TS_MS, (unsigned)val); }
                } else if (lpc_log_verbose && addr_log < 200) {
                    fprintf(stderr, "[%07lld] SADDR ADDR: bb_reg_addr=0x%08X\n",
                            TS_MS, (unsigned)val);
                    addr_log++;
                }
            }
            break;
        case 0xA0000040: /* DMA status/enable */
            s->bb_reg_status = (uint32_t)val;
            if (val & 0x80000000) {
                s->bb_dma_active = true;
                s->bb_dma_count = 0;
                static int burst_on_log = 0;
                if (lpc_log_verbose && burst_on_log < 10) {
                    fprintf(stderr, "[%07lld] SADDR BURST ON: target=0x%08X\n",
                            TS_MS, s->bb_reg_addr);
                    burst_on_log++;
                }
            } else {
                if (s->bb_dma_active) {
                    static int burst_off_log = 0;
                    if (lpc_log_verbose && burst_off_log < 10) {
                        fprintf(stderr, "[%07lld] SADDR BURST OFF: wrote %u dwords\n",
                                TS_MS, s->bb_dma_count);
                        burst_off_log++;
                    }
                }
                s->bb_dma_active = false;
            }
            break;
        case 0xA0000000: /* Data write to address in bb_reg_addr */
            if (s->bb_dma_active) {
                uint32_t wa = s->bb_reg_addr;
                {
                    static int burst_data_log = 0;
                    if (lpc_log_verbose && burst_data_log < 40) {
                        fprintf(stderr, "[%07lld] SADDR BURST DATA: [0x%08X] <- 0x%08X (dma#%u)\n",
                                TS_MS, wa, (unsigned)val, s->bb_dma_count);
                        burst_data_log++;
                    }
                }
                chihiro_saddr_store(s, wa, (uint32_t)val);
                s->bb_dma_count++;
                s->bb_reg_addr += 4;  /* auto-increment */
            } else {
                /* Register-mode: single-word write to target address */
                uint32_t wa = s->bb_reg_addr;
                static int regmode_log_count = 0;
                if (lpc_log_verbose && regmode_log_count < 200) {
                    fprintf(stderr, "[%07lld] SADDR REG-WRITE: target=0x%08X val=0x%08X\n",
                            TS_MS, wa, (unsigned)val);
                    regmode_log_count++;
                }
                if ((wa >= 0x84000000 && wa <= 0x8400003C) ||
                    (wa >= FW_UPLOAD_BASE &&
                     wa < FW_UPLOAD_BASE + FW_UPLOAD_SIZE)) {
                    chihiro_saddr_store(s, wa, (uint32_t)val);
                } else if (wa == 0x80000140) {
                    s->asic_cpu_ctrl = (uint32_t)val;
                    /* Bit 0 releases the V850, and the image is complete by
                     * the time it is written. A released core keeps its own. */
                    if ((val & 1) && !chihiro_asic_released()) {
                        chihiro_fw_upload_hand_over(s);
                    }
                    chihiro_asic_set_running(val & 1);
                } else if (wa == 0xA0001E60) {
                    if (lpc_log_verbose) fprintf(stderr, "[%07lld] SADDR WRITE 0xA0001E60 <- 0x%08X (fw state)\n",
                            TS_MS, (unsigned)val);
                } else if (wa == 0x84000040) {
                    /* ASIC execute trigger */
                    if (lpc_log_verbose) fprintf(stderr, "[%07lld] SADDR REG-MODE: 0x84000040 <- 0x%08X\n",
                            TS_MS, (unsigned)val);
                    if (val & 1) {
                        static int exec_dump = 0;
                        static int game_exec_log = 0;
                        bool answered = true;

                        if (lpc_log_verbose && exec_dump < 30) {
                            fprintf(stderr, "[%07lld] EXEC dimm_cmd: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                                    TS_MS,
                                    s->dimm_cmd[0], s->dimm_cmd[1], s->dimm_cmd[2], s->dimm_cmd[3],
                                    s->dimm_cmd[4], s->dimm_cmd[5], s->dimm_cmd[6], s->dimm_cmd[7]);
                            exec_dump++;
                        }
                        if (chihiro_is_type3()) {
                            answered = chihiro_dimm_board_answer(s);
                        } else {
                            chihiro_dimm_process_cmd(s);
                        }
                        if (lpc_log_verbose && chihiro_game_running && game_exec_log < 50) {
                            fprintf(stderr, "[%07lld] GAME-EXEC #%d: cmd=%08X → resp: %08X %08X %08X %08X\n",
                                    TS_MS, game_exec_log,
                                    s->dimm_cmd[0], s->dimm_resp[0], s->dimm_resp[1],
                                    s->dimm_resp[2], s->dimm_resp[3]);
                            game_exec_log++;
                        }
                        if (answered) {
                            chihiro_dimm_raise_message(s);
                        }
                    }
                }
            }
            break;
        }
        return;
    case 0x04: /* Port 0x4004: set register address */
        s->lpc_reg_addr = (uint32_t)val;
        return;
    case 0x08: /* Port 0x4008: reset cycle */
        return;
    case 0x26:  /* Port 0x4026: scratch register */
        s->lpc_scratch_4026 = (uint16_t)val;
        break;
    case SEGA_IRQ10_ACK: { /* 0xE0 — ack: clear specific bits */
        uint8_t old_e0 = s->mbcom_e0_status;
        if (val & 0x04)
            chihiro_netboard_host_ack();
        s->mbcom_e0_status &= ~(uint8_t)val;
        if (s->mbcom_e0_status == 0)
            qemu_irq_lower(s->irq10);
        static int e0w_log = 0;
        if (lpc_log_verbose && e0w_log < 100) { e0w_log++;
            fprintf(stderr, "[%07lld] E0 WRITE val=0x%02X e0=0x%02X→0x%02X%s\n",
                    TS_MS, (unsigned)(uint8_t)val, old_e0, s->mbcom_e0_status,
                    s->mbcom_e0_status == 0 ? " irq10↓" : ""); }
        break;
    }
    case 0xE2:            /* 0x40E2 — IRQ10 deassert only, do NOT clear data ready */
        qemu_irq_lower(s->irq10);
        break;
    case 0xE1: {          /* 0x40E1 — mbcom trigger / IRQ10 deassert
                           * Protocol (from vsg.xbe FUN_0014c120 + DPC at 0x14c0e0):
                           *   DPC writes E1=0xF (ARM)
                           *   Worker checks scratch bit 8: set=busy, clear=ready
                           *   Worker writes cmd to DMA (FC801), then E1=0xF, E1=0
                           *   E1=0 = "process the DMA command" (not scratch!)
                           * scratch 0x0102 = status register (bit8=busy), NOT a command */
        static int e1_log = 0;
        if (val != 0) {
            if (!chihiro_is_type3() && !chihiro_game_running &&
                (chihiro_mbcom_command[0] != 0 || chihiro_mbcom_command[1] != 0)) {
                chihiro_mbcom_process();
                s->mbcom_e0_status |= 0x01;
                qemu_irq_lower(s->irq10);
                qemu_irq_raise(s->irq10);
            } else {
                /* ARM for E1=0 trigger (game mode or SEGABOOT ack) */
                chihiro_e1_armed = true;
                if (lpc_log_verbose && e1_log < 2000) { e1_log++;
                    fprintf(stderr, "[%07lld] E1=0x%X ARM scratch=0x%04X dma=%02X%02X\n",
                            TS_MS, (unsigned)val, s->lpc_scratch_4026,
                            chihiro_mbcom_command[0], chihiro_mbcom_command[1]); }
            }
        } else {
            if (chihiro_game_running && chihiro_is_type3() && chihiro_e1_armed) {
                /* ')' mode (Type-3 game): E1=0 is the game worker's
                 * confirmation that it processed the ARM cycle. Clear E0
                 * so the worker can proceed to read SADDR. */
                static bool t3_worker_logged;
                if (!t3_worker_logged) {
                    t3_worker_logged = true;
                    if (lpc_log_verbose) fprintf(stderr, "[%07lld] T3 WORKER ALIVE\n", TS_MS);
                }
                s->mbcom_e0_status &= ~0x01;
                qemu_irq_lower(s->irq10);

                if (lpc_log_verbose && e1_log < 2000) { e1_log++;
                    fprintf(stderr, "[%07lld] E1=0 T3_ACK e0=0x%02X irq10↓\n",
                            TS_MS, s->mbcom_e0_status); }
            } else if (chihiro_e1_armed && s->mbcom_resp_ready) {
                /* RESP_DELIVER ('!' mode / SEGABOOT) */
                s->mbcom_resp_ready = false;
                qemu_irq_lower(s->irq10);
                qemu_irq_raise(s->irq10);
                if (lpc_log_verbose && e1_log < 2000) { e1_log++;
                    fprintf(stderr, "[%07lld] E1=0 RESP_DELIVER scratch=0x%04X resp: %02X%02X %02X%02X\n",
                            TS_MS, s->lpc_scratch_4026,
                            chihiro_mbcom_response[0], chihiro_mbcom_response[1],
                            chihiro_mbcom_response[2], chihiro_mbcom_response[3]); }
            } else if (!chihiro_is_type3() && chihiro_e1_armed &&
                       (chihiro_mbcom_command[0] != 0 ||
                        chihiro_mbcom_command[1] != 0)) {
                /* DMA_PROCESS ('!' mode / SEGABOOT) */
                const uint8_t *w = chihiro_mbcom_command;
                uint16_t seq = w[0] | (w[1] << 8);
                uint16_t cmd = w[2] | (w[3] << 8);
                s->dimm_cmd[0] = seq | ((uint32_t)cmd << 16);
                memset(&s->dimm_cmd[1], 0, 7 * sizeof(uint32_t));
                chihiro_dimm_process_cmd(s);
                chihiro_mbcom_process();
                memset(chihiro_mbcom_command, 0, 32);
                s->mbcom_e0_status |= 0x01;
                s->mbcom_resp_ready = true;
                s->lpc_scratch_4026 &= ~0x0100;
                qemu_irq_lower(s->irq10);
                qemu_irq_raise(s->irq10);
                if (lpc_log_verbose && e1_log < 2000) { e1_log++;
                    fprintf(stderr, "[%07lld] E1=0 DMA_PROCESS scratch=0x%04X resp: %02X%02X %02X%02X %02X%02X%02X%02X\n",
                            TS_MS, s->lpc_scratch_4026,
                            chihiro_mbcom_response[0], chihiro_mbcom_response[1],
                            chihiro_mbcom_response[2], chihiro_mbcom_response[3],
                            chihiro_mbcom_response[4], chihiro_mbcom_response[5],
                            chihiro_mbcom_response[6], chihiro_mbcom_response[7]); }
            } else {
                s->lpc_scratch_4026 &= ~0x0100;
                qemu_irq_lower(s->irq10);
                if (lpc_log_verbose && e1_log < 2000) { e1_log++;
                    fprintf(stderr, "[%07lld] E1=0 LOWER armed=%d scratch=0x%04X\n",
                            TS_MS, chihiro_e1_armed, s->lpc_scratch_4026); }
            }
            chihiro_e1_armed = false;
        }
        break;
    }
    default:
        break;
    }
}

static const MemoryRegionOps chihiro_lpc_io_ops = {
    .read = chihiro_lpc_io_read,
    .write = chihiro_lpc_io_write,
    .impl = {
        .min_access_size = 2,
        .max_access_size = 4,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* The baseboard EEPROM dumps: ic10 (QC firmware), ic11 (settings), pc20 (SC
 * firmware). */
uint8_t *chihiro_ic10_data = NULL;
uint32_t chihiro_ic10_size = 0;
uint8_t *chihiro_ic11_data = NULL;
uint32_t chihiro_ic11_size = 0;
uint8_t *chihiro_pc20_data = NULL;
uint32_t chihiro_pc20_size = 0;

const uint8_t *chihiro_flash_rom_bytes(uint32_t *size)
{
    if (size) *size = chihiro_flash_rom_size;
    return chihiro_flash_rom;
}

bool chihiro_flash_rom_loaded(void)
{
    return chihiro_flash_rom != NULL;
}

int chihiro_detected_game_profile(void)
{
    const ChihiroCabinet *c = chihiro_cabinet();

    return c ? c->profile : -1;
}

static bool chihiro_read_flash_rom(const char *path)
{
    FILE *f = qemu_fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0 && sz <= 4 * 1024 * 1024) {
        chihiro_flash_rom = (uint8_t *)g_malloc0(sz);
        if (fread(chihiro_flash_rom, 1, sz, f) == (size_t)sz) {
            chihiro_flash_rom_size = sz;
        } else {
            g_free(chihiro_flash_rom);
            chihiro_flash_rom = NULL;
        }
    }
    fclose(f);
    return chihiro_flash_rom != NULL;
}

/* The Chihiro BIOS listed by MAME (chihiro_xbox_bios.bin). */
bool chihiro_bios_known(const char *path)
{
    g_autofree gchar *data = NULL;
    gsize len = 0;

    if (!g_file_get_contents(path, &data, &len, NULL) || len != 0x80000) {
        return false;
    }
    g_autofree gchar *sha1 = g_compute_checksum_for_data(
        G_CHECKSUM_SHA1, (const guchar *)data, len);
    return !strcmp(sha1, "b700b0041af8f84835e45d1d1250247bf7077188");
}

/* Explicit path from Settings > System > Chihiro Files wins; otherwise look
 * for the known file names next to the BIOS. */
void chihiro_load_flash_rom(const char *bios_path)
{
    if (chihiro_flash_rom) return; /* already loaded */

    const char *configured = g_config.chihiro.roms.mediaboard_path;
    if (configured && configured[0]) {
        if (chihiro_read_flash_rom(configured)) return;
        fprintf(stderr, "Chihiro: cannot read media board flash '%s'\n",
                configured);
    }
    if (!bios_path[0]) {
        return;
    }

    char dir[1024] = {0};
    const char *last_sep = strrchr(bios_path, '/');
    if (!last_sep) last_sep = strrchr(bios_path, '\\');
    if (last_sep) {
        int dir_len = last_sep - bios_path + 1;
        if (dir_len < (int)sizeof(dir)) {
            memcpy(dir, bios_path, dir_len);
        }
    }

    const char *flash_names[] = {
        "fpr21042_m29w160et.bin",
        "fpr-23887_29lv160te.ic4",
        "fpr-23887.bin",
        NULL
    };

    for (int i = 0; flash_names[i]; i++) {
        char path[2048];
        snprintf(path, sizeof(path), "%s%s", dir, flash_names[i]);
        if (chihiro_read_flash_rom(path)) return;
        snprintf(path, sizeof(path), "%s../%s", dir, flash_names[i]);
        if (chihiro_read_flash_rom(path)) return;
    }
}

static uint8_t *load_eeprom_file(const char *dir, const char *name,
                                 uint32_t expected_size, uint32_t *out_size)
{
    char path[2048];
    snprintf(path, sizeof(path), "%s%s", dir, name);
    FILE *f = qemu_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (expected_size && (uint32_t)sz != expected_size)) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)g_malloc0(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) {
        g_free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (uint32_t)sz;
    return buf;
}

/* Explicit paths from Settings > System > Chihiro Files win; otherwise look
 * for the known file names next to the BIOS. */
static uint8_t *load_eeprom_configured(const char *configured, const char *dir,
                                       const char *name, uint32_t expected_size,
                                       uint32_t *out_size)
{
    if (configured && configured[0]) {
        uint8_t *buf = load_eeprom_file("", configured, expected_size, out_size);
        if (buf) return buf;
        fprintf(stderr, "Chihiro: cannot read EEPROM '%s' (expected %u bytes)\n",
                configured, expected_size);
    }
    return dir ? load_eeprom_file(dir, name, expected_size, out_size) : NULL;
}

void chihiro_load_eeproms(const char *bios_path)
{
    static bool loaded;
    if (loaded) return;
    loaded = true;

    char dir[1024] = {0};
    const char *last_sep = strrchr(bios_path, '/');
    if (!last_sep) last_sep = strrchr(bios_path, '\\');
    if (last_sep) {
        int dir_len = last_sep - bios_path + 1;
        if (dir_len < (int)sizeof(dir))
            memcpy(dir, bios_path, dir_len);
    }

    /* Without a BIOS, only the files set in the settings are used. */
    const char *next_to = bios_path[0] ? dir : NULL;
    chihiro_ic10_data = load_eeprom_configured(g_config.chihiro.roms.ic10_path,
                                               next_to, "ic10_g24lc64.bin",
                                               8192, &chihiro_ic10_size);
    chihiro_ic11_data = load_eeprom_configured(g_config.chihiro.roms.ic11_path,
                                               next_to, "ic11_24lc024.bin",
                                               128, &chihiro_ic11_size);
    chihiro_pc20_data = load_eeprom_configured(g_config.chihiro.roms.pc20_path,
                                               next_to, "pc20_g24lc64.bin",
                                               8192, &chihiro_pc20_size);

    printf("Chihiro: EEPROMs from disk: ic10=%s (%uB), ic11=%s (%uB), pc20=%s (%uB)\n",
           chihiro_ic10_data ? "OK" : "MISSING", chihiro_ic10_size,
           chihiro_ic11_data ? "OK" : "MISSING", chihiro_ic11_size,
           chihiro_pc20_data ? "OK" : "MISSING", chihiro_pc20_size);
}

static void chihiro_dimm_resp_timer_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;
    s->mbcom_e0_status |= 0x01; /* bit 0 only: ')' mode → status=2 */
    qemu_irq_lower(s->irq10);
    qemu_irq_raise(s->irq10);
}

/* Guest memory by virtual address, one page at a time. */
static bool chihiro_guest_rw(uint32_t va, void *buf, uint32_t len, bool write)
{
    uint8_t *p = buf;

    while (len) {
        uint32_t pa = chihiro_va_to_pa(va);
        uint32_t run = 0x1000 - (va & 0xFFF);

        if (run > len) {
            run = len;
        }
        if (pa == 0xFFFFFFFF || (uint64_t)pa + run > 0x08000000u) {
            return false;
        }
        if (write) {
            cpu_physical_memory_write(pa, p, run);
        } else {
            cpu_physical_memory_read(pa, p, run);
        }
        va += run;
        p += run;
        len -= run;
    }
    return true;
}

/* HACK: a game's own bug corrected in its memory at the first USB bus start,
 * only where the whole signature matches (the image, whose digests the kernel
 * checks, is never touched).
 * Gundam B.O.S. checks the network firmware version as two numbers, the first
 * at least 12 AND the second at least 09 (gs.xbe 0x6499A: cmp cl,0x12 / jc;
 * cmp al,9 / jnc), so 13.05, the only dumped firmware, gets "Error 14", as on
 * a real cabinet on 13.05. The second jump (0F 83) becomes nop / jmp (90 E9).
 * The game is patched rather than the firmware: 13.05 is the only dump, and it
 * stays as dumped. */
typedef struct {
    const char    *xbe;         /* chihiro_game_filename */
    uint32_t       va;          /* where the bytes live in the running game */
    const uint8_t *expect;      /* what must be there */
    const uint8_t *replace;     /* what goes there instead */
    uint32_t       len;
    const char    *why;
} ChihiroGamePatch;

static const uint8_t chihiro_gundam_check[] = {
    0x3C, 0x09, 0x0F, 0x83, 0xDE, 0x00, 0x00, 0x00
};
static const uint8_t chihiro_gundam_patched[] = {
    0x3C, 0x09, 0x90, 0xE9, 0xDE, 0x00, 0x00, 0x00
};

static const ChihiroGamePatch chihiro_game_patches[] = {
    { "gs.xbe", 0x0006499F, chihiro_gundam_check, chihiro_gundam_patched,
      sizeof(chihiro_gundam_check),
      "Gundam reads the network firmware version as two separate numbers and "
      "refuses 13.05; its second compare no longer decides" },
};

static void chihiro_patch_running_game(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(chihiro_game_patches); i++) {
        const ChihiroGamePatch *gp = &chihiro_game_patches[i];
        uint8_t have[16];

        if (g_ascii_strcasecmp(chihiro_game_filename, gp->xbe) != 0 ||
            gp->len > sizeof(have)) {
            continue;
        }
        if (!chihiro_guest_rw(gp->va, have, gp->len, false)) {
            fprintf(stderr, "Chihiro: %s is not mapped at %08X yet, its patch "
                    "is skipped\n", gp->xbe, gp->va);
            continue;
        }
        if (memcmp(have, gp->replace, gp->len) == 0) {
            continue;                       /* already there */
        }
        if (memcmp(have, gp->expect, gp->len) != 0) {
            fprintf(stderr, "Chihiro: %s is not the revision the patch knows, "
                    "left alone\n", gp->xbe);
            continue;
        }
        if (chihiro_guest_rw(gp->va, (void *)gp->replace, gp->len, true)) {
            fprintf(stderr, "Chihiro: HACK, %s patched at %08X: %s\n",
                    gp->xbe, gp->va, gp->why);
        }
    }
}

/* The mov that carries the slot table, then the fourteen bytes behind it and
 * the add that carries the stride. */
static const uint8_t chihiro_mbcom_slot_sig[] = {
    0x33, 0xD2, 0x56, 0xEB, 0x06, 0x8D, 0x9B, 0x00, 0x00, 0x00, 0x00,
    0xF6, 0x41, 0x03, 0x80, 0x8D, 0x71, 0xE0, 0x75, 0x04, 0x38, 0x01,
    0x74, 0x12, 0x83, 0xC2
};

#define CHIHIRO_SEGABOOT_CODE_START 0x00011000u
#define CHIHIRO_SEGABOOT_CODE_END   0x00115000u
#define CHIHIRO_MBCOM_SIG_SPAN      48

static void chihiro_read_slot_table(const uint8_t *win, size_t i,
                                    ChihiroMbcomSlots *out)
{
    const uint8_t *p = win + i + 5 + sizeof(chihiro_mbcom_slot_sig);
    uint32_t stride = p[0];
    uint32_t slots, size;

    if (p[1] != 0x83 || p[2] != 0xC1 || p[3] != stride ||
        p[4] != 0x81 || p[5] != 0xFA) {
        return;
    }
    slots = ldl_le_p(win + i + 1);
    size = ldl_le_p(p + 6);
    if (stride < 0x20 || !size || size % stride || slots < 0x20) {
        return;
    }
    out->slots = slots;
    out->meta = slots - 0x20;
    out->stride = stride;
    out->count = size / stride;
}

static bool chihiro_find_mbcom_slots(void)
{
    /* A page at a time, keeping the tail of the previous one so a match that
     * straddles a page boundary is still seen. */
    uint8_t win[64 + 0x1000];
    ChihiroMbcomSlots found = { 0 };

    memset(win, 0, sizeof(win));

    for (uint32_t va = CHIHIRO_SEGABOOT_CODE_START;
         va < CHIHIRO_SEGABOOT_CODE_END; va += 0x1000) {
        memmove(win, win + 0x1000, 64);
        if (!chihiro_guest_rw(va, win + 64, 0x1000, false)) {
            memset(win + 64, 0, 0x1000);
            continue;
        }

        for (size_t i = 0; i + CHIHIRO_MBCOM_SIG_SPAN <= sizeof(win); i++) {
            if (win[i] == 0xB9 &&
                !memcmp(win + i + 5, chihiro_mbcom_slot_sig,
                        sizeof(chihiro_mbcom_slot_sig))) {
                chihiro_read_slot_table(win, i, &found);
            }
        }
        if (found.count) {
            break;
        }
    }

    if (!found.count) {
        return false;
    }
    found.known = true;
    chihiro_mbcom_slots = found;
    fprintf(stderr,
            "[%07lld] Chihiro: SEGABOOT parks its media board commands in %u "
            "slots of %u bytes at %08X, read from its own code\n",
            TS_MS, found.count, found.stride, found.slots);
    return true;
}

/* HEURISTIC safety net, not the bus: answers a command SEGABOOT parked that
 * nothing took for a whole second. */
static void chihiro_answer_mbcom_slots(ChihiroLPCState *s)
{
    static int64_t first_seen[CHIHIRO_MBCOM_SLOT_MAX];
    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    uint32_t stride = chihiro_mbcom_slots.stride;

    for (uint32_t sl = 0; sl < chihiro_mbcom_slots.count
                          && sl < CHIHIRO_MBCOM_SLOT_MAX; sl++) {
        uint32_t data_va = chihiro_mbcom_slots.slots + sl * stride;
        uint32_t meta_va = chihiro_mbcom_slots.meta + sl * stride;
        uint8_t data_byte0 = 0;
        uint16_t meta_marker = 0, cmd_opcode = 0;

        if (!chihiro_guest_rw(data_va, &data_byte0, 1, false) ||
            !chihiro_guest_rw(meta_va + 2, &meta_marker, 2, false)) {
            continue;
        }
        if (data_byte0 == 0 || meta_marker != 0) {
            first_seen[sl] = 0;
            continue;
        }
        if (!first_seen[sl]) {
            first_seen[sl] = now_ms;
        }
        if (now_ms - first_seen[sl] < CHIHIRO_MBCOM_SLOT_GRACE_MS) {
            continue;
        }
        if (!chihiro_guest_rw(data_va + 2, &cmd_opcode, 2, false)) {
            continue;
        }

        if (cmd_opcode == MB_CMD_GET_NET_PROPERTY) {
            /* No network behind the board: an all-zero property block (the window
             * reads back as zero), shown as NETWORK TYPE NONE. */
            uint32_t offset = 0;

            if (!chihiro_guest_rw(meta_va + 4, &offset, 4, true)) {
                continue;
            }
            meta_marker = 0x0001;
        } else {
            /* Failed rather than invented: SEGABOOT frees the slot and goes on. */
            meta_marker = 0x8000;
        }

        if (!chihiro_guest_rw(meta_va + 2, &meta_marker, 2, true)) {
            continue;
        }
        first_seen[sl] = 0;
        s->mbcom_e0_status |= 0x05;
        s->lpc_scratch_4026 &= ~0x0100;
        qemu_irq_raise(s->irq10);
    }
}

static void chihiro_irq10_timer_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;
    /* IRQ10 for SEGABOOT's Type-1 baseboard communication. The board only
     * signals a host that is already talking to it — the first LPC access is
     * that proof, and it is a bus event rather than a peek into guest memory.
     * A Type-3 gets no pulse: its V850 raises IRQ 10 when it answers. */
    if (s->host_seen && !chihiro_game_running && !chihiro_is_type3()) {
        qemu_irq_raise(s->irq10);
    }

    /* Type-1 game-mode mbcom bootstrap ('!' mode), once per game start: the
     * INIT reply (DIMM size, 0x8001) goes to the SADDR buffer, which the
     * games logged do not read (they use the IDE mailbox), and E0 bit 0,
     * resp_ready and an IRQ10 edge are left for the game. A Type-3 game asks
     * its V850 itself (0x0001 by EXEC). */
    if (chihiro_game_running && !chihiro_mbcom_bootstrap_done) {
        memset(chihiro_mbcom_command, 0, 32);
        if (!chihiro_is_type3()) {
            s->dimm_cmd[0] = 1 | (0x0001 << 16);
            chihiro_dimm_process_cmd(s);
            s->mbcom_e0_status |= 0x01;
            s->mbcom_resp_ready = true;
            s->lpc_scratch_4026 &= ~0x0100;
            qemu_irq_lower(s->irq10);
            qemu_irq_raise(s->irq10);
            fprintf(stderr, "[%07lld] *** GAME MBCOM BOOTSTRAP T1: 0x8001 loaded, IRQ10 edge fired ***\n", TS_MS);
        }
        chihiro_mbcom_bootstrap_done = true;
    }

    /* Pick up the media board commands SEGABOOT is spinning on (Type-1: a
     * Type-3's V850 answers them itself). */
    if (!chihiro_game_running && !chihiro_is_type3() && s->diag_armed) {
        if (!chihiro_mbcom_slots.known) {
            /* Looked for twice a second until SEGABOOT's code is mapped,
             * again after each QuickReboot; the first table found is used. */
            static unsigned tries;
            if (++tries % 32 == 1) {
                chihiro_find_mbcom_slots();
            }
        }
        if (chihiro_mbcom_slots.known) {
            chihiro_answer_mbcom_slots(s);
        }
    }

    /* Re-arm every 16ms (~60Hz) — keep running even after game detection */
    timer_mod(s->irq10_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 16);
}

static void chihiro_lpc_realize(DeviceState *dev, Error **errp)
{
    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(dev);
    ISADevice *isa = ISA_DEVICE(dev);

    chihiro_log_init();
    lpc_log_verbose = !!(chihiro_log_mask & CHIHIRO_LOG_VERBOSE);

    chihiro_active = true;
    chihiro_lpc_global = s;
    memory_region_init_io(&s->ioport, OBJECT(dev), &chihiro_lpc_io_ops, s,
                          "chihiro-lpc-io", 0x100);
    isa_register_ioport(isa, &s->ioport, 0x4000);

    setvbuf(stderr, NULL, _IONBF, 0);

    /* 0x80000140 bit 0 is the media board CPU's release. While it reads 0
     * the acLib uploads the firmware, writes 1 and returns 5 to be called
     * again (FUN_0014c580); with 1 it sets up its mailboxes. A Type-1 has no
     * V850 and reads 1; a Type-3 reads the flash through it until the host
     * releases its V850 (chihiro_asic_latch). */
    chihiro_asic_init();
    s->asic_cpu_ctrl = 1;

    /* Initialize IRQ10 for baseboard communication */
    s->irq10 = isa_get_irq(isa, 10);
    s->irq10_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                   chihiro_irq10_timer_cb, s);
    timer_mod(s->irq10_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 500);

    /* Initialize media board state (single source for all mbcom responses) */
    mediaboard_init();

    /* Initialize mbcom protocol handler */
    chihiro_mbcom_init();

    s->dimm_resp_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                       chihiro_dimm_resp_timer_cb, s);

    /* USB hotplug timers: created here, armed only when ohci_bus_start()
     * fires (chihiro_on_ohci_bus_start).
     * This ensures devices attach AFTER the kernel has enabled RHSC,
     * so fresh CSC events trigger full enumeration including SET_CONFIG.
     *
     * On real hardware: AN2131 boot ~200ms, kernel OHCI ~600ms.
     * Kernel sees devices during first scan → SET_CONFIG → CONFIGURED.
     * Here the devices attach 50 ms (QC) and 100 ms (SC) after BUS START,
     * to the same effect. */
    s->usb_hotplug_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                         chihiro_usb_hotplug_qc_cb, s);
    /* Timer NOT armed yet — will be armed by chihiro_on_ohci_bus_start() */

    s->usb_hotplug_sc_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                            chihiro_usb_hotplug_sc_cb, s);
    /* Timer NOT armed yet — will be armed by chihiro_on_ohci_bus_start() */

    /* The periodic tick is armed 10 ms after power-on (chihiro_arm_diag_cb)
     * and first runs a second later. */
    s->diag_armed = false;
    s->diag_arm_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                            chihiro_arm_diag_cb, s);
    timer_mod(s->diag_arm_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
}

/* The whole mediaboard protocol state: without it a snapshot restored
 * into a fresh boot leaves the LPC at reset under a guest mid-protocol,
 * and the game kernel panics within a second. The globals ride along in
 * the device state (mig_* fields) so they are actually serialized. */

static int chihiro_lpc_pre_save(void *opaque)
{
    ChihiroLPCState *s = opaque;
    s->mig_game_running = chihiro_game_running;
    s->mig_active = chihiro_active;
    g_strlcpy((char *)s->mig_game_filename, chihiro_game_filename,
              sizeof(s->mig_game_filename));
    return 0;
}

static int chihiro_lpc_post_load(void *opaque, int version_id)
{
    ChihiroLPCState *s = opaque;
    if (version_id >= 3) {
        chihiro_game_running = s->mig_game_running;
        chihiro_active = s->mig_active;
        /* A game that was running had its Type-1 bootstrap then: it must
         * not fire again in the middle of the game. */
        chihiro_mbcom_bootstrap_done = chihiro_game_running;
    }
    if (version_id >= 4 && s->mig_game_filename[0]) {
        /* Goes through the single writer, so the cabinet is worked out
         * again from the restored name. */
        s->mig_game_filename[sizeof(s->mig_game_filename) - 1] = '\0';
        chihiro_set_game_executable((const char *)s->mig_game_filename);
    }
    /* The periodic tick is not migrated and its deadline belongs to the
     * replaced session: restart it, or the readers and drive boards stop. */
    chihiro_diag_timer_start(s, 16);
    return 0;
}

static const VMStateDescription vmstate_chihiro_lpc = {
    .name = "chihiro-lpc",
    .version_id = 4,
    .minimum_version_id = 1,
    .pre_save = chihiro_lpc_pre_save,
    .post_load = chihiro_lpc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UNUSED(64),
        VMSTATE_BOOL(host_seen, ChihiroLPCState),
        VMSTATE_UINT32(lpc_reg_addr, ChihiroLPCState),
        VMSTATE_UNUSED(4),
        VMSTATE_BOOL(diag_armed, ChihiroLPCState),
        VMSTATE_UINT32(lpc_401e_reads, ChihiroLPCState),
        VMSTATE_UNUSED(4),
        VMSTATE_UINT16(lpc_scratch_4026, ChihiroLPCState),
        VMSTATE_UINT8(mbcom_e0_status, ChihiroLPCState),
        VMSTATE_BOOL(mbcom_resp_ready, ChihiroLPCState),
        VMSTATE_UINT32(bb_reg_addr, ChihiroLPCState),
        VMSTATE_UINT32(bb_reg_status, ChihiroLPCState),
        VMSTATE_BOOL(bb_dma_active, ChihiroLPCState),
        VMSTATE_UINT32(bb_dma_count, ChihiroLPCState),
        VMSTATE_UNUSED(1),
        VMSTATE_UINT32(asic_cpu_ctrl, ChihiroLPCState),
        VMSTATE_UINT32_ARRAY(dimm_cmd, ChihiroLPCState, 8),
        VMSTATE_UINT32_ARRAY(dimm_resp, ChihiroLPCState, 8),
        VMSTATE_UNUSED(11),
        /* The IRQ10 tick and the one-shot reply timer: a snapshot taken with
         * a mediaboard transaction in flight owes the guest a response —
         * without the timer the reply never comes and the game hangs on its
         * next mediaboard poll. */
        VMSTATE_TIMER_PTR_V(irq10_timer, ChihiroLPCState, 2),
        VMSTATE_TIMER_PTR_V(dimm_resp_timer, ChihiroLPCState, 2),
        VMSTATE_BOOL_V(mig_game_running, ChihiroLPCState, 3),
        VMSTATE_UNUSED_V(3, 1),
        VMSTATE_BOOL_V(mig_active, ChihiroLPCState, 3),
        VMSTATE_BUFFER_V(mig_game_filename, ChihiroLPCState, 4),
        VMSTATE_END_OF_LIST()
    }
};

static void chihiro_lpc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = chihiro_lpc_realize;
    dc->vmsd = &vmstate_chihiro_lpc;
    dc->desc = "Chihiro Mediaboard LPC I/O";
}

static const TypeInfo chihiro_lpc_info = {
    .name          = "chihiro-lpc",
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(ChihiroLPCState),
    .class_init    = chihiro_lpc_class_init,
};

static void chihiro_register_types(void)
{
    type_register_static(&chihiro_lpc_info);
}

type_init(chihiro_register_types)

/* ═══════════════════════════════════════════════════════════════════════
 * Save file persistence
 *
 * Persists QC ic11 (512 B) and the baseboard SRAM's backup half (the two
 * windows the game's backup occupies, 55 KB) per game.
 * File: saves/<MAME name>.sav in xemu's data folder
 * ═══════════════════════════════════════════════════════════════════════ */

bool chihiro_file_replace(const char *path, const ChihiroFilePart *parts,
                          int count)
{
    char *tmp = g_strdup_printf("%s.tmp", path);
    FILE *f = qemu_fopen(tmp, "wb");
    bool ok = f != NULL;

    for (int i = 0; ok && i < count; i++) {
        ok = fwrite(parts[i].data, 1, parts[i].size, f) == parts[i].size;
    }
    if (f && fclose(f) != 0) {
        ok = false;
    }
    if (ok) {
        ok = g_rename(tmp, path) == 0;
    }
    if (!ok) {
        g_remove(tmp);
        CHIHIRO_ERRF("cannot write %s\n", path);
    }
    g_free(tmp);
    return ok;
}

/* Per-game data goes where the rest of xemu keeps its state (see the shader
 * cache): xemu_settings_get_base_path() honours portable mode and is correct
 * on Windows and macOS, unlike a hand-built $HOME path. */
static bool chihiro_data_dir(const char *name, char *out, size_t out_len)
{
    const char *base = xemu_settings_get_base_path();
    if (!base || !base[0]) return false;
    snprintf(out, out_len, "%s%s", base, name);
    g_mkdir_with_parents(out, 0755);
    return true;
}

/* A card issued to a player with none: a new empty file (a blank) in
 * <data>/cards/, named after the game, the player and the day
 * ("wangmid2_<year>-<month>-<day>.bin", then "_2", "_3"), created
 * exclusively. The slot is assigned at the UI's next frame. */
static struct {
    bool        due;
    const char **cfg;    /* the setting that names the slot's card */
    char        path[1200];
} chihiro_card_issued[CHIHIRO_CARD_SLOTS];

bool chihiro_card_issue(ChihiroCardSlot slot, char *out, size_t out_len)
{
    const ChihiroCabinet *c = chihiro_cabinet();
    const char **cfg;
    char dir[1024], who[8] = "";
    char *day;
    GDateTime *now;
    bool ok = false;

    if (!c || !c->cards || !chihiro_data_dir("cards", dir, sizeof(dir)))
        return false;
    cfg = chihiro_card_slot_setting(slot);
    if (slot == CHIHIRO_CARD_SLOT_HW210_P1)
        snprintf(who, sizeof(who), "_p1");
    else if (slot == CHIHIRO_CARD_SLOT_HW210_P2)
        snprintf(who, sizeof(who), "_p2");

    now = g_date_time_new_now_local();
    day = g_date_time_format(now, "%Y-%m-%d");
    g_date_time_unref(now);
    for (int n = 1; n < 1000 && !ok; n++) {
        char name[128], more[16] = "";
        char *path;
        int fd;

        if (n > 1)
            snprintf(more, sizeof(more), "_%d", n);
        snprintf(name, sizeof(name), "%s%s_%s%s.bin", c->cards, who, day, more);
        path = g_build_filename(dir, name, NULL);
        fd = qemu_open_old(path, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0644);
        if (fd >= 0) {
            close(fd);
            snprintf(out, out_len, "%s", path);
            ok = true;
        } else if (errno != EEXIST) {
            fprintf(stderr, "Chihiro: cannot make a card at %s: %s\n", path,
                    strerror(errno));
            g_free(path);
            break;
        }
        g_free(path);
    }
    g_free(day);
    if (!ok)
        return false;

    fprintf(stderr, "Chihiro: new card issued: %s\n", out);
    chihiro_card_issued[slot].cfg = cfg;
    snprintf(chihiro_card_issued[slot].path,
             sizeof(chihiro_card_issued[slot].path), "%s", out);
    chihiro_card_issued[slot].due = true;
    return true;
}

/* The readers run under the big lock, which the UI also holds while it
 * draws the notifications, so theirs are queued at once. */
void chihiro_card_note(const char *msg, bool warning)
{
    if (warning)
        xemu_queue_notification_warning(msg);
    else
        xemu_queue_notification(msg);
}

void chihiro_card_ui_sync(void)
{
    for (int i = 0; i < CHIHIRO_CARD_SLOTS; i++) {
        const char *name;
        char msg[200];

        if (!chihiro_card_issued[i].due)
            continue;
        chihiro_card_issued[i].due = false;
        xemu_settings_set_string(chihiro_card_issued[i].cfg,
                                 chihiro_card_issued[i].path);
        xemu_settings_save();
        name = strrchr(chihiro_card_issued[i].path, G_DIR_SEPARATOR);
        snprintf(msg, sizeof(msg), "New card: %s",
                 name ? name + 1 : chihiro_card_issued[i].path);
        xemu_queue_notification(msg);
    }
}

/* Game name without its .xbe extension, e.g. "vsg". */
static bool chihiro_game_base_name(char *base, size_t base_len)
{
    if (!chihiro_game_dir[0]) return false;

    chihiro_capture_game_filename_from_dir();
    if (!chihiro_game_filename[0]) return false;

    g_strlcpy(base, chihiro_game_filename, base_len);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    return base[0] != 0;
}

/* A game's files take its MAME name, the parent set's (revisions and regions
 * share it), found by the identifier at boot.id 0x30. A game MAME does not
 * have takes that identifier in lower case (the OutRun 2 prototype: gbz). */
static const struct {
    char id[5];
    const char *name;
} chihiro_game_names[] = {
    { "SBFN", "hotd3" },    /* The House of the Dead III */
    { "SBFY", "crtaxihr" }, /* Crazy Taxi High Roller */
    { "SBFZ", "vcop3" },    /* Virtua Cop 3 */
    { "SGBZ", "outr2" },    /* OutRun 2 */
    { "SBHC", "mj2" },      /* Sega Network Taisen Mahjong MJ 2 */
    { "SBHF", "ollie" },    /* Ollie King */
    { "SBHQ", "wangmid" },  /* Wangan Midnight Maximum Tune */
    { "SBHU", "ghostsqu" }, /* Ghost Squad */
    { "SBJK", "gundamos" }, /* Gundam Battle Operating Simulator */
    { "SBJE", "outr2st" },  /* OutRun 2 Special Tours */
    { "SBKD", "wangmid2" }, /* Wangan Midnight Maximum Tune 2 */
    { "SBKK", "mj3" },      /* Sega Network Taisen Mahjong MJ 3 */
    { "SBLF", "scg06nt" },  /* Sega Club Golf 2006 Next Tours */
    { "SBME", "mj3evo" },   /* Sega Network Taisen Mahjong MJ 3 Evolution */
};

static void chihiro_game_name_of(const uint8_t *id, char *out, size_t out_len)
{
    size_t n = 0;

    for (size_t i = 0; i < ARRAY_SIZE(chihiro_game_names); i++) {
        if (memcmp(chihiro_game_names[i].id, id, 4) == 0) {
            g_strlcpy(out, chihiro_game_names[i].name, out_len);
            return;
        }
    }
    while (n < 4 && n + 1 < out_len && g_ascii_isalnum(id[n])) {
        out[n] = g_ascii_tolower(id[n]);
        n++;
    }
    out[n] = 0;
}

/* The name of the game on the disc; false when its boot.id has none. */
static bool chihiro_game_name(char *out, size_t out_len)
{
    uint8_t bid[CHIHIRO_BOOTID_LEN];

    out[0] = 0;
    if (chihiro_read_image_bootid(bid)) {
        chihiro_game_name_of(bid + 0x30, out, out_len);
    }
    return out[0] != 0;
}

/* A save named after the executable, from before, moves to the name of the
 * game that wrote it, the one its backup header names, with its DIMM system
 * area: two games that shared the file (OutRun 2 and OutRun 2 SP) each find
 * their own, whichever runs first. */
static void chihiro_save_move_old(const char *saves_dir, const char *base)
{
    char old_path[1200], new_path[1200], old_dimm[1200], new_dimm[1200];
    char name[16];
    uint8_t owner[4];

    snprintf(old_path, sizeof(old_path), "%s/%s.sav", saves_dir, base);
    if (!chihiro_usb_save_owner(old_path, owner)) {
        return;
    }
    chihiro_game_name_of(owner, name, sizeof(name));
    snprintf(new_path, sizeof(new_path), "%s/%s.sav", saves_dir, name);
    if (!name[0] || strcmp(new_path, old_path) == 0 ||
        g_file_test(new_path, G_FILE_TEST_EXISTS) ||
        g_rename(old_path, new_path) != 0) {
        return;
    }
    fprintf(stderr, "Chihiro: save %s renamed %s\n", old_path, new_path);
    snprintf(old_dimm, sizeof(old_dimm), "%s/%s.dimm", saves_dir, base);
    snprintf(new_dimm, sizeof(new_dimm), "%s/%s.dimm", saves_dir, name);
    if (g_file_test(old_dimm, G_FILE_TEST_EXISTS)) {
        g_rename(old_dimm, new_dimm);
    }
}

/* saves/<name>.sav, the game's name above. The executable's will not do:
 * games share it (OUTRUN2.XBE is OutRun 2 and OutRun 2 SP, mj3.xbe MJ3 and
 * MJ3 Evolution), and a backup another game wrote does not load: OutRun 2 SP
 * then resets its bookkeeping, settings and ranking (acBackupLoadUserData
 * answers 1). A disc without an identifier keeps saves/<executable>.sav. */
static bool chihiro_resolve_save_path(void)
{
    char base[64], name[16];
    char saves_dir[1024];

    if (!chihiro_game_base_name(base, sizeof(base)) ||
        !chihiro_data_dir("saves", saves_dir, sizeof(saves_dir)))
        return false;

    if (chihiro_game_name(name, sizeof(name))) {
        chihiro_save_move_old(saves_dir, base);
        snprintf(chihiro_save_path, sizeof(chihiro_save_path), "%s/%s.sav",
                 saves_dir, name);
        return true;
    }
    snprintf(chihiro_save_path, sizeof(chihiro_save_path),
             "%s/%s.sav", saves_dir, base);
    /* "A\V322.xbe" keeps its backslash: one file on Linux, a folder "A" that
     * must exist on Windows. */
    char *dir = g_path_get_dirname(chihiro_save_path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    return true;
}

/* An older configuration's card1_path and card2_path move to the reader's
 * slots once. */
static void chihiro_migrate_card_paths(void)
{
    static const struct { const char **old; const char **now; } moves[] = {
        { &g_config.chihiro.card_reader.card1_path,
          &g_config.chihiro.card_reader.hw210.slot1 },
        { &g_config.chihiro.card_reader.card2_path,
          &g_config.chihiro.card_reader.hw210.slot2 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(moves); i++) {
        const char *old = *moves[i].old;
        const char *now = *moves[i].now;
        if (!old || !old[0] || (now && now[0]))
            continue;
        fprintf(stderr, "Chihiro: card moved to the HW210 reader: %s\n", old);
        xemu_settings_set_string(moves[i].now, old);
        xemu_settings_set_string(moves[i].old, "");
    }
}

static void chihiro_resolve_card_path(int player, char *out, size_t out_len)
{
    const char *cfg = "";

    /* The Gundam cabinet's one reader sits on Ghost Squad's player 1 wire,
     * but it takes Banpresto cards, not Sega ones, so it has its own slot. */
    if (player == 0 || !chihiro_cabinet_is("gs"))
        cfg = *chihiro_card_slot_setting(chihiro_hw210_card_slot(player));
    if (cfg && cfg[0]) {
        snprintf(out, out_len, "%s", cfg);
        return;
    }
    out[0] = '\0'; /* unassigned: no card in this reader */
}

/* The backup SRAM is battery-backed, so the save follows the game's writes:
 * rewritten at most every five seconds while the game writes it, at once on
 * exit. */
static void chihiro_backup_tick(void)
{
    static int64_t last_flush;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    if (!chihiro_usb_save_dirty() && !chihiro_dimm_sys_dirty)
        return;
    if (now - last_flush < 5000)
        return;
    if (!chihiro_save_path[0] && !chihiro_resolve_save_path()) return;
    chihiro_usb_save_flush(chihiro_save_path);
    chihiro_dimm_sys_flush();
    last_flush = now;
}

/*
 * The operator's CARD REPAIR (gs_gtest.xbe FUN_00013320), done to the card
 * file. A game cut short leaves its card open (B[0x41] set on read, gs.xbe
 * FUN_000660c0, cleared on the final write, FUN_0005beb0); the cabinet keeps
 * ten copies of cards it read in its backup (FUN_000658b0, keyed on block 0),
 * and CARD REPAIR puts the copy back, from the live backup or the saved one.
 * The game's checks only (header 45 2X, serial past 2000: FUN_00017700,
 * FUN_00017790); the copy must carry the card's number and not be retired
 * (B[0x40]); B[0x41] is cleared and the game's checksum rebuilt (folded ones'
 * complement over B[0x40..0x1FF], FUN_00017640). The copies sit 0x84 into the
 * SBJK1290 block (gs.xbe 0x000EF874), 0x200 apart, at 0x8490 in the backup.
 */
bool chihiro_gundam_card_repair(const char *card_path, char *why, size_t why_len)
{
    uint8_t card[CARD_TOTAL_SIZE];
    uint8_t image[0x10000];
    const uint8_t *backup = NULL;
    FILE *f;

    if (!card_path || !card_path[0]) {
        snprintf(why, why_len, "No card.");
        return false;
    }
    f = qemu_fopen(card_path, "rb");
    if (!f || fread(card, 1, sizeof(card), f) != sizeof(card)) {
        if (f) fclose(f);
        snprintf(why, why_len, "Cannot read the card file.");
        return false;
    }
    fclose(f);

    if (((card[0x22] << 8 | card[0x23]) & 0xFFF0) != 0x4520) {
        snprintf(why, why_len, "Not a Gundam card.");
        return false;
    }
    {
        unsigned serial = 0;
        for (int i = 0x24; i < 0x28; i++)
            serial = serial * 100 + (card[i] >> 4) * 10 + (card[i] & 0xF);
        if (serial <= 2000) {
            snprintf(why, why_len, "Not a Gundam card (serial %u).", serial);
            return false;
        }
    }
    if (card[0x40] != 0) {
        snprintf(why, why_len, "Retired card, the game would not repair it either.");
        return false;
    }
    /* The solenoid is on: the game is reading or writing this card. */
    if (chihiro_jvs_card_lock[0]) {
        snprintf(why, why_len, "The game is using the card.");
        return false;
    }

    /* The cabinet's backup: live if this cabinet is the one running, else
     * the file it was last saved to. */
    if (chihiro_game_running && chihiro_cabinet_is("gs"))
        backup = chihiro_usb_backup_live();
    if (!backup) {
        char saves_dir[1024], path[1200];
        if (!chihiro_data_dir("saves", saves_dir, sizeof(saves_dir))) {
            snprintf(why, why_len, "No saves folder.");
            return false;
        }
        /* gundamos.sav, or gs.sav from before saves took the MAME name */
        snprintf(path, sizeof(path), "%s/gundamos.sav", saves_dir);
        if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
            snprintf(path, sizeof(path), "%s/gs.sav", saves_dir);
        }
        if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
            snprintf(why, why_len, "No save yet, the cabinet never read this card.");
            return false;
        }
        memset(image, 0, sizeof(image));
        if (!chihiro_usb_save_read_backup(path, image)) {
            snprintf(why, why_len, "The save file is damaged.");
            return false;
        }
        backup = image;
    }

    /* The game's backup block, by its own tag. */
    uint32_t cache = 0;
    for (uint32_t off = 0x8000; off < 0x10000 - 12; off += 0x200) {
        if (memcmp(backup + off, "SBJK1290", 8) == 0) {
            cache = off + 12 + 0x84;
            break;
        }
    }
    if (!cache || cache + 10 * 0x200 > 0x10000) {
        snprintf(why, why_len, "The cabinet has no card copies yet.");
        return false;
    }

    for (int k = 0; k < 10; k++) {
        const uint8_t *copy = backup + cache + k * 0x200;
        if (memcmp(copy, card, 8) != 0)
            continue;
        memcpy(card, copy, 0x200);
        card[0x41] = 0;
        card[0x42] = card[0x43] = 0;
        {
            uint32_t sum = 0;
            for (int i = 0x40; i < 0x200; i += 2)
                sum += card[i] | (card[i + 1] << 8);
            sum = (sum >> 16) + (sum & 0xFFFF);
            sum = ~((sum >> 16) + sum);
            card[0x42] = sum & 0xFF;
            card[0x43] = (sum >> 8) & 0xFF;
        }
        /* A reader holding this card writes the repair itself. */
        bool written = false;
        for (int p = 0; p < 2; p++) {
            if (card_reader_initialized && hw210.reader[p].card_present &&
                strcmp(hw210.reader[p].card_path, card_path) == 0) {
                memcpy(hw210.reader[p].card_data, card, sizeof(card));
                written = card_reader_flush(&hw210.reader[p]);
            }
        }
        if (!written) {
            ChihiroFilePart part = { card, sizeof(card) };
            if (!chihiro_file_replace(card_path, &part, 1)) {
                snprintf(why, why_len, "Cannot write the card file.");
                return false;
            }
        }
        fprintf(stderr, "Chihiro: Gundam card repaired from the cabinet's copy %d: %s\n",
                k, card_path);
        snprintf(why, why_len, "Card repaired!");
        return true;
    }
    snprintf(why, why_len, "No copy of this card: never read here, or ten "
             "others read since.");
    return false;
}

static void chihiro_exit_notify(Notifier *notifier, void *data)
{
    (void)notifier;
    (void)data;
    chihiro_netboard_exit();
    if (!chihiro_active) return;
    if (!chihiro_save_path[0] && !chihiro_resolve_save_path()) return;
    if (chihiro_usb_save_flush(chihiro_save_path))
        fprintf(stderr, "Chihiro: save flushed to %s\n", chihiro_save_path);
    chihiro_dimm_sys_flush();
}

static Notifier chihiro_exit_notifier = { .notify = chihiro_exit_notify };
/* Flush the arcade backup (game save) immediately. The UI quit path skips
 * the doomed driver atexit chain with _exit(), which also skips the exit
 * notifier — it must flush explicitly before leaving. */
void chihiro_flush_save_now(void)
{
    chihiro_exit_notify(NULL, NULL);
}

static bool chihiro_exit_notifier_registered = false;

void chihiro_save_init(void)
{
    if (chihiro_exit_notifier_registered) return;
    qemu_add_exit_notifier(&chihiro_exit_notifier);
    chihiro_exit_notifier_registered = true;

    chihiro_migrate_card_paths();
    if (chihiro_resolve_save_path()) {
        chihiro_usb_save_load(chihiro_save_path);
        chihiro_dimm_sys_load();
    }
}

/*
 * Chihiro MediaBoard IDE mbcom protocol handler
 *
 * The baseboard communicates with SEGABOOT via IDE sector read/write
 * at specific LBAs within the mbcom partition:
 *   - Response sector: mbcom_base + 0x4800 (read by SEGABOOT)
 *   - Command sector:  mbcom_base + 0x4801 (written by SEGABOOT)
 *
 * The disk is laid out from the DIMM jumpers, the way the kernel's MediaBoard
 * driver lays it out: mbcom is the 0x8000 sectors below the DIMM's end and
 * mbfs, the game's filesystem, is everything below mbcom, so a 512 MB board
 * offers 496 MiB of game and a 1 GB board 1008 MiB. The images are formatted
 * for exactly that: Gundam Battle Operating Simulator is written for the
 * 1008 MiB of its 1 GB board, its gs.xbe sits at 564 MiB, and a disk cut at
 * 496 MiB hands SEGABOOT a game with no executable, and SEGABOOT stays at its
 * system menu. MEASURED on the Gundam image: 51 files past 496 MiB.
 *
 *   mbcom_base = (0x40000 << factor) - 0x8000    (0xF8000 for 512 MB)
 *   Response LBA = mbcom_base + 0x4800, Command LBA = mbcom_base + 0x4801
 */

static uint32_t chihiro_mbcom_base(void)
{
    return (0x40000u << chihiro_dimm_factor()) - 0x8000u;
}
#define CHIHIRO_MBROM0          0x8000000

/* ── The DIMM's system area ───────────────────────────────────────────────
 * The 0x8000 sectors above the game's filesystem (the kernel's "mbsys:"),
 * with the mailbox in the middle; games keep big records there (OutRun 2
 * SP's ranking). Battery-backed on the cabinet: the written sectors follow
 * the save as <game>.dimm. Not in snapshots: a load does not roll it back. */
#define CHIHIRO_DIMM_SYS_SECTORS 0x8000u
#define CHIHIRO_DIMM_SYS_MAGIC   0x53444843u   /* "CHDS" */
#define CHIHIRO_DIMM_SYS_VERSION 1

static uint8_t *chihiro_dimm_sys;                       /* 16 MB, on first use */
static uint32_t chihiro_dimm_sys_first, chihiro_dimm_sys_end;  /* written: [first, end) */

static uint8_t *chihiro_dimm_sys_buffer(void)
{
    if (!chihiro_dimm_sys)
        chihiro_dimm_sys = g_malloc0((size_t)CHIHIRO_DIMM_SYS_SECTORS * 512);
    return chihiro_dimm_sys;
}

static void chihiro_dimm_sys_read(uint32_t sector, void *buf, int n)
{
    memcpy(buf, chihiro_dimm_sys_buffer() + (size_t)sector * 512, (size_t)n * 512);
}

static void chihiro_dimm_sys_write(uint32_t sector, const void *buf, int n)
{
    memcpy(chihiro_dimm_sys_buffer() + (size_t)sector * 512, buf, (size_t)n * 512);
    if (chihiro_dimm_sys_end == 0) {
        chihiro_dimm_sys_first = sector;
        chihiro_dimm_sys_end = sector + n;
    } else {
        chihiro_dimm_sys_first = MIN(chihiro_dimm_sys_first, sector);
        chihiro_dimm_sys_end = MAX(chihiro_dimm_sys_end, sector + n);
    }
    chihiro_dimm_sys_dirty = true;
}

/* <game>.dimm next to <game>.sav */
static bool chihiro_dimm_sys_path(char *out, size_t out_len)
{
    size_t len = strlen(chihiro_save_path);
    if (len < 4 || strcmp(chihiro_save_path + len - 4, ".sav") != 0) return false;
    snprintf(out, out_len, "%.*s.dimm", (int)(len - 4), chihiro_save_path);
    return true;
}

/* File: magic, version, first sector, sector count, then the sectors. */
static void chihiro_dimm_sys_load(void)
{
    char path[1024];
    uint32_t hdr[4];

    if (!chihiro_dimm_sys_path(path, sizeof(path))) return;
    /* A fresh start for this game, unless it has already written. */
    if (!chihiro_dimm_sys_dirty) {
        chihiro_dimm_sys_first = chihiro_dimm_sys_end = 0;
        if (chihiro_dimm_sys)
            memset(chihiro_dimm_sys, 0, (size_t)CHIHIRO_DIMM_SYS_SECTORS * 512);
    }
    FILE *f = qemu_fopen(path, "rb");
    if (!f) return;
    if (fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == CHIHIRO_DIMM_SYS_MAGIC &&
        hdr[1] == CHIHIRO_DIMM_SYS_VERSION && hdr[2] < CHIHIRO_DIMM_SYS_SECTORS &&
        hdr[3] <= CHIHIRO_DIMM_SYS_SECTORS - hdr[2]) {
        uint8_t *dst = chihiro_dimm_sys_buffer() + (size_t)hdr[2] * 512;
        if (fread(dst, 512, hdr[3], f) == hdr[3]) {
            chihiro_dimm_sys_first = hdr[2];
            chihiro_dimm_sys_end = hdr[2] + hdr[3];
            fprintf(stderr, "Chihiro: DIMM system area loaded from %s "
                    "(%u sectors)\n", path, hdr[3]);
        }
    }
    fclose(f);
    chihiro_dimm_sys_dirty = false;
}

static bool chihiro_dimm_sys_flush(void)
{
    char path[1024];

    /* Unchanged since loaded or written: the file already holds it. */
    if (!chihiro_dimm_sys_dirty) return true;
    if (!chihiro_dimm_sys || chihiro_dimm_sys_end == 0) return false;
    if (!chihiro_dimm_sys_path(path, sizeof(path))) return false;
    uint32_t hdr[4] = { CHIHIRO_DIMM_SYS_MAGIC, CHIHIRO_DIMM_SYS_VERSION,
                        chihiro_dimm_sys_first,
                        chihiro_dimm_sys_end - chihiro_dimm_sys_first };
    ChihiroFilePart parts[] = {
        { hdr, sizeof(hdr) },
        { chihiro_dimm_sys + (size_t)chihiro_dimm_sys_first * 512, (size_t)hdr[3] * 512 },
    };
    if (!chihiro_file_replace(path, parts, ARRAY_SIZE(parts))) return false;
    chihiro_dimm_sys_dirty = false;
    return true;
}

/* MemoryRegion-backed IDE interface. DMA on unit 1 (FATX, flash ROM, mbcom)
 * is served by chihiro_ide_serve, hooked in hw/ide/core.c; only PIO reaches
 * the block device. */
static uint64_t chihiro_fs_size(void)
{
    return (uint64_t)chihiro_mbcom_base() * 512;
}

/* boot.id, found as the kernel finds it (FatxMountVolume): in mbfs, a FAT of
 * one entry per cluster plus one (16-bit below 0xFFF0 entries, else 32-bit),
 * rounded up to a page, then the root directory; 64-byte entries (name
 * length, attributes, name, first cluster at 0x2C, size at 0x30). A game
 * launched from a directory carries it as a file. */
static bool chihiro_read_image_bootid(uint8_t *bid)
{
    const char *path = xemu_chihiro_image();
    if (!path || !path[0]) {
        return false;
    }
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        char *bootid_path = g_strdup_printf("%s/boot.id", path);
        FILE *f = qemu_fopen(bootid_path, "rb");
        g_free(bootid_path);
        if (!f) {
            return false;
        }
        bool ok = fread(bid, 1, CHIHIRO_BOOTID_LEN, f) == CHIHIRO_BOOTID_LEN;
        fclose(f);
        return ok && memcmp(bid, "BTID", 4) == 0;
    }

    FILE *f = qemu_fopen(path, "rb");
    if (!f) {
        return false;
    }
    bool ok = false;
    uint8_t hdr[16];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "FATX", 4) != 0) {
        goto out;
    }
    uint32_t spc = ldl_le_p(hdr + 8);
    uint32_t root = ldl_le_p(hdr + 12);
    if (spc == 0 || spc > 128 || (spc & (spc - 1)) || root == 0) {
        goto out;
    }
    uint32_t cluster = spc * 512;
    uint64_t entries = chihiro_fs_size() / cluster + 1;
    uint64_t data = 4096 + ROUND_UP(entries * (entries < 0xFFF0 ? 2 : 4), 4096);
    uint8_t *dir = g_malloc(cluster);
    if (fseeko(f, data + (uint64_t)(root - 1) * cluster, SEEK_SET) == 0 &&
        fread(dir, 1, cluster, f) == cluster) {
        for (uint32_t i = 0; i + 64 <= cluster; i += 64) {
            const uint8_t *e = dir + i;
            if (e[0] == 0 || e[0] == 0xFF) {
                break;
            }
            if (e[0] != 7 ||
                g_ascii_strncasecmp((const char *)e + 2, "boot.id", 7) != 0) {
                continue;
            }
            uint32_t first = ldl_le_p(e + 0x2C);
            ok = first != 0 &&
                 fseeko(f, data + (uint64_t)(first - 1) * cluster, SEEK_SET) == 0 &&
                 fread(bid, 1, CHIHIRO_BOOTID_LEN, f) == CHIHIRO_BOOTID_LEN &&
                 memcmp(bid, "BTID", 4) == 0;
            break;
        }
    }
    g_free(dir);
out:
    fclose(f);
    return ok;
}

/* The cabinet's region, the byte at EEPROM 0x1F00: 1 Japan, 2 USA, 3 Export.
 * SEGABOOT tests bit (1 << byte) of the mask at boot.id + 0x38 and stops on
 * Error 05 when it is clear (fpr21042, verifier at 0x0002EAE0). The setting
 * is the preferred region, Export unless changed ('auto' is Export's older
 * name): a game whose mask lacks it gets the first of Export, USA and Japan
 * the mask has, and a notification says so. */
uint8_t chihiro_region_byte(void)
{
    static const char *const names[] = { "", "Japan", "USA", "Export" };
    static uint8_t region;
    static int for_set = -1;
    static char *for_path;
    int set = g_config.chihiro.settings.region;
    const char *path = xemu_chihiro_image();

    if (set == CONFIG_CHIHIRO_SETTINGS_REGION_AUTO) {
        set = CONFIG_CHIHIRO_SETTINGS_REGION_EX;
    }
    if (set == for_set && for_path && g_strcmp0(for_path, path) == 0) {
        return region;
    }
    for_set = set;
    g_free(for_path);
    for_path = g_strdup(path ? path : "");

    uint8_t bid[CHIHIRO_BOOTID_LEN];
    uint8_t mask = chihiro_read_image_bootid(bid) ? bid[0x38] : 0;
    region = set;           /* the enum index is the byte */
    if (mask & (1u << set)) {
        fprintf(stderr, "Chihiro: region %s, the preferred one, %.4s "
                "accepts it\n", names[region], (const char *)bid + 0x30);
    } else if (mask & 0x0E) {
        for (int r = CONFIG_CHIHIRO_SETTINGS_REGION_EX;
             r >= CONFIG_CHIHIRO_SETTINGS_REGION_JP; r--) {
            if (mask & (1u << r)) {
                region = r;
                break;
            }
        }
        fprintf(stderr, "Chihiro: region %s, %.4s does not accept %s\n",
                names[region], (const char *)bid + 0x30, names[set]);
        char msg[96];
        snprintf(msg, sizeof(msg), "This game has no %s region: %s used",
                 names[set], names[region]);
        xemu_queue_notification(msg);
    } else {
        fprintf(stderr, "Chihiro: region %s, the preferred one, no boot.id "
                "list to check it against\n", names[region]);
    }
    return region;
}

static MemoryRegion chihiro_interface_container;
static MemoryRegion chihiro_interface_fs;
static AddressSpace chihiro_interface_as;
static bool chihiro_interface_ready = false;

/* DIMM migration: instead of ~540 MB of raw RAM, store the netboot image
 * identity (size + CRC32) plus the pages that differ from it; load
 * re-reads the image and applies the delta. */

#define DIMM_MIG_MAGIC   0x4344494d /* CDIM */
#define DIMM_MIG_VERSION 1
#define DIMM_PAGE_SIZE   4096

typedef struct ChihiroDimmMig {
    uint32_t blob_size;
    uint8_t *blob;
} ChihiroDimmMig;

static ChihiroDimmMig chihiro_dimm_mig;
static char chihiro_dimm_error[256];

const char *chihiro_dimm_last_error(void)
{
    return chihiro_dimm_error[0] ? chihiro_dimm_error : NULL;
}

void chihiro_dimm_clear_error(void)
{
    chihiro_dimm_error[0] = 0;
}

static const char *chihiro_image_basename(void)
{
    const char *path = xemu_chihiro_image();
    const char *sep = path ? strrchr(path, '/') : NULL;
    const char *bsep = path ? strrchr(path, '\\') : NULL;
    if (bsep > sep) sep = bsep;
    return sep ? sep + 1 : (path ? path : "(none)");
}

static uint8_t *chihiro_dimm_read_image(uint64_t buf_size,
                                        uint64_t *file_size, uint32_t *crc)
{
    const char *path = xemu_chihiro_image();
    FILE *f = (path && path[0]) ? qemu_fopen(path, "rb") : NULL;
    if (!f) {
        return NULL;
    }
    uint8_t *buf = g_malloc0(buf_size);
    size_t n = fread(buf, 1, buf_size, f);
    fclose(f);
    *file_size = n;
    *crc = crc32(0, buf, n);
    return buf;
}

bool chihiro_dimm_image_identity(uint64_t *size, uint32_t *crc)
{
    uint32_t fs_size;
    uint8_t *fs = chihiro_fatx_get_buffer(&fs_size);
    if (!fs) {
        return false;
    }
    uint8_t *ref = chihiro_dimm_read_image(fs_size, size, crc);
    g_free(ref);
    return ref != NULL;
}

static int chihiro_dimm_pre_save(void *opaque)
{
    ChihiroDimmMig *m = opaque;
    uint32_t fs_size;
    uint8_t *fs = chihiro_fatx_get_buffer(&fs_size);
    uint64_t file_size;
    uint32_t crc;
    uint8_t *ref = fs ? chihiro_dimm_read_image(fs_size, &file_size, &crc)
                      : NULL;
    if (!ref) {
        error_report("chihiro: cannot read the game image to delta against");
        snprintf(chihiro_dimm_error, sizeof(chihiro_dimm_error),
                 "the game image '%s' cannot be read", chihiro_image_basename());
        return -EINVAL;
    }

    uint32_t hdr[7] = { DIMM_MIG_MAGIC, DIMM_MIG_VERSION,
                        (uint32_t)file_size, (uint32_t)(file_size >> 32),
                        crc, DIMM_PAGE_SIZE, 0 /* npages */ };
    GByteArray *blob = g_byte_array_new();
    g_byte_array_append(blob, (uint8_t *)hdr, sizeof(hdr));

    uint32_t npages = 0;
    for (uint32_t pg = 0; pg < fs_size / DIMM_PAGE_SIZE; pg++) {
        const uint8_t *cur = fs + (size_t)pg * DIMM_PAGE_SIZE;
        if (!memcmp(cur, ref + (size_t)pg * DIMM_PAGE_SIZE, DIMM_PAGE_SIZE)) {
            continue;
        }
        g_byte_array_append(blob, (uint8_t *)&pg, 4);
        g_byte_array_append(blob, cur, DIMM_PAGE_SIZE);
        npages++;
    }
    memcpy(blob->data + 24, &npages, 4);
    g_free(ref);
    chihiro_dimm_error[0] = 0;

    m->blob_size = blob->len;
    g_free(m->blob);
    m->blob = g_byte_array_free(blob, FALSE);

    printf("Chihiro DIMM migration: %u changed pages, %u byte blob\n",
           npages, m->blob_size);
    return 0;
}

static int chihiro_dimm_post_save(void *opaque)
{
    ChihiroDimmMig *m = opaque;
    g_free(m->blob);
    m->blob = NULL;
    m->blob_size = 0;
    return 0;
}

static int chihiro_dimm_post_load(void *opaque, int version_id)
{
    ChihiroDimmMig *m = opaque;
    int ret = -EINVAL;
    uint32_t fs_size;
    uint8_t *fs = chihiro_fatx_get_buffer(&fs_size);
    const uint8_t *p = m->blob;
    uint32_t hdr[7];
    uint64_t cur_size;
    uint32_t cur_crc;
    uint8_t *ref = NULL;

    snprintf(chihiro_dimm_error, sizeof(chihiro_dimm_error),
             "the snapshot's game data is damaged");
    if (!fs || !m->blob || m->blob_size < sizeof(hdr)) {
        goto out;
    }
    memcpy(hdr, p, sizeof(hdr));
    p += sizeof(hdr);
    if (hdr[0] != DIMM_MIG_MAGIC || hdr[1] != DIMM_MIG_VERSION ||
        hdr[5] != DIMM_PAGE_SIZE) {
        goto out;
    }

    ref = chihiro_dimm_read_image(fs_size, &cur_size, &cur_crc);
    if (!ref) {
        snprintf(chihiro_dimm_error, sizeof(chihiro_dimm_error),
                 "the game image '%s' cannot be read", chihiro_image_basename());
        goto out;
    }
    if (cur_size != (hdr[2] | (uint64_t)hdr[3] << 32) || cur_crc != hdr[4]) {
        error_report("chihiro: mounted game image does not match this "
                     "snapshot");
        snprintf(chihiro_dimm_error, sizeof(chihiro_dimm_error),
                 "it was taken with another game than '%s'. Restart xemu "
                 "with that game to load it", chihiro_image_basename());
        goto out;
    }
    memcpy(fs, ref, fs_size);

    for (uint32_t i = 0; i < hdr[6]; i++) {
        uint32_t pg;
        if ((uint64_t)(p - m->blob) + 4 + DIMM_PAGE_SIZE > m->blob_size) {
            goto out;
        }
        memcpy(&pg, p, 4);
        p += 4;
        if ((uint64_t)pg * DIMM_PAGE_SIZE + DIMM_PAGE_SIZE > fs_size) {
            goto out;
        }
        memcpy(fs + (size_t)pg * DIMM_PAGE_SIZE, p, DIMM_PAGE_SIZE);
        p += DIMM_PAGE_SIZE;
    }
    ret = 0;
    chihiro_dimm_error[0] = 0;

out:
    g_free(ref);
    g_free(m->blob);
    m->blob = NULL;
    m->blob_size = 0;
    return ret;
}

static const VMStateDescription vmstate_chihiro_dimm = {
    .name = "chihiro-dimm",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = chihiro_dimm_pre_save,
    .post_save = chihiro_dimm_post_save,
    .post_load = chihiro_dimm_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(blob_size, ChihiroDimmMig),
        VMSTATE_VBUFFER_ALLOC_UINT32(blob, ChihiroDimmMig, 1, NULL,
                                     blob_size),
        VMSTATE_END_OF_LIST()
    }
};

void chihiro_ide_interface_init(void)
{
    memory_region_init(&chihiro_interface_container, NULL,
                       "chihiro.interface", chihiro_fs_size());

    /* Serialized by chihiro-dimm as a delta, not by the RAM stream. */
    memory_region_init_ram_nomigrate(&chihiro_interface_fs, NULL,
                                     "chihiro.interface.filesystem",
                                     chihiro_fs_size(), &error_fatal);
    vmstate_register(NULL, 0, &vmstate_chihiro_dimm, &chihiro_dimm_mig);
    vmstate_register(NULL, 0, &vmstate_chihiro_hw210, &hw210);

    memory_region_add_subregion(&chihiro_interface_container,
                                0, &chihiro_interface_fs);

    address_space_init(&chihiro_interface_as, &chihiro_interface_container,
                       "chihiro.interface");

    BlockDriverState *bs = bdrv_new();
    bdrv_memory_open(bs, &chihiro_interface_as, chihiro_fs_size());
    bdrv_set_monitor_owned(bs);

    BlockBackend *blk = blk_new(qemu_get_aio_context(),
                                BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                                BLK_PERM_ALL);
    blk_insert_bs(blk, bs, &error_fatal);
    monitor_add_blk(blk, "chihiro-interface", &error_fatal);

    DriveInfo *dinfo = g_malloc0(sizeof(*dinfo));
    dinfo->type = IF_IDE;
    dinfo->bus = 0;
    dinfo->unit = 1;
    dinfo->media_cd = false;
    blk_set_legacy_dinfo(blk, dinfo);

    chihiro_interface_ready = true;
    printf("Chihiro: IDE interface initialized (%u MB fs, ROM via hook)\n",
           (uint32_t)(chihiro_fs_size() / (1024 * 1024)));
    fflush(stdout);
}

/* memmem is missing from some C libraries (mingw). */
static const uint8_t *chihiro_mem_find(const uint8_t *hay, size_t hay_len,
                                       const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; n && i + n <= hay_len; i++) {
        if (hay[i] == (uint8_t)needle[0] && memcmp(hay + i, needle, n) == 0)
            return hay + i;
    }
    return NULL;
}

static void chihiro_segaboot_identify(void)
{
    if (chihiro_flash_rom_size < 0x200000)
        return;

    const uint8_t *half = chihiro_flash_rom + 0x100000;
    const char *tag = "SegaBoot Ver.";
    /* The version's six characters follow the tag inside the dump. */
    const uint8_t *p = chihiro_mem_find(half, 0x100000 - 6, tag);
    if (!p) {
        printf("Chihiro: no SEGABOOT in this flash dump\n");
        return;
    }

    const char *version = (const char *)p + strlen(tag);
    printf("Chihiro: SEGABOOT Ver.%.6s%s\n", version,
           strncmp(version, SEGABOOT_VERSION, strlen(SEGABOOT_VERSION))
               ? " — boot state reporting unavailable for this build" : "");
}

void chihiro_ide_load_rom(void)
{
    if (!chihiro_flash_rom) return;
    printf("Chihiro: flash ROM (%u bytes) ready for IDE hook\n",
           chihiro_flash_rom_size);
    chihiro_segaboot_identify();
}

uint8_t *chihiro_fatx_get_buffer(uint32_t *out_size)
{
    if (!chihiro_interface_ready) {
        fprintf(stderr, "Chihiro: ERROR — fatx_get_buffer called before interface_init\n");
        return NULL;
    }
    *out_size = (uint32_t)chihiro_fs_size();
    return (uint8_t *)memory_region_get_ram_ptr(&chihiro_interface_fs);
}

/* Copies len bytes into the guest's scatter-gather list, from its start. */
static void sg_write(QEMUSGList *sg, const void *src, int len)
{
    int sg_idx = 0, done = 0;
    while (done < len && sg_idx < sg->nsg) {
        int chunk = MIN(len - done, (int)sg->sg[sg_idx].len);
        dma_memory_write(&address_space_memory, sg->sg[sg_idx].base,
                         (const uint8_t *)src + done, chunk,
                         MEMTXATTRS_UNSPECIFIED);
        done += chunk;
        sg_idx++;
    }
}

/* The other way: len bytes out of the guest's list, from its start. */
static void sg_read(QEMUSGList *sg, void *dst, int len)
{
    int sg_idx = 0, done = 0;
    while (done < len && sg_idx < sg->nsg) {
        int chunk = MIN(len - done, (int)sg->sg[sg_idx].len);
        dma_memory_read(&address_space_memory, sg->sg[sg_idx].base,
                        (uint8_t *)dst + done, chunk, MEMTXATTRS_UNSPECIFIED);
        done += chunk;
        sg_idx++;
    }
}

/* Whether the n sectors from lba lie within the count sectors from first,
 * without the sum wrapping round 2^32. */
static bool chihiro_lba_in(uint32_t lba, int n, uint32_t first, uint32_t count)
{
    return lba >= first && (uint32_t)n <= count &&
           lba - first <= count - (uint32_t)n;
}

bool chihiro_ide_serve(int dma_cmd, uint32_t lba, int n,
                       QEMUSGList *sg, bool *irq)
{
    const uint32_t mbcom = chihiro_mbcom_base();
    const uint32_t mbcom_resp = mbcom + 0x4800, mbcom_cmd = mbcom + 0x4801;

    /* A command moves 65536 sectors at most; a count restored from a snapshot
     * can say anything, and n * 512 must not wrap. */
    if (n <= 0 || n > 65536) {
        return false;
    }
    *irq = true;

    if (dma_cmd == 0) { /* IDE_DMA_READ */
        /* FATX: synchronous from MemoryRegion RAM (timing-critical) */
        if (lba < mbcom && chihiro_interface_ready) {
            uint64_t offset = (uint64_t)lba * 512;
            uint64_t fs_size = chihiro_fs_size();
            if (offset < fs_size) {
                uint8_t *src = (uint8_t *)memory_region_get_ram_ptr(
                    &chihiro_interface_fs) + offset;
                if ((uint64_t)n * 512 <= fs_size - offset) {
                    sg_write(sg, src, n * 512);
                } else {
                    /* Across the end of the filesystem: zeros past it. */
                    uint8_t *buf = g_malloc0(n * 512);
                    memcpy(buf, src, fs_size - offset);
                    sg_write(sg, buf, n * 512);
                    g_free(buf);
                }
                return true;
            }
        }
        /* mbcom response/command */
        if (lba == mbcom_resp || lba == mbcom_cmd) {
            uint8_t buf[512] = {0};
            const uint8_t *src = (lba == mbcom_resp)
                ? chihiro_mbcom_response : chihiro_mbcom_command;
            memcpy(buf, src, 32);
            sg_write(sg, buf, 512);
            return true;
        }
        /* The network board's window by its IDE door: the mailbox in its first two
         * sectors, the link slots further in, read whole. */
        if (chihiro_netboard_present() &&
            chihiro_lba_in(lba, n, NETDIMM_WINDOW_LBA,
                           NETDIMM_WINDOW_SECTORS)) {
            int len = n * 512;
            uint8_t *buf = g_malloc(len);
            chihiro_netboard_host_window_read((lba - NETDIMM_WINDOW_LBA) * 512, buf, len);
            sg_write(sg, buf, len);
            g_free(buf);
            return true;
        }
        if (lba == NETDIMM_RESP_LBA || lba == NETDIMM_CMD_LBA) {
            int len = n * 512;
            uint8_t *buf = g_malloc0(len);
            if (lba == NETDIMM_RESP_LBA) {
                memcpy(buf, chihiro_netdimm_resp, sizeof(chihiro_netdimm_resp));
            }
            sg_write(sg, buf, len);
            g_free(buf);
            return true;
        }
        /* the DIMM's system area */
        if (chihiro_lba_in(lba, n, mbcom, CHIHIRO_DIMM_SYS_SECTORS)) {
            int len = n * 512;
            uint8_t *buf = g_malloc(len);
            chihiro_dimm_sys_read(lba - mbcom, buf, n);
            sg_write(sg, buf, len);
            g_free(buf);
            return true;
        }
        /* flash ROM */
        if (lba >= CHIHIRO_MBROM0 && chihiro_flash_rom) {
            uint64_t rom_off = (uint64_t)(lba - CHIHIRO_MBROM0) * 512;
            if (rom_off + (uint64_t)n * 512 <= chihiro_flash_rom_size) {
                sg_write(sg, chihiro_flash_rom + rom_off, n * 512);
                return true;
            }
        }
    }

    if (dma_cmd == 1) { /* IDE_DMA_WRITE */
        if (chihiro_netboard_present() &&
            chihiro_lba_in(lba, n, NETDIMM_WINDOW_LBA,
                           NETDIMM_WINDOW_SECTORS)) {
            /* Straight into the board's window; the doorbell is the read of
             * 0x90000000 that follows a command. */
            int len = n * 512;
            uint8_t *buf = g_malloc(len);
            sg_read(sg, buf, len);
            if (lba == NETDIMM_CMD_LBA)
                chihiro_link_watch_command((const uint32_t *)buf);
            chihiro_netboard_host_window_write((lba - NETDIMM_WINDOW_LBA) * 512,
                                               buf, len);
            g_free(buf);
            return true;
        }
        if (lba == NETDIMM_RESP_LBA || lba == NETDIMM_CMD_LBA) {
            uint8_t buf[512];
            sg_read(sg, buf, sizeof(buf));
            if (lba == NETDIMM_RESP_LBA) {
                /* the game clears the answer block to release it */
                memset(chihiro_netdimm_resp, 0, sizeof(chihiro_netdimm_resp));
                chihiro_netdimm_resp_idx = 0;
            } else {
                memcpy(chihiro_netdimm_cmd, buf, sizeof(chihiro_netdimm_cmd));
                chihiro_netdimm_cmd_idx = 0;
                /* a zeroed block is the game clearing the slot, not a command */
                if (chihiro_netdimm_cmd[0] & 0xFFFF0000)
                    chihiro_netdimm_answer();
            }
            return true;
        }
        if (lba == mbcom_resp || lba == mbcom_cmd) {
            uint8_t buf[512];
            dma_memory_read(&address_space_memory,
                            sg->sg[0].base, buf, 512,
                            MEMTXATTRS_UNSPECIFIED);
            if (lba == mbcom_resp) {
                if (!chihiro_game_running)
                    memcpy(chihiro_mbcom_response, buf, 32);
            } else {
                memcpy(chihiro_mbcom_command, buf, 32);
                if (chihiro_is_type3() &&
                    (chihiro_mbcom_command[0] || chihiro_mbcom_command[1])) {
                    /* Only a Type-1's board answers this mailbox; on a
                     * Type-3 the V850 talks through the SADDR window. */
                    static bool said;
                    if (!said) {
                        said = true;
                        CHIHIRO_ERRF("a Type-1 media board command on a "
                                     "Type-3: left unanswered\n");
                    }
                } else if (chihiro_game_running &&
                    (chihiro_mbcom_command[0] || chihiro_mbcom_command[1])) {
                    chihiro_mbcom_process();
                    memset(chihiro_mbcom_command, 0, 32);
                    if (chihiro_lpc_global) {
                        chihiro_lpc_global->mbcom_e0_status |= 0x01;
                        qemu_irq_lower(chihiro_lpc_global->irq10);
                        qemu_irq_raise(chihiro_lpc_global->irq10);
                    }
                }
            }
            return true;
        }
    }

    if (dma_cmd == 1 &&
        chihiro_lba_in(lba, n, mbcom, CHIHIRO_DIMM_SYS_SECTORS)) {
        int len = n * 512;
        uint8_t *buf = g_malloc(len);
        sg_read(sg, buf, len);
        chihiro_dimm_sys_write(lba - mbcom, buf, n);
        g_free(buf);
        return true;
    }

    /* Any unhandled LBA: return zeros (read) or discard (write).
     * Never fall through to async block device — breaks JVS timing. */
    if (dma_cmd == 0) {
        int total = n * 512;
        int sg_idx = 0, done = 0;
        while (done < total && sg_idx < sg->nsg) {
            int chunk = MIN(total - done, (int)sg->sg[sg_idx].len);
            dma_memory_set(&address_space_memory, sg->sg[sg_idx].base,
                           0, chunk, MEMTXATTRS_UNSPECIFIED);
            done += chunk;
            sg_idx++;
        }
    }
    return true;
}

static void chihiro_mbcom_init(void)
{
    memset(chihiro_mbcom_response, 0, sizeof(chihiro_mbcom_response));
    memset(chihiro_mbcom_command, 0, sizeof(chihiro_mbcom_command));
}

/* The Type-1 board's answer to a command in the IDE mailbox (a Type-3's V850
 * answers for itself). */
static void chihiro_mbcom_process(void)
{
    const uint8_t *w = chihiro_mbcom_command;
    uint8_t *r = chihiro_mbcom_response;

    if (w[0] == 0 && w[1] == 0) return;  /* no command */

    uint16_t cmd_code = w[2] | (w[3] << 8);

    /* Cxbx-style response: echo sequence + command|0x8000 success flag */
    r[0] = w[0];
    r[1] = w[1];
    r[2] = w[2];                             /* the command's low byte */
    r[3] = (w[3] & 0x7F) | 0x80;             /* its high byte, bit 15 set */
    /* zero out rest of 32-byte response area */
    memset(r + 4, 0, 28);

    {
        static int mbcom_cmd_log = 0;
        if (lpc_log_verbose && mbcom_cmd_log < 200) {
            mbcom_cmd_log++;
            fprintf(stderr, "[%07lld] MBCOM cmd=0x%04X seq=%02X%02X data: %02X %02X %02X %02X %02X %02X\n",
                    TS_MS, cmd_code, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
    }

    switch (cmd_code) {
    case MB_CMD_INIT: { /* DIMM size in bytes (from JP1/JP2 jumpers) */
        uint32_t sz = mediaboard.dimm_size;
        memcpy(r + 4, &sz, 4);
        break;
    }
    case MB_CMD_STATUS: /* Boot phase + completion percentage */
        r[4] = mediaboard.status; r[5] = 0; r[6] = 0; r[7] = 0;
        r[8] = mediaboard.progress; r[9] = 0; r[10] = 0; r[11] = 0;
        break;
    case MB_CMD_GET_VERSION: { /* Media board firmware version */
        uint16_t v = mediaboard.fw_version;
        memcpy(r + 4, &v, 2); r[6] = 0; r[7] = 0;
        break;
    }
    case MB_CMD_SYSTEM_TYPE: /* board_type | (fw_ver << 8) */
        r[4] = mediaboard.board_type;
        r[5] = mediaboard.fw_version & 0xFF;
        r[6] = (mediaboard.fw_version >> 8) & 0xFF;
        r[7] = 0;
        break;
    case MB_CMD_GET_SERIAL: /* From flash ROM MBDT+0x10 */
        memcpy(r + 4, mediaboard.serial, 16);
        break;
    case MB_CMD_GET_NET_PROPERTY: /* answered with zeros */
        r[4] = 0; r[5] = 0; r[6] = 0; r[7] = 0;
        break;
    case 0x0204: /* Cxbx: returns 0 */
        r[4] = 0; r[5] = 0; r[6] = 0; r[7] = 0;
        break;
    case MB_CMD_HARDWARE_TEST: /* Cxbx writes "TEST OK" to the result pointer */
        r[4] = w[4]; r[5] = w[5]; r[6] = w[6]; r[7] = w[7];
        /* Write "TEST OK" to the address specified in the command */
        {
            uint32_t result_ptr = w[8] | (w[9]<<8) | (w[10]<<16) | (w[11]<<24);
            if (result_ptr >= 0x80000000) {
                uint32_t result_pa = result_ptr - 0x80000000;
                cpu_physical_memory_write(result_pa, "TEST OK\0", 8);
            }
        }
        break;
    case 0x0415: /* Network IP address */
        memcpy(r + 4, &mediaboard.net_ip, 4);
        break;
    case 0x0601: /* Cxbx: returns 0 */
        r[4] = 0; r[5] = 0; r[6] = 0; r[7] = 0;
        break;
    case 0x0602: /* Cxbx: returns 0xffff (triggers 0x0605) */
        r[4] = 0xFF; r[5] = 0xFF; r[6] = 0; r[7] = 0;
        break;
    case 0x0605: /* Cxbx: returns 0 */
    case 0x0606: /* Cxbx: returns 0 */
        r[4] = 0; r[5] = 0; r[6] = 0; r[7] = 0;
        break;
    case 0x0607: /* Cxbx: returns 0 */
        r[4] = 0; r[5] = 0; r[6] = 0; r[7] = 0;
        r[8] = 0; r[9] = 0; r[10] = 0; r[11] = 0;
        break;
    case 0x0608: /* Network IP address (same as 0x0415) */
        memcpy(r + 4, &mediaboard.net_ip, 4);
        break;
    default:
        CHIHIRO_LOGF(MBCOM, "UNHANDLED cmd=0x%04X\n", cmd_code);
        break;
    }

    chihiro_mbcom_command[0] = 0;
    chihiro_mbcom_command[1] = 0;
    chihiro_mbcom_command[2] = 0;
    chihiro_mbcom_command[3] = 0;
}

