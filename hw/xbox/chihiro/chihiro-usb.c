/*
 * QEMU Chihiro USB Devices
 *
 * Copyright (c) 2016 espes
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
#include "hw/hw.h"
#include "ui/console.h"
#include "hw/usb.h"
#include "hw/usb/desc.h"
#include "qapi/error.h"

#include "qemu/timer.h"
#include "chihiro.h"
#include "chihiro-jvs.h"
#include "chihiro-driveboard-sega838.h"
#include "chihiro-driveboard-v257.h"
#include "chihiro-cardreader-crp1231.h"
#include "chihiro-log.h"
#include "chihiro-an2131.h"
#include "migration/vmstate.h"
#include "ui/xemu-settings.h"
#define TS_MS ((long long)(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)))
extern bool lpc_log_verbose;

typedef struct ChihiroUSBState {
    USBDevice dev;

    /* Device identity (set at realize, safe to use in all callbacks) */
    bool is_qc;  /* true = AN2131QC (PID 0x0002), false = AN2131SC (PID 0x0003) */

    /* I2C EEPROM data (8KB): ic10 for QC, pc20 for SC — loaded at realize */
    uint8_t eeprom[8192];


    /* EZ-USB firmware download state (ANCHOR_LOAD / bRequest 0xA0) */
    uint32_t fw_bytes_written;  /* total bytes received via 0xA0 */
    bool fw_cpu_held;           /* true if CPUCS register set to hold CPU */

    /* ic11 EEPROM — baseboard config, "ACBU0001" + game ID: the 128-byte
     * dump, seen through a 256-byte I2C window (a 24LC024); the buffer and
     * the save file keep 512 */
    uint8_t ic11[512];

    /* The baseboard SRAM behind the 8051, two halves of 64 KB: [0] the
     * firmware's work RAM, [1] the game's backup (see AN2131State.extmem).
     * The save file keeps [1]. */
    uint8_t extmem[2][65536];
    bool extmem_backup_loaded;  /* a snapshot being loaded carried [1] */

    /* The JVS I/O board: the QC's copy, reached through chihiro_jvs_global.
     * The SC's copy is unused. */
    ChihiroJVSState jvs;

    /* AN2131 LLE: 8051 CPU + register layer (runs ic10/pc20 firmware) */
    AN2131State an2131;
    bool use_lle;  /* true after firmware loaded and AN2131 CPU running */
    QEMUTimer *lle_tick_timer;
} ChihiroUSBState;

static ChihiroUSBState *chihiro_qc_instance;

enum chihiro_usb_strings {
    STRING_SERIALNUMBER,
    STRING_MANUFACTURER,
    STRING_PRODUCT,
};

static const USBDescStrings chihiro_usb_stringtable = {
    [STRING_SERIALNUMBER]       = "\x00",
    [STRING_MANUFACTURER]       = "SEGA",
    [STRING_PRODUCT]            = "BASEBD" // different for qc?
};

static const USBDescIface desc_iface_chihiro_an2131qc = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 10,
    .bInterfaceClass               = USB_CLASS_VENDOR_SPEC,
    .bInterfaceSubClass            = 0x00,
    .bInterfaceProtocol            = 0x00,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x03,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x04,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x05,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x03,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x04,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x05,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
    },
};

static const USBDescDevice desc_device_chihiro_an2131qc = {
    .bcdUSB                        = 0x0100,
    .bDeviceClass                  = 0x60,
    .bDeviceSubClass               = 0x00,
    .bDeviceProtocol               = 0x00,
    .bMaxPacketSize0               = 0x40,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .bmAttributes          = 0x80,
            .bMaxPower             = 0x96,
            .nif = 1,
            .ifs = &desc_iface_chihiro_an2131qc,
        },
    },
};

/* The device's shape (.full) for the USB core. The 8051 firmware answers
 * GET_DESCRIPTOR itself, from its own table (CODE:0B7A), so .id and .str are
 * never sent. */
static const USBDesc desc_chihiro_an2131qc = {
    .id = {
        .idVendor          = 0x0CA3,
        .idProduct         = 0x0002,
        .bcdDevice         = 0x0108,
        .iManufacturer     = STRING_MANUFACTURER,
        .iProduct          = STRING_PRODUCT,
        .iSerialNumber     = STRING_SERIALNUMBER,
    },
    .full = &desc_device_chihiro_an2131qc,
    .str  = chihiro_usb_stringtable,
};

static const USBDescIface desc_iface_chihiro_an2131sc = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 6,
    .bInterfaceClass               = USB_CLASS_VENDOR_SPEC,
    .bInterfaceSubClass            = 0x00,
    .bInterfaceProtocol            = 0x00,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x03,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x03,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
    },
};

static const USBDescDevice desc_device_chihiro_an2131sc = {
    .bcdUSB                        = 0x0100,
    .bDeviceClass                  = 0x60,
    .bDeviceSubClass               = 0x01,
    .bDeviceProtocol               = 0x00,
    .bMaxPacketSize0               = 0x40,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .bmAttributes          = 0x80,
            .bMaxPower             = 0x96,
            .nif = 1,
            .ifs = &desc_iface_chihiro_an2131sc,
        },
    },
};

/* As for the QC above */
static const USBDesc desc_chihiro_an2131sc = {
    .id = {
        .idVendor          = 0x0CA3,
        .idProduct         = 0x0003,
        .bcdDevice         = 0x0110,
        .iManufacturer     = STRING_MANUFACTURER,
        .iProduct          = STRING_PRODUCT,
        .iSerialNumber     = STRING_SERIALNUMBER,
    },
    .full = &desc_device_chihiro_an2131sc,
    .str  = chihiro_usb_stringtable,
};


static void lle_tick_cb(void *opaque)
{
    ChihiroUSBState *s = (ChihiroUSBState *)opaque;
    if (s->use_lle && s->an2131.cpu_running) {
        an2131_run(&s->an2131, 6000);
        timer_mod(s->lle_tick_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
    }
}

/* Freeplay: set coin mode 10 in ACBU coin config (ic11 EEPROM).
 * ic11 layout: 2x 64-byte ACBU0001 slots (12-byte header + 52-byte payload).
 * Coin config sits at payload offset 0x10 (ic11 offsets 0x1C/0x5C).
 * Byte 6 = coin mode (0-10), byte 7 = freeplay flag.
 * The game's validator (acLib FUN_000f31c0 in VC3) cross-checks byte 7
 * against a reference table indexed by byte 6. Only entry 10 expects
 * freeplay=1; all other entries expect 0. Setting byte 7 alone fails
 * validation and falls back to defaults (freeplay=OFF).
 * When disabling: restore byte 6 to mode 1 (default) if it was 10.
 * Checksum at header bytes 0x0A-0x0B = 16-bit sum of 52 payload bytes.
 * Called from realize, save_load, and handle_reset. */
static void chihiro_apply_freeplay(ChihiroUSBState *s)
{
    if (s->ic11[0] != 'A' || s->ic11[1] != 'C') {
        return;
    }
    static const uint8_t freeplay_defaults[20] = {
        0x63, 0x09, 0x04, 0x00, 0x01, 0x01, 0x0A, 0x01,
        0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01,
    };
    for (int slot = 0; slot < 2; slot++) {
        int base = slot * 0x40;
        int cfg = base + 0x1C;
        if (chihiro_freeplay_setting) {
            if (s->ic11[cfg] <= 8 || s->ic11[cfg] >= 100) {
                memcpy(&s->ic11[cfg], freeplay_defaults, 20);
            } else {
                s->ic11[base + 0x22] = 0x0A;
                s->ic11[base + 0x23] = 0x01;
            }
        } else if (s->ic11[base + 0x22] == 0x0A) {
            s->ic11[base + 0x22] = 0x01;
            s->ic11[base + 0x23] = 0x00;
        }
        uint16_t sum = 0;
        for (int i = 0; i < 52; i++) {
            sum += s->ic11[base + 0x0C + i];
        }
        s->ic11[base + 0x0A] = sum & 0xFF;
        s->ic11[base + 0x0B] = (sum >> 8) & 0xFF;
    }
}

/* ── The cabinet link's numbers in the game's backup ─────────────────────
 * How many cabinets are linked and which one this is live in the game's
 * backup (set in its test menu). For these titles, Settings > Network >
 * Cabinet Link writes them as the game does, checksums included.
 *
 * acLib backup records are tag(8), a 2-byte field, byte-sum(2), data:
 *   Ollie King    "SBHF", twice in ic11: data +5 cabinets, +6 cabinet (0-based)
 *   OutRun 2      "OUTRUN2" in ic11: +6 TOTAL MACHINE, +7 LINK_ID (1-based),
 *   OutRun 2 SP   "SBJE", twice: the same, its structure two bytes longer; the
 *                 game's own checksum (the Internet one, over the first 0x2C
 *                 bytes) sits at +0x2C (OutRun 2) or +0x2E (SP)
 *   Maximum Tune  "SBKD0000" (2) / "SBHQ0000" (1) in the backup memory:
 *                 +0xA PCB ID (0-based), +0 the u32 byte sum of the rest of
 *                 the block; the game counts its machines itself */

static uint16_t internet_checksum(const uint8_t *b, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) sum += b[i] | (b[i + 1] << 8);
    if (len & 1) sum += b[len - 1];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return sum == 0xFFFF ? 0xFFFF : (uint16_t)~sum;
}

static uint16_t byte_sum16(const uint8_t *b, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += b[i];
    return (uint16_t)sum;
}

/* The next record with that 8-byte tag and kind at or after `from` whose
 * `need` bytes of data fit before `len`, or -1. */
static int record_find(const uint8_t *mem, size_t len, const char *tag,
                       uint16_t kind, size_t need, size_t from)
{
    for (size_t pos = from; pos + 12 + need <= len; pos++) {
        if (memcmp(mem + pos, tag, 8) == 0 && lduw_le_p(mem + pos + 8) == kind)
            return (int)pos;
    }
    return -1;
}

static void link_write_ollie_king(uint8_t *ic11, int cabinets, int cabinet)
{
    for (int pos = record_find(ic11, 512, "SBHF\0\0\0\0", 3, 12, 0); pos >= 0;
         pos = record_find(ic11, 512, "SBHF\0\0\0\0", 3, 12, pos + 12)) {
        uint8_t *data = ic11 + pos + 12;
        data[5] = cabinets;
        data[6] = cabinet - 1;
        stw_le_p(ic11 + pos + 10, byte_sum16(data, 12));
    }
}

static void link_write_outrun2(uint8_t *ic11, const char *tag, int own_checksum_at,
                               int cabinets, int cabinet)
{
    for (int pos = record_find(ic11, 512, tag, 12, 52, 0); pos >= 0;
         pos = record_find(ic11, 512, tag, 12, 52, pos + 12)) {
        uint8_t *data = ic11 + pos + 12;
        data[6] = cabinets;
        data[7] = cabinet;
        stw_le_p(data + own_checksum_at, internet_checksum(data, 0x2C));
        stw_le_p(ic11 + pos + 10, byte_sum16(data, 52));
    }
}

static void link_write_maximum_tune(uint8_t *extmem, const char *tag, int cabinet)
{
    for (size_t pos = 0x8000; pos + 12 <= 0x10000; pos++) {
        if (memcmp(extmem + pos, tag, 8) != 0) continue;
        size_t size = (size_t)lduw_le_p(extmem + pos + 8) * 4;
        uint8_t *data = extmem + pos + 12;
        if (size < 0x10 || pos + 12 + size > 0x10000) return;
        data[0xA] = cabinet - 1;
        uint32_t sum = 0;
        for (size_t i = 4; i < size; i++) sum += data[i];
        stl_le_p(data, sum);
        return;
    }
}

static void chihiro_apply_link_settings(ChihiroUSBState *s)
{
    int cabinets = g_config.chihiro.link.cabinets;
    int cabinet = g_config.chihiro.link.cabinet;

    if (!g_config.chihiro.link.enable) return;
    if (cabinets < 2 || cabinets > 4 || cabinet < 1 || cabinet > cabinets) return;
    link_write_ollie_king(s->ic11, cabinets, cabinet);
    link_write_outrun2(s->ic11, "OUTRUN2\0", 0x2C, cabinets, cabinet);
    link_write_outrun2(s->ic11, "SBJE\0\0\0\0", 0x2E, cabinets, cabinet);
    link_write_maximum_tune(s->extmem[1], "SBKD0000", cabinet);
    link_write_maximum_tune(s->extmem[1], "SBHQ0000", cabinet);
}

static void handle_reset(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    const char *id = s->is_qc ? "QC" : "SC";
    fprintf(stderr, "[%07lld] chihiro-usb [%s]: USB RESET (lle=%d)\n", TS_MS, id, s->use_lle);
    if (s->is_qc) {
        s->eeprom[0x1F00] = chihiro_region_byte();
        chihiro_apply_freeplay(s);
        chihiro_apply_link_settings(s);
    }
    if (s->use_lle) {
        s->an2131.usbirq |= USBIRQ_URES;
        s->an2131.cpu.irq_recheck = true;
        an2131_run(&s->an2131, 10000);
    }
}


static void handle_control(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    const char *id = s->is_qc ? "QC" : "SC";

    int bRequest = request & 0xFF;

    /* LLE path: route ALL control requests through 8051 firmware.
     * The firmware's SUDAV dispatch table (CODE:02C7) handles standard USB
     * requests (GET_DESCRIPTOR at CODE:02E3, SET_CONFIGURATION at CODE:033B,
     * etc.) AND vendor requests (0x16-0x30 via CODE:069B).
     * Exceptions handled by AN2131 silicon, not firmware:
     *   - SET_ADDRESS (0x05): silicon writes FNADDR automatically
     *   - ANCHOR_LOAD (0xA0): silicon-level firmware download */
    if (s->use_lle && bRequest != 0xA0) {
        if (bRequest == 0x05) {
            dev->addr = value;
            return;
        }
        uint8_t setup[8];
        setup[0] = (uint8_t)(request >> 8);  /* bmRequestType */
        setup[1] = (uint8_t)bRequest;
        setup[2] = (uint8_t)(value & 0xFF);
        setup[3] = (uint8_t)(value >> 8);
        setup[4] = (uint8_t)(index & 0xFF);
        setup[5] = (uint8_t)(index >> 8);
        setup[6] = (uint8_t)(length & 0xFF);
        setup[7] = (uint8_t)(length >> 8);



        const uint8_t *out_data = NULL;
        int out_len = 0;
        if (!(setup[0] & 0x80) && length > 0) {
            out_data = data;
            out_len = length;
        }

        uint8_t resp[256];
        int resp_len = an2131_setup_packet(&s->an2131, setup,
                                           out_data, out_len,
                                           resp, sizeof(resp));

        if (resp_len > 0) {
            int copy = MIN(resp_len, length);
            memcpy(data, resp, copy);
            p->actual_length = copy;
        } else {
            p->actual_length = length;
        }

        /* 0x18 excluded: logging in that path freezes the game at INITIALIZING */
        if (lpc_log_verbose && (bRequest == 0x1C || bRequest == 0x24 || bRequest == 0x15
                                || bRequest == 0x17 || bRequest == 0x16)) {
            fprintf(stderr, "[%07lld] LLE [%s] v0x%02X val=0x%04X idx=0x%04X resp_len=%d ctrl={",
                    TS_MS, id, bRequest, value, index, resp_len);
            int show = resp_len > 0 ? MIN(resp_len, 16) : MIN(length, 16);
            for (int i = 0; i < show; i++)
                fprintf(stderr, "%s0x%02X", i?",":"", data[i]);
            fprintf(stderr, "}");
            if (bRequest == 0x17) {
                if (s->an2131.ep[2].in_armed) {
                    int bc = s->an2131.ep[2].bc_in;
                    int ep_show = MIN(bc, 16);
                    fprintf(stderr, " IN2BUF[%d]={", bc);
                    for (int i = 0; i < ep_show; i++)
                        fprintf(stderr, "%s0x%02X", i?",":"", s->an2131.ram[0x1E00 + i]);
                    fprintf(stderr, "}");
                } else {
                    fprintf(stderr, " IN2BUF=NOT_ARMED");
                }
            }
            fprintf(stderr, "\n");
        }

        if (lpc_log_verbose && !s->is_qc &&
            (bRequest == 0x1A || bRequest == 0x1B || bRequest == 0x22 || bRequest == 0x23)) {
            static int sc_uart_log = 0;
            static int sc_uart_1a_count = 0, sc_uart_1b_count = 0;
            static int64_t sc_uart_last_summary = 0;
            if (bRequest == 0x1A) sc_uart_1a_count++;
            if (bRequest == 0x1B) sc_uart_1b_count++;
            bool log_this = (bRequest == 0x22 || bRequest == 0x23 || sc_uart_log < 50 ||
                             (resp_len > 0 && sc_uart_log < 500));
            if (log_this) {
                sc_uart_log++;
                fprintf(stderr, "[%lld] SC LLE v0x%02X: val=0x%04X len=%d resp_len=%d",
                        TS_MS, bRequest, value, length, resp_len);
                if (resp_len > 0) {
                    fprintf(stderr, " data:");
                    for (int i = 0; i < resp_len && i < 16; i++)
                        fprintf(stderr, " %02X", resp[i]);
                }
                if (bRequest == 0x22 && out_len > 0) {
                    fprintf(stderr, " out:");
                    for (int i = 0; i < out_len && i < 16; i++)
                        fprintf(stderr, " %02X", out_data[i]);
                }
                fprintf(stderr, "\n");
            }
            int64_t now = TS_MS;
            if (now - sc_uart_last_summary >= 5000) {
                fprintf(stderr, "[%ld] SC UART summary: 0x1A=%d 0x1B=%d polls\n",
                        (long)now, sc_uart_1a_count, sc_uart_1b_count);
                sc_uart_last_summary = now;
            }
        }

        return;
    }

    /* Vendor request map (all handled by 8051 firmware in LLE mode):
     *
     * QC (baseboard controller):
     *   0x16  Read ic10 EEPROM #1 → bulk EP1 IN
     *   0x17  Read baseboard EEPROM ic11 (24LC024) → bulk EP2 IN
     *   0x18  Read external memory / ACBU write-complete status poll → EP3 IN
     *   0x19  JVS poll (triggers EP4 IN arm with switch/analog data)
     *   0x1C  Read RTC (BCD time) → bulk EP5 IN
     *   0x1D  Write ic10 EEPROM #1
     *   0x1E  Write ic11 EEPROM #2 via EP2 OUT
     *   0x1F  Write external memory (ACBU backup) via EP3 OUT
     *   0x20  JVS send (payload in SETUP data, firmware sends via SBUF1)
     *   0x24  Write RTC
     *   0x30  External interrupt control
     *
     * SC (serial controller):
     *   0x1A  Get UART0 data (RS-232C: the card reader)
     *   0x1B  Get UART1 data (MIDI: the drive board, or the HW210 readers)
     *   0x22  Send UART0 data
     *   0x23  Send UART1 data
     *   0x25-0x2F  UART config
     *   0x31  Set PORTB pins
     *
     * Silicon-level (not firmware):
     *   0xA0  ANCHOR_LOAD — EZ-USB firmware download (Cypress AN2131)
     */

    switch (bRequest) {
    case 0xA0: /* ANCHOR_LOAD — EZ-USB firmware download (Cypress AN2131) */
    {
        uint16_t ram_addr = value;
        int count = length;
        bool is_read = (request >> 8) & 0x80;
        if (!is_read) {
            if (lpc_log_verbose && count > 0 && count <= 8) {
                fprintf(stderr, "[%07lld] ANCHOR_LOAD addr=0x%04X len=%d data[]={",
                       TS_MS, ram_addr, count);
                for (int i = 0; i < count && i < 8; i++)
                    fprintf(stderr, "%s0x%02X", i?",":"", data[i]);
                fprintf(stderr, "}\n");
            }
            an2131_anchor_load(&s->an2131, ram_addr, data, count);
            s->fw_bytes_written += count;
            if (lpc_log_verbose && ram_addr != 0x7F92 && count > 0) {
                fprintf(stderr, "[%07lld] chihiro-usb [%s]: ANCHOR_LOAD addr=0x%04X len=%d (total %u)\n",
                       TS_MS, id, ram_addr, count, s->fw_bytes_written);
            }
            if (ram_addr == 0x7F92) {
                bool hold = (count > 0 && data[0] & 0x01);
                fprintf(stderr, "[%07lld] chihiro-usb [%s]: ANCHOR_LOAD CPUCS=%s (total %u bytes)\n",
                       TS_MS, id, hold ? "HOLD" : "RUN", s->fw_bytes_written);
                if (s->fw_cpu_held && !hold) {
                    s->use_lle = s->an2131.cpu_running;
                    if (s->use_lle) {
                        timer_mod(s->lle_tick_timer,
                                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
                    }
                    fprintf(stderr, "[%07lld] chihiro-usb [%s]: FW LOADED\n",
                            TS_MS, id);
                }
                s->fw_cpu_held = hold;
            }
        }
        break;
    }
    default:
        break;
    }

    p->actual_length = length;
}

static void handle_data(USBDevice *dev, USBPacket *p)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    const char *id = s->is_qc ? "QC" : "SC";
    int ep = p->ep->nr;

    /* The AN2131 has EP0-EP7 only. */
    if (ep >= AN2131_EP_COUNT) {
        p->status = USB_RET_STALL;
        return;
    }

    /* LLE path: route bulk transfers through AN2131 firmware */
    if (s->use_lle) {
        if (p->pid == USB_TOKEN_IN) {
            int avail = an2131_ep_in_poll(&s->an2131, ep);
            if (lpc_log_verbose && !s->is_qc && (ep == 1 || ep == 2)) {
                static int ep_poll_log = 0;
                if (ep_poll_log < 100) {
                    ep_poll_log++;
                    fprintf(stderr, "[%lld] SC BULK EP%d IN poll: avail=%d\n",
                            TS_MS, ep, avail);
                }
            }
            if (avail >= 0) {
                uint8_t buf[64];
                int got = an2131_ep_in_read(&s->an2131, ep, buf, MIN((int)p->iov.size, (int)sizeof(buf)));
                if (got > 0) {
                    if (lpc_log_verbose && !s->is_qc && (ep == 1 || ep == 2)) {
                        fprintf(stderr, "[%lld] SC BULK EP%d IN: %d bytes:", TS_MS, ep, got);
                        for (int i = 0; i < got && i < 32; i++)
                            fprintf(stderr, " %02X", buf[i]);
                        fprintf(stderr, "\n");
                    }
                    usb_packet_copy(p, buf, got);
                }
            } else {
                p->status = USB_RET_NAK;
            }
        } else {
            int len = p->iov.size;
            uint8_t buf[64];
            int chunk = MIN(len, (int)sizeof(buf));
            /* Silicon back-pressure: the OUT endpoint NAKs while the firmware
             * has not consumed the previous packet (it re-arms by writing
             * OUTnBC). Overwriting the buffer instead corrupts multi-chunk
             * frames — the card WRITE command in particular. */
            if (s->an2131.ep[ep].cs_out & EPCS_BSY) {
                p->status = USB_RET_NAK;
                return;
            }
            usb_packet_copy(p, buf, chunk);
            if (!s->is_qc) {
                CHIHIRO_LOG_HEX(USB, buf, MIN(chunk, 32),
                                "%s BULK EP%d OUT: %d bytes:", id, ep, chunk);
            }
            an2131_ep_out_write(&s->an2131, ep, buf, chunk);
            an2131_run(&s->an2131, 2000);
        }
        return;
    }

    /* Not reached once the AN2131s run their firmware (use_lle, set after
     * the B2 boot), which serves the bulk endpoints: for the QC, vendor
     * 0x16/0x17 → EP1/EP2 and 0x1E/0x1F → EP2/EP3; for the SC, UART data
     * on EP1-EP3. */
    p->status = USB_RET_NAK;
}

static void chihiro_an2131qc_realize(USBDevice *dev, Error **errp)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    s->is_qc = true;
    dev->auto_attach = 0;  /* Attach later via hotplug timer */

    /* Load ic10 QC EEPROM firmware (8192 bytes).
     * Contains AN2131 8051 firmware code + region/serial/game data.
     * SEGABOOT reads this via vendor request 0x16 + bulk IN EP1. */
    if (chihiro_ic10_data && chihiro_ic10_size == sizeof(s->eeprom)) {
        memcpy(s->eeprom, chihiro_ic10_data, sizeof(s->eeprom));
    } else {
        error_setg(errp, "Chihiro QC: ic10 EEPROM dump required (ic10_g24lc64.bin)");
        return;
    }

    usb_desc_init(dev);
    /* The cabinet's region at eeprom[0x1F00], 1 Japan, 2 USA, 3 Export,
     * over whatever the ic10 dump carried: see chihiro_region_byte(). */
    s->eeprom[0x1F00] = chihiro_region_byte();

    /* Load ic11 baseboard EEPROM (the 128-byte dump of a 24LC024) */
    memset(s->ic11, 0, sizeof(s->ic11));
    if (chihiro_ic11_data && chihiro_ic11_size <= sizeof(s->ic11)) {
        memcpy(s->ic11, chihiro_ic11_data, chihiro_ic11_size);
    } else {
        error_setg(errp, "Chihiro QC: ic11 EEPROM dump required (ic11_24lc024.bin)");
        return;
    }

    memset(s->extmem, 0, sizeof(s->extmem));
    chihiro_apply_freeplay(s);
    chihiro_apply_link_settings(s);

    /* Initialize EZ-USB firmware state */
    s->fw_bytes_written = 0;
    s->fw_cpu_held = false;

    chihiro_jvs_init(&s->jvs);
    chihiro_jvs_global = &s->jvs;

    /* AN2131 LLE: init 8051 CPU + register layer, wire EEPROMs + extmem */
    an2131_init(&s->an2131);
    s->an2131.is_qc = true;
    s->an2131.ic10_eeprom = s->eeprom;
    s->an2131.ic10_size = sizeof(s->eeprom);
    s->an2131.ic11_eeprom = s->ic11;
    s->an2131.ic11_size = 256;
    s->an2131.extmem = s->extmem[0];

    /* B2 boot: parse ic10 EEPROM firmware and start 8051 CPU */
    an2131_b2_boot(&s->an2131, s->eeprom, sizeof(s->eeprom));
    s->use_lle = s->an2131.cpu_running;

    s->lle_tick_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, lle_tick_cb, s);
    if (s->use_lle) {
        timer_mod(s->lle_tick_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
    }

    chihiro_qc_instance = s;

    printf("[%07lld] Chihiro QC: ic10 + ic11 loaded, region=0x%02X, LLE=%s\n",
           TS_MS, s->eeprom[0x1F00], s->use_lle ? "ACTIVE" : "OFF");
}

static void chihiro_an2131qc_unrealize(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    timer_free(s->lle_tick_timer);
}

/* ── Migration state ─────────────────────────────────────────────────
 * Plain data only: every pointer and callback is re-attached in
 * post_load, mirroring what realize wires up. */
static const VMStateDescription vmstate_cpu8051 = {
    .name = "chihiro-usb/cpu8051",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(pc, Cpu8051State),
        VMSTATE_UINT8(sp, Cpu8051State),
        VMSTATE_UINT8(acc, Cpu8051State),
        VMSTATE_UINT8(b, Cpu8051State),
        VMSTATE_UINT16(dptr, Cpu8051State),
        VMSTATE_UINT16(dptr_alt, Cpu8051State),
        VMSTATE_UINT8(psw, Cpu8051State),
        VMSTATE_UINT8_ARRAY(iram, Cpu8051State, 256),
        VMSTATE_UINT8_ARRAY(sfr, Cpu8051State, 128),
        VMSTATE_UNUSED(8192),
        VMSTATE_UNUSED(1),
        VMSTATE_BOOL(in_interrupt, Cpu8051State),
        VMSTATE_UNUSED(8),
        VMSTATE_UINT8(timer0_prescale, Cpu8051State),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_an2131 = {
    .name = "chihiro-usb/an2131",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(cpu, AN2131State, 1, vmstate_cpu8051, Cpu8051State),
        VMSTATE_UINT8_ARRAY(ram, AN2131State, AN2131_RAM_SIZE),
        /* ep[] is saved as raw bytes. An I2C transfer in flight is not
         * saved: an2131_relink drops it to idle. */
        VMSTATE_BUFFER_UNSAFE(ep, AN2131State, 1,
                              sizeof(((AN2131State *)0)->ep)),
        VMSTATE_UINT8(ep0cs, AN2131State),
        VMSTATE_UINT8(ivec, AN2131State),
        VMSTATE_UINT8(in07irq, AN2131State),
        VMSTATE_UINT8(out07irq, AN2131State),
        VMSTATE_UINT8(usbirq, AN2131State),
        VMSTATE_UINT8(in07ien, AN2131State),
        VMSTATE_UINT8(out07ien, AN2131State),
        VMSTATE_UINT8(usbien, AN2131State),
        VMSTATE_UINT8(usbbav, AN2131State),
        VMSTATE_UINT8(i2cs, AN2131State),
        VMSTATE_UNUSED(1),
        VMSTATE_BOOL(i2c_irq_pending, AN2131State),
        VMSTATE_BOOL(i2c_lastrd, AN2131State),
        VMSTATE_UINT8_ARRAY(rtc_regs, AN2131State, 16),
        /* Where a Crazy Taxi backup workaround kept its range and flag, gone
         * with the SRAM's second half. */
        VMSTATE_UNUSED(7),
        VMSTATE_UINT8(cpucs, AN2131State),
        VMSTATE_UINT8(usbcs, AN2131State),
        VMSTATE_UINT8(fnaddr, AN2131State),
        VMSTATE_UINT8(togctl, AN2131State),
        VMSTATE_UINT8(usbpair, AN2131State),
        VMSTATE_UINT8(in07val, AN2131State),
        VMSTATE_UINT8(out07val, AN2131State),
        VMSTATE_UINT16(sudptr, AN2131State),
        VMSTATE_UINT8(fastxfr, AN2131State),
        VMSTATE_UINT16(autoptr, AN2131State),
        VMSTATE_UINT8_ARRAY(setupdat, AN2131State, 8),
        VMSTATE_UINT8(outa, AN2131State),
        VMSTATE_UINT8(outb, AN2131State),
        VMSTATE_UINT8(outc, AN2131State),
        VMSTATE_UINT8(pinsa, AN2131State),
        VMSTATE_UINT8(pinsb, AN2131State),
        VMSTATE_UINT8(pinsc, AN2131State),
        VMSTATE_UINT8(oea, AN2131State),
        VMSTATE_UINT8(oeb, AN2131State),
        VMSTATE_UINT8(oec, AN2131State),
        VMSTATE_UINT8(portacfg, AN2131State),
        VMSTATE_UINT8(portbcfg, AN2131State),
        VMSTATE_UINT8(portccfg, AN2131State),
        VMSTATE_UINT8(exif, AN2131State),
        VMSTATE_UINT8(eie, AN2131State),
        VMSTATE_UINT8(eip, AN2131State),
        VMSTATE_UINT8(mpage, AN2131State),
        VMSTATE_UINT8(dps, AN2131State),
        VMSTATE_UINT8_ARRAY(jvs_tx_buf, AN2131State, 256),
        VMSTATE_INT32(jvs_tx_len, AN2131State),
        VMSTATE_INT32(jvs_tx_expected, AN2131State),
        VMSTATE_BOOL(jvs_tx_escape, AN2131State),
        VMSTATE_UINT8_ARRAY(jvs_rx_buf, AN2131State, 512),
        VMSTATE_INT32(jvs_rx_len, AN2131State),
        VMSTATE_INT32(jvs_rx_pos, AN2131State),
        VMSTATE_BOOL(cpu_running, AN2131State),
        VMSTATE_BOOL(jvs_response_ready, AN2131State),
        VMSTATE_BOOL(jvs_rx_pending, AN2131State),
        VMSTATE_UINT64(total_cycles, AN2131State),
        VMSTATE_UINT64(jvs_response_set_cycles, AN2131State),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_chihiro_jvs = {
    .name = "chihiro-usb/jvs",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(device_id, ChihiroJVSState),
        VMSTATE_UINT8(sense, ChihiroJVSState),
        VMSTATE_UINT16_ARRAY(coin_count, ChihiroJVSState, JVS_MAX_COINS),
        VMSTATE_UINT8(reset_count, ChihiroJVSState),
        VMSTATE_UINT8(system_switches, ChihiroJVSState),
        VMSTATE_UINT8_2DARRAY(player_switches, ChihiroJVSState,
                              JVS_MAX_PLAYERS, 2),
        VMSTATE_UINT16_ARRAY(analog, ChihiroJVSState, JVS_MAX_ANALOG),
        VMSTATE_END_OF_LIST()
    }
};

static int chihiro_usb_pre_load(void *opaque)
{
    ChihiroUSBState *s = opaque;

    s->extmem_backup_loaded = false;
    return 0;
}

static int chihiro_usb_post_load(void *opaque, int version_id)
{
    ChihiroUSBState *s = opaque;
    AN2131State *a = &s->an2131;

    /* A snapshot file comes from outside: JVS positions that would reach
     * past their buffers refuse it. */
    if (a->jvs_tx_len < 0 || a->jvs_tx_len > (int)sizeof(a->jvs_tx_buf) ||
        a->jvs_rx_len < 0 || a->jvs_rx_len > (int)sizeof(a->jvs_rx_buf) ||
        a->jvs_rx_pos < 0 || a->jvs_rx_pos > a->jvs_rx_len) {
        /* Cleared too: a failed load can still be resumed. */
        a->jvs_tx_len = a->jvs_tx_expected = 0;
        a->jvs_rx_len = a->jvs_rx_pos = 0;
        return -EINVAL;
    }

    /* A snapshot from before the SRAM had two halves carries one, which
     * held the firmware's work and the game's backup at once: it is both. */
    if (s->is_qc && !s->extmem_backup_loaded)
        memcpy(s->extmem[1], s->extmem[0], sizeof(s->extmem[1]));

    an2131_relink(&s->an2131);
    s->an2131.ic10_eeprom = s->eeprom;
    s->an2131.ic10_size = sizeof(s->eeprom);
    s->an2131.ic11_eeprom = s->ic11;
    s->an2131.ic11_size = 256;
    s->an2131.extmem = s->extmem[0];

    if (s->use_lle && s->an2131.cpu_running) {
        timer_mod(s->lle_tick_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
    }
    return 0;
}

static bool chihiro_usb_backup_half_needed(void *opaque)
{
    ChihiroUSBState *s = opaque;

    return s->is_qc;
}

static int chihiro_usb_backup_half_post_load(void *opaque, int version_id)
{
    ChihiroUSBState *s = opaque;

    s->extmem_backup_loaded = true;
    return 0;
}

/* The SRAM half with the game's backup, which snapshots from before it had
 * two halves do not carry. */
static const VMStateDescription vmstate_chihiro_usb_backup_half = {
    .name = "chihiro-usb/backup-half",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = chihiro_usb_backup_half_needed,
    .post_load = chihiro_usb_backup_half_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(extmem[1], ChihiroUSBState, 65536),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_chihiro_usb = {
    .name = "chihiro-usb",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = chihiro_usb_pre_load,
    .post_load = chihiro_usb_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_USB_DEVICE(dev, ChihiroUSBState),
        VMSTATE_UINT8_ARRAY(eeprom, ChihiroUSBState, 8192),
        VMSTATE_UINT32(fw_bytes_written, ChihiroUSBState),
        VMSTATE_BOOL(fw_cpu_held, ChihiroUSBState),
        VMSTATE_UNUSED(2),
        VMSTATE_UINT8_ARRAY(ic11, ChihiroUSBState, 512),
        VMSTATE_UINT8_ARRAY(extmem[0], ChihiroUSBState, 65536),
        VMSTATE_STRUCT(jvs, ChihiroUSBState, 1,
                       vmstate_chihiro_jvs, ChihiroJVSState),
        VMSTATE_STRUCT(an2131, ChihiroUSBState, 1,
                       vmstate_an2131, AN2131State),
        VMSTATE_BOOL(use_lle, ChihiroUSBState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_chihiro_usb_backup_half,
        NULL
    }
};

static void chihiro_an2131qc_class_init(ObjectClass *klass, const void *data)
{
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    uc->realize        = chihiro_an2131qc_realize;
    uc->unrealize      = chihiro_an2131qc_unrealize;
    uc->product_desc   = "Chihiro an2131qc";
    uc->usb_desc       = &desc_chihiro_an2131qc;

    uc->handle_reset   = handle_reset;
    uc->handle_control = handle_control;
    uc->handle_data    = handle_data;
    uc->handle_attach  = usb_desc_attach;

    dc->vmsd = &vmstate_chihiro_usb;
}

static const TypeInfo chihiro_an2131qc_info = {
    .name          = "chihiro-an2131qc",
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(ChihiroUSBState),
    .class_init    = chihiro_an2131qc_class_init,
};

/* The card that comes back may be an older state of a card whose file has
 * moved on since, and writing it there would undo those games. It comes back
 * with no file, so its next write makes a new one. */
static int crp1231_pre_load(void *opaque)
{
    CRP1231State *r = opaque;

    r->card_path[0] = '\0';
    return 0;
}

/* A snapshot file comes from outside: positions that would reach past the
 * reader's buffers refuse it. */
static int crp1231_post_load(void *opaque, int version_id)
{
    CRP1231State *r = opaque;

    if (r->cmd_len < 0 || r->cmd_len > CRP1231_CMD_MAX ||
        (r->cmd_expect != 0 &&
         (r->cmd_expect < 4 || r->cmd_expect > CRP1231_CMD_MAX)) ||
        r->resp_len < 0 || r->resp_len > CRP1231_RESP_MAX ||
        r->resp_pos < 0 || r->resp_pos > r->resp_len ||
        r->last_len < 0 || r->last_len > (int)sizeof(r->last_frame)) {
        /* Cleared too: a failed load can still be resumed. */
        r->cmd_len = r->cmd_expect = 0;
        r->resp_len = r->resp_pos = 0;
        r->last_len = 0;
        return -EINVAL;
    }
    return 0;
}

/* Half a command in and half an answer out: a snapshot that loses either
 * leaves the reader and the game waiting on each other. */
static const VMStateDescription vmstate_chihiro_cardreader_crp1231 = {
    .name = "chihiro-cardreader-crp1231",
    .version_id = 5,
    .minimum_version_id = 5,
    .pre_load = crp1231_pre_load,
    .post_load = crp1231_post_load,
    .fields = (const VMStateField[]) {
        /* card_path is dropped on load (crp1231_pre_load). */
        VMSTATE_UINT8_ARRAY(cmd, CRP1231State, CRP1231_CMD_MAX),
        VMSTATE_INT32(cmd_len, CRP1231State),
        VMSTATE_INT32(cmd_expect, CRP1231State),
        VMSTATE_UINT8_ARRAY(resp, CRP1231State, CRP1231_RESP_MAX),
        VMSTATE_INT32(resp_len, CRP1231State),
        VMSTATE_INT32(resp_pos, CRP1231State),
        VMSTATE_UINT8_ARRAY(last_frame, CRP1231State, 80),
        VMSTATE_INT32(last_len, CRP1231State),
        VMSTATE_BOOL(have_frame, CRP1231State),
        /* An answer still waiting for its ENQ. */
        VMSTATE_BOOL(answer_pending, CRP1231State),
        /* The card travels with the snapshot. */
        VMSTATE_UINT8_ARRAY(card, CRP1231State, CRP1231_CARD_BYTES),
        VMSTATE_INT32(card_pos, CRP1231State),
        VMSTATE_UINT8(last_cmd, CRP1231State),
        VMSTATE_UINT8(last_param, CRP1231State),
        VMSTATE_BOOL(card_written, CRP1231State),
        /* A take-in the game is still waiting on: the answer to its next
         * ENQ depends on it. */
        VMSTATE_BOOL(waiting, CRP1231State),
        VMSTATE_END_OF_LIST()
    }
};

/* Positions out of a snapshot file that would reach past the steering
 * board's command and queues refuse it too. */
static int v257_post_load(void *opaque, int version_id)
{
    V257DriveBoard *db = opaque;

    if (db->cmd_pos < 0 || db->cmd_pos >= V257_CMD_LEN ||
        db->resp_head < 0 || db->resp_head >= V257_RESP_SIZE ||
        db->resp_tail < 0 || db->resp_tail >= V257_RESP_SIZE ||
        db->pending_head < 0 || db->pending_head >= V257_MSG_SLOTS ||
        db->pending_tail < 0 || db->pending_tail >= V257_MSG_SLOTS) {
        db->cmd_pos = 0;
        db->resp_head = db->resp_tail = 0;
        db->pending_head = db->pending_tail = 0;
        return -EINVAL;
    }
    return 0;
}

/* A snapshot taken mid-frame has half a command on the wire and an answer
 * still queued; losing either stalls the game's send-then-wait loop. */
static const VMStateDescription vmstate_chihiro_driveboard_v257 = {
    .name = "chihiro-driveboard-v257",
    .version_id = 3,
    .minimum_version_id = 3,
    .post_load = v257_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(cmd, V257DriveBoard, V257_CMD_LEN),
        VMSTATE_INT32(cmd_pos, V257DriveBoard),
        VMSTATE_UINT8_ARRAY(resp, V257DriveBoard, V257_RESP_SIZE),
        VMSTATE_INT32(resp_head, V257DriveBoard),
        VMSTATE_INT32(resp_tail, V257DriveBoard),
        VMSTATE_UINT16(position, V257DriveBoard),
        VMSTATE_UINT16(wheel, V257DriveBoard),
        VMSTATE_UINT8(force_a, V257DriveBoard),
        VMSTATE_UINT8(force_b, V257DriveBoard),
        VMSTATE_INT8(torque, V257DriveBoard),
        VMSTATE_BOOL(motor_on, V257DriveBoard),
        VMSTATE_BOOL(motor_known, V257DriveBoard),
        VMSTATE_UINT8_2DARRAY(pending, V257DriveBoard, V257_MSG_SLOTS,
                              V257_MSG_LEN),
        VMSTATE_INT32(pending_head, V257DriveBoard),
        VMSTATE_INT32(pending_tail, V257DriveBoard),
        VMSTATE_BOOL_ARRAY(pending_twice, V257DriveBoard, V257_MSG_SLOTS),
        VMSTATE_UINT8(state, V257DriveBoard),
        VMSTATE_INT32(countdown, V257DriveBoard),
        VMSTATE_INT32(torque_ticks, V257DriveBoard),
        VMSTATE_END_OF_LIST()
    }
};

/* Positions out of a snapshot file that would reach past the drive board's
 * frame and answer queue refuse it too. */
static int driveboard_post_load(void *opaque, int version_id)
{
    DriveBoardState *db = opaque;

    if (db->tx_pos < 0 || db->tx_pos >= (int)sizeof(db->tx_buf) ||
        db->resp_head < 0 || db->resp_head >= DRIVEBOARD_RESP_SIZE ||
        db->resp_tail < 0 || db->resp_tail >= DRIVEBOARD_RESP_SIZE) {
        db->tx_pos = 0;
        db->resp_head = db->resp_tail = 0;
        return -EINVAL;
    }
    return 0;
}

/* The game uploads its SUD effect packages once at boot; without this
 * section a loaded snapshot plays every impact as silence. */
static const VMStateDescription vmstate_chihiro_driveboard = {
    .name = "chihiro-driveboard",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = driveboard_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(tx_buf, DriveBoardState, 4),
        VMSTATE_INT32(tx_pos, DriveBoardState),
        VMSTATE_UINT8_ARRAY(resp_buf, DriveBoardState, DRIVEBOARD_RESP_SIZE),
        VMSTATE_INT32(resp_head, DriveBoardState),
        VMSTATE_INT32(resp_tail, DriveBoardState),
        VMSTATE_BOOL(motor_active, DriveBoardState),
        VMSTATE_UINT8(global_power, DriveBoardState),
        VMSTATE_BOOL(spring_active, DriveBoardState),
        VMSTATE_UINT8(damper_level, DriveBoardState),
        VMSTATE_UINT8(friction_power, DriveBoardState),
        VMSTATE_UNUSED(2),
        VMSTATE_UINT8(vibration_power, DriveBoardState),
        VMSTATE_UNUSED(1),
        VMSTATE_UINT8(movement_dir, DriveBoardState),
        VMSTATE_UINT8(movement_power, DriveBoardState),
        VMSTATE_UINT8_2DARRAY(sud_pkg, DriveBoardState, 16, 16),
        VMSTATE_UINT8(sud_up_pkg, DriveBoardState),
        VMSTATE_UINT8(sud_up_idx, DriveBoardState),
        VMSTATE_INT8(play_pkg, DriveBoardState),
        VMSTATE_UINT8(play_pos, DriveBoardState),
        VMSTATE_UINT8(play_sub, DriveBoardState),
        VMSTATE_END_OF_LIST()
    }
};

static void chihiro_an2131sc_realize(USBDevice *dev, Error **errp)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    s->is_qc = false;
    dev->auto_attach = 0;  /* Attach later via hotplug timer */

    /* Load pc20 SC EEPROM firmware (8192 bytes). */
    if (chihiro_pc20_data && chihiro_pc20_size == sizeof(s->eeprom)) {
        memcpy(s->eeprom, chihiro_pc20_data, sizeof(s->eeprom));
    } else {
        error_setg(errp, "Chihiro SC: pc20 EEPROM dump required (pc20_g24lc64.bin)");
        return;
    }

    usb_desc_init(dev);

    /* Load ic11 baseboard EEPROM (the 128-byte dump of a 24LC024) */
    memset(s->ic11, 0, sizeof(s->ic11));
    if (chihiro_ic11_data && chihiro_ic11_size <= sizeof(s->ic11)) {
        memcpy(s->ic11, chihiro_ic11_data, chihiro_ic11_size);
    } else {
        error_setg(errp, "Chihiro SC: ic11 EEPROM dump required (ic11_24lc024.bin)");
        return;
    }
    memset(s->extmem, 0, sizeof(s->extmem));

    /* Initialize EZ-USB firmware state */
    s->fw_bytes_written = 0;
    s->fw_cpu_held = false;

    chihiro_jvs_init(&s->jvs);

    static DriveBoardState driveboard_instance;
    driveboard_init(&driveboard_instance);
    chihiro_driveboard_global = &driveboard_instance;
    vmstate_register(NULL, 0, &vmstate_chihiro_driveboard, &driveboard_instance);

    static CRP1231State crp1231_instance;
    crp1231_init(&crp1231_instance);
    chihiro_crp1231_global = &crp1231_instance;
    vmstate_register(NULL, 0, &vmstate_chihiro_cardreader_crp1231,
                     &crp1231_instance);

    static V257DriveBoard v257_instance;
    v257_init(&v257_instance);
    chihiro_v257_global = &v257_instance;
    vmstate_register(NULL, 0, &vmstate_chihiro_driveboard_v257,
                     &v257_instance);

    /* AN2131 LLE: init 8051 CPU + register layer, wire EEPROMs + extmem */
    an2131_init(&s->an2131);
    s->an2131.is_qc = false;
    s->an2131.ic10_eeprom = s->eeprom;
    s->an2131.ic10_size = sizeof(s->eeprom);
    s->an2131.ic11_eeprom = s->ic11;
    s->an2131.ic11_size = 256;
    s->an2131.extmem = s->extmem[0];

    /* SC firmware init (FUN_CODE_0e9e) completes during B2 boot:
     * SCON/SCON1 |= 3 triggers serial ISRs, counters increment,
     * spin-polls pass, sc_process_cmd() runs — all within 8M cycles. */
    an2131_b2_boot(&s->an2131, s->eeprom, sizeof(s->eeprom));
    if (s->an2131.cpu_running) {
        an2131_run(&s->an2131, 8000000);
    }
    s->use_lle = s->an2131.cpu_running;

    s->lle_tick_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, lle_tick_cb, s);
    if (s->use_lle) {
        timer_mod(s->lle_tick_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
    }

    printf("[%07lld] Chihiro SC: loaded pc20 (8192B) + ic11 (128B), LLE=%s\n",
           TS_MS, s->use_lle ? "ACTIVE" : "OFF");
}

static void chihiro_an2131sc_unrealize(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    timer_free(s->lle_tick_timer);
}

static void chihiro_an2131sc_class_init(ObjectClass *klass, const void *data)
{
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    uc->realize        = chihiro_an2131sc_realize;
    uc->unrealize      = chihiro_an2131sc_unrealize;
    uc->product_desc   = "Chihiro an2131sc";
    uc->usb_desc       = &desc_chihiro_an2131sc;

    uc->handle_reset   = handle_reset;
    uc->handle_control = handle_control;
    uc->handle_data    = handle_data;
    uc->handle_attach  = usb_desc_attach;

    dc->vmsd = &vmstate_chihiro_usb;
}

static const TypeInfo chihiro_an2131sc_info = {
    .name          = "chihiro-an2131sc",
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(ChihiroUSBState),
    .class_init    = chihiro_an2131sc_class_init,
};

/* The save file: magic, version, ic11, then the SRAM's backup half in its
 * two windows. The game's logical backup addresses 0 to 0x7FFF are the
 * window at 0x8000, and from 0x8000 on it spills into the one at 0x2000
 * (V307.xbe JvsBACKUP_Read 0x001AE703, JvsBACKUP_Write 0x001AE853).
 * Version 1 files kept the first window only. */
#define CHIHIRO_SAVE_MAGIC 0x56534843  /* "CHSV" LE */
#define CHIHIRO_SAVE_VERSION 2
#define CHIHIRO_SAVE_IC11_SIZE 512
#define CHIHIRO_SAVE_EXTMEM_OFF 0x8000
#define CHIHIRO_SAVE_EXTMEM_SIZE 0x8000
#define CHIHIRO_SAVE_LOW_OFF 0x2000
#define CHIHIRO_SAVE_LOW_SIZE 0x5B40

/* A save file: "CHSV", its version, the ic11 EEPROM, then the backup half
 * from 0x8000 and, since version 2, from 0x2000 too. */
static bool chihiro_usb_save_read(const char *path, uint8_t *ic11,
                                  uint8_t *backup, uint32_t *version)
{
    FILE *f = qemu_fopen(path, "rb");
    uint32_t magic;
    bool ok;

    if (!f) return false;
    ok = fread(&magic, 4, 1, f) == 1 && magic == CHIHIRO_SAVE_MAGIC &&
         fread(version, 4, 1, f) == 1 &&
         (*version == 1 || *version == CHIHIRO_SAVE_VERSION) &&
         fread(ic11, 1, CHIHIRO_SAVE_IC11_SIZE, f) == CHIHIRO_SAVE_IC11_SIZE &&
         fread(backup + CHIHIRO_SAVE_EXTMEM_OFF, 1, CHIHIRO_SAVE_EXTMEM_SIZE,
               f) == CHIHIRO_SAVE_EXTMEM_SIZE;
    if (ok) {
        /* A version 1 file had one SRAM window: what lies past 0x7FFF starts
         * empty, and the games' checksums discard the blocks it had
         * overwritten. */
        memset(backup + CHIHIRO_SAVE_LOW_OFF, 0, CHIHIRO_SAVE_LOW_SIZE);
        ok = *version != CHIHIRO_SAVE_VERSION ||
             fread(backup + CHIHIRO_SAVE_LOW_OFF, 1, CHIHIRO_SAVE_LOW_SIZE,
                   f) == CHIHIRO_SAVE_LOW_SIZE;
    }
    fclose(f);
    return ok;
}

bool chihiro_usb_save_read_backup(const char *path, uint8_t *backup)
{
    uint8_t ic11[CHIHIRO_SAVE_IC11_SIZE];
    uint32_t version;

    return chihiro_usb_save_read(path, ic11, backup, &version);
}

/* The game that wrote a save: the identifier after "ACBU0001" in its ic11
 * (0x14, the four characters at boot.id 0x30). */
bool chihiro_usb_save_owner(const char *path, uint8_t *owner)
{
    uint8_t ic11[CHIHIRO_SAVE_IC11_SIZE];
    uint8_t *backup = g_malloc(0x10000);
    uint32_t version;
    bool ok = chihiro_usb_save_read(path, ic11, backup, &version) &&
              memcmp(ic11, "ACBU0001", 8) == 0;

    g_free(backup);
    if (ok) {
        memcpy(owner, ic11 + 0x14, 4);
    }
    return ok;
}

bool chihiro_usb_save_load(const char *path)
{
    ChihiroUSBState *s = chihiro_qc_instance;
    uint32_t version;

    if (!s || !path ||
        !chihiro_usb_save_read(path, s->ic11, s->extmem[1], &version))
        return false;
    fprintf(stderr, "Chihiro: save (version %u) loaded from %s\n", version,
            path);

    chihiro_apply_freeplay(s);
    chihiro_apply_link_settings(s);

    return true;
}

/* The baseboard's backup half as it stands right now, 64 KB, or NULL before
 * the board exists. Read-only to callers. */
const uint8_t *chihiro_usb_backup_live(void)
{
    return chihiro_qc_instance ? chihiro_qc_instance->extmem[1] : NULL;
}

/* Whether the game has written its backup since the file last caught up. */
bool chihiro_usb_save_dirty(void)
{
    return chihiro_qc_instance && chihiro_qc_instance->an2131.backup_dirty;
}

bool chihiro_usb_save_flush(const char *path)
{
    if (!chihiro_qc_instance || !path) return false;

    ChihiroUSBState *s = chihiro_qc_instance;

    uint32_t magic = CHIHIRO_SAVE_MAGIC;
    uint32_t version = CHIHIRO_SAVE_VERSION;
    ChihiroFilePart parts[] = {
        { &magic, 4 },
        { &version, 4 },
        { s->ic11, CHIHIRO_SAVE_IC11_SIZE },
        { s->extmem[1] + CHIHIRO_SAVE_EXTMEM_OFF, CHIHIRO_SAVE_EXTMEM_SIZE },
        { s->extmem[1] + CHIHIRO_SAVE_LOW_OFF, CHIHIRO_SAVE_LOW_SIZE },
    };
    if (!chihiro_file_replace(path, parts, ARRAY_SIZE(parts))) {
        return false;
    }
    s->an2131.backup_dirty = false;
    return true;
}

static void chihiro_usb_register_types(void)
{
    type_register_static(&chihiro_an2131qc_info);
    type_register_static(&chihiro_an2131sc_info);
}

type_init(chihiro_usb_register_types)
