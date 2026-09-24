/*
 * QEMU Chihiro USB Devices
 *
 * Copyright (c) 2016 espes
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
#include "chihiro-firmware.h"
#include "chihiro-jvs.h"
#define TS_MS ((long long)(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)))
#define DEBUG_CUSB
#ifdef DEBUG_CUSB
#define DPRINTF(s, ...) do { } while(0)
#else
#define DPRINTF(...)
#endif

typedef struct ChihiroUSBState {
    USBDevice dev;

    /* Device identity (set at realize, safe to use in all callbacks) */
    bool is_qc;  /* true = AN2131QC (PID 0x0002), false = AN2131SC (PID 0x0003) */

    /* I2C EEPROM data (8KB): ic10 for QC, pc20 for SC — loaded at realize */
    uint8_t eeprom[8192];

    /* Per-endpoint bulk IN buffers (EP1–EP5, index 0 unused) */
    #define CHIHIRO_USB_MAX_EP 6
    #define CHIHIRO_USB_EP_BUFSZ 1024
    struct {
        uint8_t buf[CHIHIRO_USB_EP_BUFSZ];
        int pending;
        int offset;
    } ep_in[CHIHIRO_USB_MAX_EP];

    /* EZ-USB firmware download state (ANCHOR_LOAD / bRequest 0xA0) */
    uint32_t fw_bytes_written;  /* total bytes received via 0xA0 */
    bool fw_cpu_held;           /* true if CPUCS register set to hold CPU */

    /* ic11 EEPROM (512 bytes) — baseboard config, "ACBU0001" + game ID */
    uint8_t ic11[512];

    /* External memory (64KB, mapped at 0x0000–0xFFFF on the AN2131 8051) */
    uint8_t extmem[65536];

    /* Pending write tracking for 0x1E (ic11 via EP2) and 0x1F (extmem via EP3) */
    uint16_t write_1e_addr;
    uint16_t write_1f_addr;

    /* SC UART buffers (for JVS communication) */
    uint8_t uart0_rx[256];  /* UART0 receive buffer */
    int uart0_rx_len;
    uint8_t uart1_rx[256];  /* UART1 / JVS receive buffer */
    int uart1_rx_len;

    /* v202: instrumentation counters (read/reset by DIAG timer) */
    uint32_t nak_count;    /* bulk IN NAK count since last report */
    uint32_t bulk_in_count;  /* successful bulk IN count */
    uint32_t bulk_out_count; /* bulk OUT count */

    /* v302: EZ-USB firmware reboot simulation timers.
     * Real AN2131 loads firmware from EEPROM after initial enumeration,
     * then disconnects and reconnects. SEGABOOT waits for the CSC. */
    QEMUTimer *ezusb_disconnect_timer;
    QEMUTimer *ezusb_reconnect_timer;
    bool ezusb_rebooted;

    /* JVS I/O board emulation state (shared between QC and SC paths) */
    ChihiroJVSState jvs;
} ChihiroUSBState;

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

/* v302: EZ-USB firmware reboot — disconnect callback */
static void ezusb_disconnect_cb(void *opaque)
{
    ChihiroUSBState *s = (ChihiroUSBState *)opaque;
    USBDevice *dev = &s->dev;
    const char *id = s->is_qc ? "QC" : "SC";

    if (!dev->attached) {
        if(0) printf("[%07lld] chihiro-usb [%s]: v302 disconnect skipped (already detached)\n", TS_MS, id);
        return;
    }

    if(0) printf("[%07lld] chihiro-usb [%s]: ★ v302 EZ-USB firmware reboot — DISCONNECT (CSC will fire)\n", TS_MS, id);
    usb_device_detach(dev);

    /* Schedule reconnect 50ms later */
    timer_mod(s->ezusb_reconnect_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
}

/* v302: EZ-USB firmware reboot — reconnect callback */
static void ezusb_reconnect_cb(void *opaque)
{
    ChihiroUSBState *s = (ChihiroUSBState *)opaque;
    USBDevice *dev = &s->dev;
    const char *id = s->is_qc ? "QC" : "SC";

    if (dev->attached) {
        if(0) printf("[%07lld] chihiro-usb [%s]: v302 reconnect skipped (already attached)\n", TS_MS, id);
        return;
    }

    if(0) printf("[%07lld] chihiro-usb [%s]: ★ v302 EZ-USB firmware reboot — RECONNECT (CSC will fire → Phase 2)\n", TS_MS, id);
    usb_device_attach(dev, &error_abort);
}

static void handle_reset(USBDevice *dev)
{
    const char *id = ((ChihiroUSBState *)dev)->is_qc ? "QC" : "SC";
    if(0) printf("[%07lld] chihiro-usb [%s]: device reset\n", TS_MS, id);
    fflush(stdout);
}

static uint64_t jvs_send_count = 0;
static uint64_t jvs_recv_count = 0;
static uint64_t jvs_recv_has_data = 0;
static uint64_t vendor_req_counts[256] = {0};

static void handle_control(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    extern uint64_t perf_cnt_usb_control;
    perf_cnt_usb_control++;
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    const char *id = ((ChihiroUSBState *)dev)->is_qc ? "QC" : "SC";

    if(0) printf("[%07lld] chihiro-usb [%s]: control req=0x%04X val=0x%04X idx=0x%04X len=%d [%s]\n", TS_MS,
           id, request, value, index, length,
           (request == 0x8006 && (value >> 8) == 1) ? "GET_DESCRIPTOR(DEVICE)" :
           (request == 0x8006 && (value >> 8) == 2) ? "GET_DESCRIPTOR(CONFIG)" :
           (request == 0x8006 && (value >> 8) == 3) ? "GET_DESCRIPTOR(STRING)" :
           (request == 0x0005) ? "SET_ADDRESS" :
           (request == 0x0009) ? "SET_CONFIG" :
           (request == 0x010B) ? "SET_INTERFACE" :
           (request == 0x0001) ? "CLEAR_FEATURE" :
           (request == 0x0003) ? "SET_FEATURE" :
           (request == 0x8000) ? "GET_STATUS" :
           ((request >> 8) == 0x40 || (request >> 8) == 0xC0) ? "VENDOR" :
           "OTHER");
    fflush(stdout);

    int ret = usb_desc_handle_control(dev, p, request, value, index,
                                      length, data);
    if (ret >= 0) {
        int actual = p->actual_length;
        if(0) {
            if(0) printf("[%07lld] chihiro-usb [%s]: std handled actual=%d addr=%d", TS_MS, id, actual, dev->addr);
            if (actual > 0) {
                if(0) printf(" data=");
                if(0) for (int i = 0; i < actual && i < 18; i++) printf("%02X", data[i]);
            }
            if(0) printf("\n");
        }

        /* v302: After SET_ADDRESS completes, schedule EZ-USB firmware reboot.
         * Real AN2131 loads firmware from EEPROM, then disconnects+reconnects.
         * SEGABOOT waits for the CSC from reconnect to start Phase 2. */
        if (request == (DeviceOutRequest | USB_REQ_SET_ADDRESS) && !s->ezusb_rebooted) {
            s->ezusb_rebooted = true;
            if(0) printf("[%07lld] chihiro-usb [%s]: v302 SET_ADDRESS done (addr=%d) → scheduling EZ-USB reboot in 100ms\n",
                   TS_MS, id, dev->addr);
            timer_mod(s->ezusb_disconnect_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
        }

        return;
    }

    /* v202: Log when standard handler rejects a request */
    {
        uint8_t bmRequestType = request >> 8;
        uint8_t reqType = (bmRequestType >> 5) & 0x03;  /* 0=std, 1=class, 2=vendor */
        if (reqType != 2) {
            /* Non-vendor request rejected by usb_desc — could indicate descriptor issue */
            if(0) printf("[%07lld] chihiro-usb [%s]: ⚠ std handler REJECTED req=0x%04X (ret=%d) — "
                   "falling through to vendor handler\n", TS_MS, id, request, ret);
        }
    }

    /* Vendor request — extract bRequest from combined field.
     * QEMU encodes: request = (bmRequestType << 8) | bRequest */
    int bRequest = request & 0xFF;
    vendor_req_counts[(uint8_t)bRequest]++;

    if (0) { /* JVS vendor request logging — enable for debugging */
        if (bRequest >= 0x15 && bRequest <= 0x20) {
            printf("[%07lld] chihiro-usb [%s]: VENDOR 0x%02X val=0x%04X idx=0x%04X len=%d",
                   TS_MS, id, bRequest, value, index, length);
            if (!((request >> 8) & 0x80) && length > 0) {
                printf(" OUT_DATA=");
                for (int i = 0; i < length && i < 32; i++) printf("%02X", data[i]);
            }
            printf("\n");
        }
    }

    /* Save original host data for OUT (host-to-device) vendor requests.
     * The default fill below overwrites data[], so we need a copy for
     * requests like 0x20/0x23 that carry JVS payload. */
    uint8_t host_data[256];
    int host_len = MIN(length, (int)sizeof(host_data));
    if (!((request >> 8) & 0x80)) {
        memcpy(host_data, data, host_len);
    }

    /* Default response (MAME: every vendor request gets this) */
    for (int n = 0; n < length && n < 6; n++) {
        data[n] = 0x50 ^ n;
    }
    data[0] = 0x00;  /* success */
    data[1] = 0xCB;  /* PINSA (active low: 0=pressed/ON, 1=released/OFF)
                      * bit0=1 DIP1 OFF
                      * bit1=1 DIP2 OFF
                      * bit2=0 DIP3 ON  (horiz freq — required to avoid CAUTION 51)
                      * bit3=1 CS pin (ignored)
                      * bit4=0 DIP4 ON  (horiz freq — required to avoid CAUTION 51)
                      * bit5=0 DIP5 ON
                      * bit6=1 TEST released
                      * bit7=1 SERVICE released */
    data[2] = 0x52 | (s->jvs.sense & 0x03);  /* PINSB with current JVS sense */
    data[3] = 0x53;  /* OUTB register */

    switch (bRequest) {
    case 0x16: /* Read ic10 EEPROM #1 — queue for bulk EP1 IN */
    {
        int addr = value;     /* wValue = start address in ic10 */
        int count = index;    /* wIndex = byte count */
        if (count > CHIHIRO_USB_EP_BUFSZ) count = CHIHIRO_USB_EP_BUFSZ;
        if (addr + count > 8192) count = 8192 - addr;
        if (addr >= 0 && count > 0) {
            memcpy(s->ep_in[1].buf, s->eeprom + addr, count);
        } else {
            memset(s->ep_in[1].buf, 0xFF, count);
        }
        s->ep_in[1].pending = count;
        s->ep_in[1].offset = 0;
        if(0) printf("[%07lld] chihiro-usb [%s]: READ EEPROM1 addr=0x%04X count=%d → queued for EP1\n", TS_MS,
               id, addr, count);
        break;
    }
    case 0x17: /* Read baseboard EEPROM ic11 (512 bytes, 24LC024) */
    {
        int addr = value;
        int count = index;
        if (count > CHIHIRO_USB_EP_BUFSZ) count = CHIHIRO_USB_EP_BUFSZ;
        if (addr + count > 512) count = 512 - addr;
        if (addr >= 0 && addr < 512 && count > 0) {
            memcpy(s->ep_in[2].buf, s->ic11 + addr, count);
        } else {
            memset(s->ep_in[2].buf, 0xFF, count);
            count = (count > 0) ? count : 0;
        }
        s->ep_in[2].pending = count;
        s->ep_in[2].offset = 0;
        if(0) {
            if(0) printf("[%07lld] chihiro-usb [%s]: READ ic11 EEPROM addr=0x%02X count=%d data=", TS_MS, id, addr, count);
            for (int i = 0; i < count && i < 16; i++) printf("%02X", s->ep_in[2].buf[i]);
            if(0) printf("\n");
        }
        break;
    }
    case 0x19: { /* Get JVS responses (QC path) */
        jvs_recv_count++;
        data[0] = 0x00;  /* not busy */
        /* Update sense in PINSB */
        data[2] = (data[2] & 0xFC) | (s->jvs.sense & 0x03);
        if (s->jvs.response_len > 0) {
            jvs_recv_has_data++;
            int rlen = s->jvs.response_len;
            /* Wrap raw JVS frame in AN2131QC format for EP4 IN:
             * [0]=0x00 [1]=pkt_count [2]=dest [3]=0x00
             * [4..5]=frame_len(LE) [6..]=raw JVS frame */
            uint8_t *ep = s->ep_in[4].buf;
            int wrapped = 0;
            uint8_t resp_dest = (s->jvs.last_target == JVS_BROADCAST) ? 0
                                                                     : s->jvs.device_id;
            ep[wrapped++] = 0x00;       /* status */
            ep[wrapped++] = 0x01;       /* 1 packet */
            ep[wrapped++] = resp_dest;  /* dest: 0 for broadcast, device_id for addressed */
            ep[wrapped++] = 0x00;       /* dummy */
            ep[wrapped++] = rlen & 0xFF;
            ep[wrapped++] = (rlen >> 8) & 0xFF;
            int copy = MIN(rlen, (int)sizeof(s->ep_in[4].buf) - wrapped);
            memcpy(ep + wrapped, s->jvs.response, copy);
            wrapped += copy;
            data[4] = wrapped & 0xFF;
            data[5] = (wrapped >> 8) & 0xFF;
            s->ep_in[4].pending = wrapped;
            s->ep_in[4].offset = 0;
            s->jvs.response_len = 0;
            if(0) printf("[%07lld] chihiro-usb [%s]: JVS RECV wrapped %d bytes (raw %d, dest=%d)\n",
                   TS_MS, id, wrapped, rlen, resp_dest);
        } else {
            data[4] = 0;
            data[5] = 0;
        }
        break;
    }
    case 0x20: { /* Send JVS packets (QC path) */
        jvs_send_count++;
        if(0) { printf("[%07lld] chihiro-usb [%s]: JVS SEND %d bytes:", TS_MS, id, host_len);
        for (int i = 0; i < host_len && i < 16; i++) printf(" %02X", host_data[i]);
        printf("\n"); }
        /* AN2131QC format: byte 0 = sequence counter, bytes 1+ = JVS frame */
        uint8_t *jvs_data = host_data;
        int jvs_len = host_len;
        if (jvs_len >= 2 && host_data[0] != JVS_SYNC && host_data[1] == JVS_SYNC) {
            jvs_data = host_data + 1;
            jvs_len -= 1;
        }
        if (jvs_len > 0 && jvs_data[0] == JVS_SYNC) {
            int rlen = chihiro_jvs_process(&s->jvs, jvs_data, jvs_len,
                                            s->jvs.response, sizeof(s->jvs.response));
            s->jvs.response_len = rlen;
            if(0) printf("[%07lld] chihiro-usb [%s]: JVS RESP %d bytes\n", TS_MS, id, rlen);
        }
        break;
    }
    case 0x30: /* External interrupt control */
        data[4] = (value & 0xFF) > 0 ? 1 : 0;  /* enabled? */
        data[5] = 0;  /* IRQ counter */
        if(0) printf("[%07lld] chihiro-usb [%s]: EXT IRQ control val=%d\n", TS_MS, id, value);
        break;
    case 0x1C: /* Read RTC — queue BCD time for bulk IN EP4 (data[0]=0 success status) */
    {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        #define TO_BCD(v) ((uint8_t)((v) + 6 * ((v) / 10)))
        int rtc_count = index;
        if (rtc_count > CHIHIRO_USB_EP_BUFSZ) rtc_count = CHIHIRO_USB_EP_BUFSZ;
        memset(s->ep_in[5].buf, 0, rtc_count);
        s->ep_in[5].buf[0] = TO_BCD(t->tm_sec);
        s->ep_in[5].buf[1] = TO_BCD(t->tm_min);
        s->ep_in[5].buf[2] = TO_BCD(t->tm_hour);
        s->ep_in[5].buf[3] = 0;
        s->ep_in[5].buf[4] = TO_BCD(t->tm_mday);
        s->ep_in[5].buf[5] = TO_BCD(t->tm_mon + 1);
        s->ep_in[5].buf[6] = TO_BCD(t->tm_year - 100);
        s->ep_in[5].buf[7] = 0;
        s->ep_in[5].pending = rtc_count;
        s->ep_in[5].offset = 0;
        #undef TO_BCD
        if(0) printf("[%07lld] chihiro-usb [%s]: RTC READ → %02X:%02X:%02X %02X/%02X/%02X (%d bytes queued EP5)\n",
               TS_MS, id, s->ep_in[5].buf[2], s->ep_in[5].buf[1], s->ep_in[5].buf[0],
               s->ep_in[5].buf[4], s->ep_in[5].buf[5], s->ep_in[5].buf[6], rtc_count);
        break;
    }
    case 0x1D: /* Write ic10 EEPROM #1 — accept */
        break;
    case 0x1E: /* Write ic11 EEPROM #2 via EP2 OUT */
        s->write_1e_addr = value;
        break;
    case 0x1F: /* Write external memory via EP3 OUT */
        s->write_1f_addr = value;
        break;
    case 0x24: /* Write RTC — accept */
        break;
    case 0x18: /* Read external memory / write-complete status poll */
    {
        int count = index;
        if (count == 0) {
            /* Status poll — default fill already has data[0]=0 (not busy) */
            break;
        }
        if (count > CHIHIRO_USB_EP_BUFSZ) count = CHIHIRO_USB_EP_BUFSZ;
        int addr = value;
        if (addr + count > 65536) count = 65536 - addr;
        if (addr >= 0 && count > 0) {
            memcpy(s->ep_in[3].buf, s->extmem + addr, count);
        } else {
            memset(s->ep_in[3].buf, 0, count > 0 ? count : 0);
        }
        s->ep_in[3].pending = count;
        s->ep_in[3].offset = 0;
        break;
    }
    case 0xA0: /* ANCHOR_LOAD — EZ-USB firmware download (Cypress AN2131) */
    {
        uint16_t ram_addr = value;  /* wValue = target address in 8051 RAM */
        int count = length;
        if (ram_addr == 0x7F92) {
            /* CPUCS register: bit 0 = 1 → hold CPU in reset, 0 → run */
            bool hold = (count > 0 && data[0] & 0x01);
            if(0) printf("[%07lld] chihiro-usb [%s]: ANCHOR_LOAD CPUCS=%s (total %u bytes downloaded)\n",
                   TS_MS, id, hold ? "HOLD" : "RUN", s->fw_bytes_written);
            if (s->fw_cpu_held && !hold) {
                if(0) printf("[%07lld] chihiro-usb [%s]: ★ EZ-USB firmware loaded — CPU released\n",
                       TS_MS, id);
            }
            s->fw_cpu_held = hold;
        } else {
            s->fw_bytes_written += count;
            if (s->fw_bytes_written <= count) {
                if(0) printf("[%07lld] chihiro-usb [%s]: ANCHOR_LOAD start addr=0x%04X len=%d\n",
                       TS_MS, id, ram_addr, count);
            }
        }
        break;
    }
    /* === SC-specific handlers (UART / JVS) === */
    case 0x1A: /* Get UART0 data (SC only) */
    {
        int avail = s->uart0_rx_len;
        if (avail > 0 && avail <= length) {
            memcpy(data, s->uart0_rx, avail);
            p->actual_length = avail;
            s->uart0_rx_len = 0;
        } else {
            p->actual_length = 0;
        }
        if(0) printf("[%07lld] chihiro-usb [%s]: GET UART0 → %d bytes\n", TS_MS, id, avail);
        return;
    }
    case 0x1B: /* Get UART1 / JVS response (SC only) */
    {
        int avail = s->jvs.response_len;
        if (avail > 0 && avail <= length) {
            memcpy(data, s->jvs.response, avail);
            p->actual_length = avail;
            s->jvs.response_len = 0;
        } else if (s->uart1_rx_len > 0 && s->uart1_rx_len <= length) {
            memcpy(data, s->uart1_rx, s->uart1_rx_len);
            p->actual_length = s->uart1_rx_len;
            s->uart1_rx_len = 0;
        } else {
            p->actual_length = 0;
        }
        return;
    }
    case 0x22: /* Send UART0 data (SC only) — accept and discard */
        if(0) printf("[%07lld] chihiro-usb [%s]: SEND UART0 len=%d (stub)\n", TS_MS, id, length);
        break;
    case 0x23: { /* Send UART1 / JVS command (SC only) */
        int jvs_len = host_len;
        if (jvs_len > 0 && host_data[0] == JVS_SYNC) {
            int rlen = chihiro_jvs_process(&s->jvs, host_data, jvs_len,
                                            s->jvs.response, sizeof(s->jvs.response));
            s->jvs.response_len = rlen;
        }
        break;
    }
    case 0x25: /* UART config (SC only) — accept */
    case 0x26:
    case 0x27:
    case 0x28:
    case 0x29:
    case 0x2A:
    case 0x2B:
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        if(0) printf("[%07lld] chihiro-usb [%s]: UART/GPIO config 0x%02X (stub)\n", TS_MS, id, bRequest);
        break;
    case 0x31: /* Set PORTB pins (SC only) — accept */
        if(0) printf("[%07lld] chihiro-usb [%s]: SET PORTB val=0x%04X (stub)\n", TS_MS, id, value);
        break;
    default:
        if(0) printf("[%07lld] chihiro-usb [%s]: UNHANDLED vendor req 0x%02X val=0x%04X idx=0x%04X len=%d → accepting\n",
               TS_MS, id, bRequest, value, index, length);
        break;
    }

    if (0) { /* Diagnostic: log vendor 0x15 (PINSB/sense check) */
        if (bRequest == 0x15) {
            printf("[%07lld] chihiro-usb [%s]: VENDOR 0x15 → data[0..3]=%02X %02X %02X %02X sense=%d\n",
                   TS_MS, id, data[0], data[1], data[2], data[3], s->jvs.sense);
        }
    }

    p->actual_length = length;
}

static void handle_data(USBDevice *dev, USBPacket *p)
{
    extern uint64_t perf_cnt_usb_handle;
    perf_cnt_usb_handle++;
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    const char *id = ((ChihiroUSBState *)dev)->is_qc ? "QC" : "SC";
    int ep = p->ep->nr;

    if (p->pid == USB_TOKEN_IN) {
        /* Bulk IN — return queued data from per-endpoint buffer */
        if (ep >= 1 && ep < CHIHIRO_USB_MAX_EP && s->ep_in[ep].pending > 0) {
            int len = MIN(p->iov.size, s->ep_in[ep].pending);
            usb_packet_copy(p, s->ep_in[ep].buf + s->ep_in[ep].offset, len);
            s->ep_in[ep].offset += len;
            s->ep_in[ep].pending -= len;
            s->bulk_in_count++;
            if(0) printf("[%07lld] chihiro-usb [%s]: BULK IN EP%d → %d bytes (remain=%d)\n",
                   TS_MS, id, ep, len, s->ep_in[ep].pending);
        } else {
            s->nak_count++;
            p->status = USB_RET_STALL;
            if(0) printf("[%07lld] chihiro-usb [%s]: BULK IN EP%d → STALL (no data)\n",
                   TS_MS, id, ep);
        }
    } else {
        /* Bulk OUT — capture data for JVS processing */
        if(0) printf("[%07lld] chihiro-usb [%s]: BULK OUT EP%d %d bytes\n",
               TS_MS, id, ep, (int)p->iov.size);
        int len = p->iov.size;
        uint8_t buf[256];
        int total = 0;
        while (len > 0) {
            int chunk = MIN(len, (int)sizeof(buf) - total);
            if (chunk <= 0) {
                uint8_t discard[64];
                chunk = MIN(len, (int)sizeof(discard));
                usb_packet_copy(p, discard, chunk);
            } else {
                usb_packet_copy(p, buf + total, chunk);
                total += chunk;
            }
            len -= chunk;
        }
        if (total > 0) {
            if(0) { printf("[%07lld] chihiro-usb [%s]: BULK OUT data:", TS_MS, id);
            for (int i = 0; i < total && i < 32; i++) printf(" %02X", buf[i]);
            printf("\n"); }

            if (ep == 2) {
                /* EP2 OUT: ic11 EEPROM write (from vendor 0x1E) */
                uint16_t addr = s->write_1e_addr;
                int copy = MIN(total, 512 - (int)addr);
                if (copy > 0) {
                    memcpy(s->ic11 + addr, buf, copy);
                    s->write_1e_addr += copy;
                }
            } else if (ep == 3) {
                /* EP3 OUT: external memory write (from vendor 0x1F) */
                uint16_t addr = s->write_1f_addr;
                int copy = MIN(total, 65536 - (int)addr);
                if (copy > 0) {
                    memcpy(s->extmem + addr, buf, copy);
                    s->write_1f_addr += copy;
                }
            } else if (ep == 4) {
                /* EP4 OUT: JVS data with 3-byte AN2131QC header */
                uint8_t *jvs_p = NULL;
                int jvs_n = 0;
                if (total >= 4 && buf[3] == JVS_SYNC) {
                    jvs_p = buf + 3;
                    jvs_n = total - 3;
                } else if (total >= 1 && buf[0] == JVS_SYNC) {
                    jvs_p = buf;
                    jvs_n = total;
                }
                if (jvs_p && jvs_n > 0) {
                    if(0) { printf("[%07lld] chihiro-usb [%s]: JVS frame %d bytes:", TS_MS, id, jvs_n);
                    for (int i = 0; i < jvs_n && i < 16; i++) printf(" %02X", jvs_p[i]);
                    printf("\n"); }
                    int rlen = chihiro_jvs_process(&s->jvs, jvs_p, jvs_n,
                                                    s->jvs.response, sizeof(s->jvs.response));
                    s->jvs.response_len = rlen;
                    if(0) printf("[%07lld] chihiro-usb [%s]: JVS response %d bytes\n", TS_MS, id, rlen);
                }
            }
        }
        s->bulk_out_count++;
    }
}

/* v202: Counter accessors for DIAG timer in chihiro.c */
void chihiro_usb_get_counters(USBDevice *dev, uint32_t *nak, uint32_t *bulk_in, uint32_t *bulk_out)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    if (nak)      *nak      = s->nak_count;
    if (bulk_in)  *bulk_in  = s->bulk_in_count;
    if (bulk_out) *bulk_out = s->bulk_out_count;
}

void chihiro_usb_reset_counters(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    s->nak_count = 0;
    s->bulk_in_count = 0;
    s->bulk_out_count = 0;
}

void chihiro_usb_get_jvs_counters(uint64_t *send, uint64_t *recv, uint64_t *recv_data)
{
    if (send)      *send      = jvs_send_count;
    if (recv)      *recv      = jvs_recv_count;
    if (recv_data) *recv_data = jvs_recv_has_data;
}

void chihiro_usb_dump_vendor_histogram(void)
{
    printf("=== VENDOR REQUEST HISTOGRAM ===\n");
    for (int i = 0; i < 256; i++) {
        if (vendor_req_counts[i] > 0) {
            printf("  req 0x%02X: %lu\n", i, (unsigned long)vendor_req_counts[i]);
        }
    }
    printf("================================\n");
}

static void chihiro_an2131qc_realize(USBDevice *dev, Error **errp)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    s->is_qc = true;
    usb_desc_init(dev);
    dev->auto_attach = 0;  /* Attach later via hotplug timer */

    /* Load real ic10 QC EEPROM firmware (8192 bytes from MAME hotd3.zip).
     * Contains AN2131 8051 firmware code + region/serial/game data.
     * SEGABOOT reads this via vendor request 0x16 + bulk IN EP1. */
    _Static_assert(sizeof(hotd3_ic10_g24lc64) == 8192,
                   "ic10 firmware must be exactly 8KB");
    memcpy(s->eeprom, hotd3_ic10_g24lc64, sizeof(s->eeprom));

    /* Override region to USA (0x02) to match game's bootid regionFlags.
     * HOD3 bootid has regionFlags=0xFFFFFF0E (US+EXP, no JP).
     * Original ic10 EEPROM has 0x01 (Japan) which would cause ERROR 31. */
    s->eeprom[0x1F00] = 0x02;  /* Region: 01=JPN, 02=USA, 03=EXP */

    /* Initialize per-endpoint bulk transfer state */
    memset(s->ep_in, 0, sizeof(s->ep_in));

    /* Load ic11 baseboard EEPROM (256 bytes, 24LC024) — first 128 from dump, rest zero */
    _Static_assert(sizeof(hotd3_ic11_24lc024) == 128,
                   "ic11 EEPROM dump must be exactly 128 bytes");
    memset(s->ic11, 0, sizeof(s->ic11));
    memcpy(s->ic11, hotd3_ic11_24lc024, sizeof(hotd3_ic11_24lc024));
    memset(s->extmem, 0, sizeof(s->extmem));
    s->write_1e_addr = 0;
    s->write_1f_addr = 0;

    /* Initialize EZ-USB firmware state */
    s->fw_bytes_written = 0;
    s->fw_cpu_held = false;

    /* v302: EZ-USB firmware reboot timers */
    s->ezusb_disconnect_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ezusb_disconnect_cb, s);
    s->ezusb_reconnect_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ezusb_reconnect_cb, s);
    s->ezusb_rebooted = true;  /* v307: reconnect disabled — Path B fix makes it unnecessary */

    chihiro_jvs_init(&s->jvs);
    chihiro_jvs_global = &s->jvs;

    printf("[%07lld] Chihiro QC: loaded ic10 firmware (8192B) + ic11 (128B), "
           "region patched to USA (0x02), serial=%.16s\n",
           TS_MS, (const char *)&s->eeprom[0x1F10]);
}

static void chihiro_an2131qc_unrealize(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    timer_free(s->ezusb_disconnect_timer);
    timer_free(s->ezusb_reconnect_timer);
}

static void chihiro_an2131qc_class_init(ObjectClass *klass, const void *data)
{
    // DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = chihiro_an2131qc_realize;
    uc->unrealize      = chihiro_an2131qc_unrealize;
    uc->product_desc   = "Chihiro an2131qc";
    uc->usb_desc       = &desc_chihiro_an2131qc;

    uc->handle_reset   = handle_reset;
    uc->handle_control = handle_control;
    uc->handle_data    = handle_data;
    uc->handle_attach  = usb_desc_attach;

    //dc->vmsd = &vmstate_usb_kbd;
}

static const TypeInfo chihiro_an2131qc_info = {
    .name          = "chihiro-an2131qc",
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(ChihiroUSBState),
    .class_init    = chihiro_an2131qc_class_init,
};

static void chihiro_an2131sc_realize(USBDevice *dev, Error **errp)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    s->is_qc = false;
    usb_desc_init(dev);
    dev->auto_attach = 0;  /* Attach later via hotplug timer */

    /* Load real pc20 SC EEPROM firmware (8192 bytes from MAME hotd3.zip). */
    _Static_assert(sizeof(hotd3_pc20_g24lc64) == 8192,
                   "pc20 firmware must be exactly 8KB");
    memcpy(s->eeprom, hotd3_pc20_g24lc64, sizeof(s->eeprom));

    /* Initialize per-endpoint bulk transfer state */
    memset(s->ep_in, 0, sizeof(s->ep_in));

    /* Load ic11 baseboard EEPROM (256 bytes, 24LC024) */
    memset(s->ic11, 0, sizeof(s->ic11));
    memcpy(s->ic11, hotd3_ic11_24lc024, sizeof(hotd3_ic11_24lc024));
    memset(s->extmem, 0, sizeof(s->extmem));
    s->write_1e_addr = 0;
    s->write_1f_addr = 0;

    /* Initialize EZ-USB firmware state */
    s->fw_bytes_written = 0;
    s->fw_cpu_held = false;

    /* Initialize UART buffers */
    s->uart0_rx_len = 0;
    s->uart1_rx_len = 0;

    /* v302: EZ-USB firmware reboot timers */
    s->ezusb_disconnect_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ezusb_disconnect_cb, s);
    s->ezusb_reconnect_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ezusb_reconnect_cb, s);
    s->ezusb_rebooted = true;  /* v307: reconnect disabled — Path B fix makes it unnecessary */

    chihiro_jvs_init(&s->jvs);

    printf("[%07lld] Chihiro SC: loaded pc20 firmware (8192B) + ic11 (128B)\n", TS_MS);
}

static void chihiro_an2131sc_unrealize(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    timer_free(s->ezusb_disconnect_timer);
    timer_free(s->ezusb_reconnect_timer);
}

static void chihiro_an2131sc_class_init(ObjectClass *klass, const void *data)
{
    // DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = chihiro_an2131sc_realize;
    uc->unrealize      = chihiro_an2131sc_unrealize;
    uc->product_desc   = "Chihiro an2131sc";
    uc->usb_desc       = &desc_chihiro_an2131sc;

    uc->handle_reset   = handle_reset;
    uc->handle_control = handle_control;
    uc->handle_data    = handle_data;
    uc->handle_attach  = usb_desc_attach;

    //dc->vmsd = &vmstate_usb_kbd;
}

static const TypeInfo chihiro_an2131sc_info = {
    .name          = "chihiro-an2131sc",
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(ChihiroUSBState),
    .class_init    = chihiro_an2131sc_class_init,
};

static void chihiro_usb_register_types(void)
{
    type_register_static(&chihiro_an2131qc_info);
    type_register_static(&chihiro_an2131sc_info);
}

type_init(chihiro_usb_register_types)
