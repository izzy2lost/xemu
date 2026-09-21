/*
 * QEMU USB OHCI Emulation
 * Copyright (c) 2004 Gianni Tedesco
 * Copyright (c) 2006 CodeSourcery
 * Copyright (c) 2006 Openedhand Ltd.
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
 *
 * TODO:
 *  o Isochronous transfers
 *  o Allocate bandwidth in frames properly
 *  o Disable timers when nothing needs to be done, or remove timer usage
 *    all together.
 *  o BIOS work to boot from USB storage
 */

#include "qemu/osdep.h"
#include "hw/irq.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#define TS_MS ((long long)(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)))
#include "hw/usb.h"
#include "migration/vmstate.h"
#include "hw/sysbus.h"
#include "hw/qdev-dma.h"
#include "hw/qdev-properties.h"
#include "trace.h"
#include "hcd-ohci.h"
#include "hw/xbox/chihiro.h"

/* This causes frames to occur 1000x slower */
/*#define OHCI_TIME_WARP 1*/

#define ED_LINK_LIMIT 32

static int64_t usb_frame_time;
static int64_t usb_bit_time;

/* Host Controller Communications Area */
struct ohci_hcca {
    uint32_t intr[32];
    uint16_t frame, pad;
    uint32_t done;
};
#define HCCA_WRITEBACK_OFFSET   offsetof(struct ohci_hcca, frame)
#define HCCA_WRITEBACK_SIZE     8 /* frame, pad, done */

#define ED_WBACK_OFFSET offsetof(struct ohci_ed, head)
#define ED_WBACK_SIZE   4

/* Bitfields for the first word of an Endpoint Descriptor. */
#define OHCI_ED_FA_SHIFT  0
#define OHCI_ED_FA_MASK   (0x7f << OHCI_ED_FA_SHIFT)
#define OHCI_ED_EN_SHIFT  7
#define OHCI_ED_EN_MASK   (0xf << OHCI_ED_EN_SHIFT)
#define OHCI_ED_D_SHIFT   11
#define OHCI_ED_D_MASK    (3 << OHCI_ED_D_SHIFT)
#define OHCI_ED_S         (1 << 13)
#define OHCI_ED_K         (1 << 14)
#define OHCI_ED_F         (1 << 15)
#define OHCI_ED_MPS_SHIFT 16
#define OHCI_ED_MPS_MASK  (0x7ff << OHCI_ED_MPS_SHIFT)

/* Flags in the head field of an Endpoint Descriptor. */
#define OHCI_ED_H         1
#define OHCI_ED_C         2

/* Bitfields for the first word of a Transfer Descriptor. */
#define OHCI_TD_R         (1 << 18)
#define OHCI_TD_DP_SHIFT  19
#define OHCI_TD_DP_MASK   (3 << OHCI_TD_DP_SHIFT)
#define OHCI_TD_DI_SHIFT  21
#define OHCI_TD_DI_MASK   (7 << OHCI_TD_DI_SHIFT)
#define OHCI_TD_T0        (1 << 24)
#define OHCI_TD_T1        (1 << 25)
#define OHCI_TD_EC_SHIFT  26
#define OHCI_TD_EC_MASK   (3 << OHCI_TD_EC_SHIFT)
#define OHCI_TD_CC_SHIFT  28
#define OHCI_TD_CC_MASK   (0xf << OHCI_TD_CC_SHIFT)

/* Bitfields for the first word of an Isochronous Transfer Descriptor. */
/* CC & DI - same as in the General Transfer Descriptor */
#define OHCI_TD_SF_SHIFT  0
#define OHCI_TD_SF_MASK   (0xffff << OHCI_TD_SF_SHIFT)
#define OHCI_TD_FC_SHIFT  24
#define OHCI_TD_FC_MASK   (7 << OHCI_TD_FC_SHIFT)

/* Isochronous Transfer Descriptor - Offset / PacketStatusWord */
#define OHCI_TD_PSW_CC_SHIFT 12
#define OHCI_TD_PSW_CC_MASK  (0xf << OHCI_TD_PSW_CC_SHIFT)
#define OHCI_TD_PSW_SIZE_SHIFT 0
#define OHCI_TD_PSW_SIZE_MASK  (0xfff << OHCI_TD_PSW_SIZE_SHIFT)

#define OHCI_PAGE_MASK    0xfffff000
#define OHCI_OFFSET_MASK  0xfff

#define OHCI_DPTR_MASK    0xfffffff0

#define OHCI_BM(val, field) \
  (((val) & OHCI_##field##_MASK) >> OHCI_##field##_SHIFT)

#define OHCI_SET_BM(val, field, newval) do { \
    val &= ~OHCI_##field##_MASK; \
    val |= ((newval) << OHCI_##field##_SHIFT) & OHCI_##field##_MASK; \
    } while (0)

/* endpoint descriptor */
struct ohci_ed {
    uint32_t flags;
    uint32_t tail;
    uint32_t head;
    uint32_t next;
};

/* General transfer descriptor */
struct ohci_td {
    uint32_t flags;
    uint32_t cbp;
    uint32_t next;
    uint32_t be;
};

/* Isochronous transfer descriptor */
struct ohci_iso_td {
    uint32_t flags;
    uint32_t bp;
    uint32_t next;
    uint32_t be;
    uint16_t offset[8];
};

#define USB_HZ                      12000000

/* OHCI Local stuff */
#define OHCI_CTL_CBSR         ((1 << 0) | (1 << 1))
#define OHCI_CTL_PLE          (1 << 2)
#define OHCI_CTL_IE           (1 << 3)
#define OHCI_CTL_CLE          (1 << 4)
#define OHCI_CTL_BLE          (1 << 5)
#define OHCI_CTL_HCFS         ((1 << 6) | (1 << 7))
#define  OHCI_USB_RESET       0x00
#define  OHCI_USB_RESUME      0x40
#define  OHCI_USB_OPERATIONAL 0x80
#define  OHCI_USB_SUSPEND     0xc0
#define OHCI_CTL_IR           (1 << 8)
#define OHCI_CTL_RWC          (1 << 9)
#define OHCI_CTL_RWE          (1 << 10)

#define OHCI_STATUS_HCR       (1 << 0)
#define OHCI_STATUS_CLF       (1 << 1)
#define OHCI_STATUS_BLF       (1 << 2)
#define OHCI_STATUS_OCR       (1 << 3)
#define OHCI_STATUS_SOC       ((1 << 6) | (1 << 7))

#define OHCI_INTR_SO          (1U << 0) /* Scheduling overrun */
#define OHCI_INTR_WD          (1U << 1) /* HcDoneHead writeback */
#define OHCI_INTR_SF          (1U << 2) /* Start of frame */
#define OHCI_INTR_RD          (1U << 3) /* Resume detect */
#define OHCI_INTR_UE          (1U << 4) /* Unrecoverable error */
#define OHCI_INTR_FNO         (1U << 5) /* Frame number overflow */
#define OHCI_INTR_RHSC        (1U << 6) /* Root hub status change */
#define OHCI_INTR_OC          (1U << 30) /* Ownership change */
#define OHCI_INTR_MIE         (1U << 31) /* Master Interrupt Enable */

#define OHCI_HCCA_SIZE        0x100
#define OHCI_HCCA_MASK        0xffffff00

#define OHCI_EDPTR_MASK       0xfffffff0

#define OHCI_FMI_FI           0x00003fff
#define OHCI_FMI_FSMPS        0xffff0000
#define OHCI_FMI_FIT          0x80000000

#define OHCI_FR_RT            (1U << 31)

#define OHCI_LS_THRESH        0x628

#define OHCI_RHA_RW_MASK      0x00000000 /* Mask of supported features.  */
#define OHCI_RHA_PSM          (1 << 8)
#define OHCI_RHA_NPS          (1 << 9)
#define OHCI_RHA_DT           (1 << 10)
#define OHCI_RHA_OCPM         (1 << 11)
#define OHCI_RHA_NOCP         (1 << 12)
#define OHCI_RHA_POTPGT_MASK  0xff000000

#define OHCI_RHS_LPS          (1U << 0)
#define OHCI_RHS_OCI          (1U << 1)
#define OHCI_RHS_DRWE         (1U << 15)
#define OHCI_RHS_LPSC         (1U << 16)
#define OHCI_RHS_OCIC         (1U << 17)
#define OHCI_RHS_CRWE         (1U << 31)

#define OHCI_PORT_CCS         (1 << 0)
#define OHCI_PORT_PES         (1 << 1)
#define OHCI_PORT_PSS         (1 << 2)
#define OHCI_PORT_POCI        (1 << 3)
#define OHCI_PORT_PRS         (1 << 4)
#define OHCI_PORT_PPS         (1 << 8)
#define OHCI_PORT_LSDA        (1 << 9)
#define OHCI_PORT_CSC         (1 << 16)
#define OHCI_PORT_PESC        (1 << 17)
#define OHCI_PORT_PSSC        (1 << 18)
#define OHCI_PORT_OCIC        (1 << 19)
#define OHCI_PORT_PRSC        (1 << 20)
#define OHCI_PORT_WTC         (OHCI_PORT_CSC | OHCI_PORT_PESC | \
                               OHCI_PORT_PSSC | OHCI_PORT_OCIC | \
                               OHCI_PORT_PRSC)
#define OHCI_TD_DIR_SETUP     0x0
#define OHCI_TD_DIR_OUT       0x1
#define OHCI_TD_DIR_IN        0x2
#define OHCI_TD_DIR_RESERVED  0x3

#define OHCI_CC_NOERROR             0x0
#define OHCI_CC_CRC                 0x1
#define OHCI_CC_BITSTUFFING         0x2
#define OHCI_CC_DATATOGGLEMISMATCH  0x3
#define OHCI_CC_STALL               0x4
#define OHCI_CC_DEVICENOTRESPONDING 0x5
#define OHCI_CC_PIDCHECKFAILURE     0x6
#define OHCI_CC_UNDEXPETEDPID       0x7
#define OHCI_CC_DATAOVERRUN         0x8
#define OHCI_CC_DATAUNDERRUN        0x9
#define OHCI_CC_BUFFEROVERRUN       0xc
#define OHCI_CC_BUFFERUNDERRUN      0xd

#define OHCI_HRESET_FSBIR       (1 << 0)

static const char *ohci_reg_names[] = {
    "HcRevision", "HcControl", "HcCommandStatus", "HcInterruptStatus",
    "HcInterruptEnable", "HcInterruptDisable", "HcHCCA", "HcPeriodCurrentED",
    "HcControlHeadED", "HcControlCurrentED", "HcBulkHeadED", "HcBulkCurrentED",
    "HcDoneHead", "HcFmInterval", "HcFmRemaining", "HcFmNumber",
    "HcPeriodicStart", "HcLSThreshold", "HcRhDescriptorA", "HcRhDescriptorB",
    "HcRhStatus"
};

static const char *ohci_reg_name(hwaddr addr)
{
    if (addr >= 0x54) {
        static char buf[32];
        snprintf(buf, sizeof(buf), "HcRhPort%d", (int)((addr - 0x54) >> 2));
        return buf;
    }
    if (addr >> 2 < ARRAY_SIZE(ohci_reg_names)) {
        return ohci_reg_names[addr >> 2];
    }
    return "<unknown>";
}

static void ohci_die(OHCIState *ohci)
{
    ohci->ohci_die(ohci);
}

/* Update IRQ levels */
static inline void ohci_intr_update(OHCIState *ohci)
{
    int level = 0;

    if ((ohci->intr & OHCI_INTR_MIE) &&
        (ohci->intr_status & ohci->intr))
        level = 1;

    qemu_set_irq(ohci->irq, level);
}

/* Set an interrupt */
static inline void ohci_set_interrupt(OHCIState *ohci, uint32_t intr)
{
    ohci->intr_status |= intr;
    if ((intr & ~OHCI_INTR_SF) && TS_MS > 5900) {
        if(0) printf("[%07lld] OHCI IRQ: set 0x%08X (status=0x%08X enable=0x%08X)\n", TS_MS,
               intr, ohci->intr_status, ohci->intr);
        if (intr & OHCI_INTR_WD) {
            if(0) printf("[%07lld]   WDH: done=0x%08X hcca=0x%08X\n",
                   TS_MS, ohci->done, ohci->hcca);
        }
    }
    ohci_intr_update(ohci);
}

static USBDevice *ohci_find_device(OHCIState *ohci, uint8_t addr)
{
    USBDevice *dev;
    USBDevice *fallback = NULL;
    int i;

    for (i = 0; i < ohci->num_ports; i++) {
        dev = usb_find_device(&ohci->rhport[i].port, addr);
        if (dev != NULL) {
            int pes = (ohci->rhport[i].ctrl & OHCI_PORT_PES) ? 1 : 0;
            if (TS_MS > 5900)
                if(0) printf("[%07lld] OHCI ROUTE: addr=%d -> port%d PES=%d %s\n", TS_MS,
                       addr, i, pes, pes ? "MATCH" : "fallback");
            if (pes) return dev;
            /*
             * Chihiro only: the LLE baseboard enumeration reaches devices
             * on a port whose PES bit is still clear. Retail Xbox keeps the
             * spec behaviour, where a disabled port does not answer.
             */
            if (!fallback && xbox_is_chihiro()) fallback = dev;
        }
    }
    if (!fallback && TS_MS > 5900)
        if(0) printf("[%07lld] OHCI ROUTE: addr=%d -> NOT FOUND\n", TS_MS, addr);
    return fallback;
}

void ohci_stop_endpoints(OHCIState *ohci)
{
    USBDevice *dev;
    int i, j;

    if (ohci->async_td) {
        usb_cancel_packet(&ohci->usb_packet);
        ohci->async_td = 0;
    }
    for (i = 0; i < ohci->num_ports; i++) {
        dev = ohci->rhport[i].port.dev;
        if (dev && dev->attached) {
            usb_device_ep_stopped(dev, &dev->ep_ctl);
            for (j = 0; j < USB_MAX_ENDPOINTS; j++) {
                usb_device_ep_stopped(dev, &dev->ep_in[j]);
                usb_device_ep_stopped(dev, &dev->ep_out[j]);
            }
        }
    }
}

static void ohci_roothub_reset(OHCIState *ohci)
{
    OHCIPort *port;
    int i;

    ohci_bus_stop(ohci);
    ohci->rhdesc_a = OHCI_RHA_NPS | ohci->num_ports;
    ohci->rhdesc_b = 0x0; /* Impl. specific */
    ohci->rhstatus = 0;

    for (i = 0; i < ohci->num_ports; i++) {
        port = &ohci->rhport[i];
        port->ctrl = 0;
        if (port->port.dev && port->port.dev->attached) {
            usb_port_reset(&port->port);
        }
    }
    ohci_stop_endpoints(ohci);
}

/* Reset the controller */
static void ohci_soft_reset(OHCIState *ohci)
{
    trace_usb_ohci_reset(ohci->name);
    if (TS_MS > 5900) {
        if(0) printf("[%07lld] OHCI SOFT RESET: ctl=0x%08X ports:", TS_MS, ohci->ctl);
        for (int i = 0; i < ohci->num_ports; i++) {
            if(0) printf(" P%d=0x%08X", i, ohci->rhport[i].ctrl);
        }
        if(0) printf("\n");
    }

    ohci_bus_stop(ohci);
    ohci->ctl = (ohci->ctl & OHCI_CTL_IR) | OHCI_USB_SUSPEND;
    ohci->old_ctl = 0;
    ohci->status = 0;
    ohci->intr_status = 0;
    ohci->intr = OHCI_INTR_MIE;

    ohci->hcca = 0;
    ohci->ctrl_head = ohci->ctrl_cur = 0;
    ohci->bulk_head = ohci->bulk_cur = 0;
    ohci->per_cur = 0;
    ohci->done = 0;
    ohci->done_count = 7;
    /*
     * FSMPS is marked TBD in OCHI 1.0, what gives ffs?
     * I took the value linux sets ...
     */
    ohci->fsmps = 0x2778;
    ohci->fi = 0x2edf;
    ohci->fit = 0;
    ohci->frt = 0;
    ohci->frame_number = 0;
    ohci->pstart = 0;
    ohci->lst = OHCI_LS_THRESH;
}

void ohci_hard_reset(OHCIState *ohci)
{
    ohci_soft_reset(ohci);
    ohci->ctl = 0;
    ohci_roothub_reset(ohci);
}

/* Get an array of dwords from main memory */
static inline int get_dwords(OHCIState *ohci,
                             dma_addr_t addr, uint32_t *buf, int num)
{
    int i;

    addr += ohci->localmem_base;

    for (i = 0; i < num; i++, buf++, addr += sizeof(*buf)) {
        if (dma_memory_read(ohci->as, addr,
                            buf, sizeof(*buf), MEMTXATTRS_UNSPECIFIED)) {
            return -1;
        }
        *buf = le32_to_cpu(*buf);
    }

    return 0;
}

/* Put an array of dwords in to main memory */
static inline int put_dwords(OHCIState *ohci,
                             dma_addr_t addr, uint32_t *buf, int num)
{
    int i;

    addr += ohci->localmem_base;

    for (i = 0; i < num; i++, buf++, addr += sizeof(*buf)) {
        uint32_t tmp = cpu_to_le32(*buf);
        if (dma_memory_write(ohci->as, addr,
                             &tmp, sizeof(tmp), MEMTXATTRS_UNSPECIFIED)) {
            return -1;
        }
    }

    return 0;
}

/* Get an array of words from main memory */
static inline int get_words(OHCIState *ohci,
                            dma_addr_t addr, uint16_t *buf, int num)
{
    int i;

    addr += ohci->localmem_base;

    for (i = 0; i < num; i++, buf++, addr += sizeof(*buf)) {
        if (dma_memory_read(ohci->as, addr,
                            buf, sizeof(*buf), MEMTXATTRS_UNSPECIFIED)) {
            return -1;
        }
        *buf = le16_to_cpu(*buf);
    }

    return 0;
}

/* Put an array of words in to main memory */
static inline int put_words(OHCIState *ohci,
                            dma_addr_t addr, uint16_t *buf, int num)
{
    int i;

    addr += ohci->localmem_base;

    for (i = 0; i < num; i++, buf++, addr += sizeof(*buf)) {
        uint16_t tmp = cpu_to_le16(*buf);
        if (dma_memory_write(ohci->as, addr,
                             &tmp, sizeof(tmp), MEMTXATTRS_UNSPECIFIED)) {
            return -1;
        }
    }

    return 0;
}

static inline int ohci_read_ed(OHCIState *ohci,
                               dma_addr_t addr, struct ohci_ed *ed)
{
    return get_dwords(ohci, addr, (uint32_t *)ed, sizeof(*ed) >> 2);
}

static inline int ohci_read_td(OHCIState *ohci,
                               dma_addr_t addr, struct ohci_td *td)
{
    return get_dwords(ohci, addr, (uint32_t *)td, sizeof(*td) >> 2);
}

static inline int ohci_read_iso_td(OHCIState *ohci,
                                   dma_addr_t addr, struct ohci_iso_td *td)
{
    return get_dwords(ohci, addr, (uint32_t *)td, 4) ||
           get_words(ohci, addr + 16, td->offset, 8);
}

static inline int ohci_read_hcca(OHCIState *ohci,
                                 dma_addr_t addr, struct ohci_hcca *hcca)
{
    return dma_memory_read(ohci->as, addr + ohci->localmem_base, hcca,
                           sizeof(*hcca), MEMTXATTRS_UNSPECIFIED);
}

static inline int ohci_put_ed(OHCIState *ohci,
                              dma_addr_t addr, struct ohci_ed *ed)
{
    /*
     * ed->tail is under control of the HCD.
     * Since just ed->head is changed by HC, just write back this
     */
    return put_dwords(ohci, addr + ED_WBACK_OFFSET,
                      (uint32_t *)((char *)ed + ED_WBACK_OFFSET),
                      ED_WBACK_SIZE >> 2);
}

static inline int ohci_put_td(OHCIState *ohci,
                              dma_addr_t addr, struct ohci_td *td)
{
    return put_dwords(ohci, addr, (uint32_t *)td, sizeof(*td) >> 2);
}

static inline int ohci_put_iso_td(OHCIState *ohci,
                                  dma_addr_t addr, struct ohci_iso_td *td)
{
    return put_dwords(ohci, addr, (uint32_t *)td, 4) ||
           put_words(ohci, addr + 16, td->offset, 8);
}

static inline int ohci_put_hcca(OHCIState *ohci,
                                dma_addr_t addr, struct ohci_hcca *hcca)
{
    return dma_memory_write(ohci->as,
                            addr + ohci->localmem_base + HCCA_WRITEBACK_OFFSET,
                            (char *)hcca + HCCA_WRITEBACK_OFFSET,
                            HCCA_WRITEBACK_SIZE, MEMTXATTRS_UNSPECIFIED);
}

/* Read/Write the contents of a TD from/to main memory.  */
static int ohci_copy_td(OHCIState *ohci, struct ohci_td *td,
                        uint8_t *buf, int len, DMADirection dir)
{
    dma_addr_t ptr, n;

    ptr = td->cbp;
    n = 0x1000 - (ptr & 0xfff);
    if (n > len) {
        n = len;
    }
    if (dma_memory_rw(ohci->as, ptr + ohci->localmem_base, buf,
                      n, dir, MEMTXATTRS_UNSPECIFIED)) {
        return -1;
    }
    if (n == len) {
        return 0;
    }
    ptr = td->be & ~0xfffu;
    buf += n;
    if (dma_memory_rw(ohci->as, ptr + ohci->localmem_base, buf,
                      len - n, dir, MEMTXATTRS_UNSPECIFIED)) {
        return -1;
    }
    return 0;
}

/* Read/Write the contents of an ISO TD from/to main memory.  */
static int ohci_copy_iso_td(OHCIState *ohci,
                            uint32_t start_addr, uint32_t end_addr,
                            uint8_t *buf, int len, DMADirection dir)
{
    dma_addr_t ptr, n;

    ptr = start_addr;
    n = 0x1000 - (ptr & 0xfff);
    if (n > len) {
        n = len;
    }
    if (dma_memory_rw(ohci->as, ptr + ohci->localmem_base, buf,
                      n, dir, MEMTXATTRS_UNSPECIFIED)) {
        return -1;
    }
    if (n == len) {
        return 0;
    }
    ptr = end_addr & ~0xfffu;
    buf += n;
    if (dma_memory_rw(ohci->as, ptr + ohci->localmem_base, buf,
                      len - n, dir, MEMTXATTRS_UNSPECIFIED)) {
        return -1;
    }
    return 0;
}

#define USUB(a, b) ((int16_t)((uint16_t)(a) - (uint16_t)(b)))

static int ohci_service_iso_td(OHCIState *ohci, struct ohci_ed *ed)
{
    int dir;
    size_t len = 0;
    const char *str = NULL;
    int pid;
    int ret;
    int i;
    USBDevice *dev;
    USBEndpoint *ep;
    USBPacket *pkt;
    QEMU_UNINITIALIZED uint8_t buf[8192];
    bool int_req;
    struct ohci_iso_td iso_td;
    uint32_t addr;
    uint16_t starting_frame;
    int16_t relative_frame_number;
    int frame_count;
    uint32_t start_offset, next_offset, end_offset = 0;
    uint32_t start_addr, end_addr;

    addr = ed->head & OHCI_DPTR_MASK;

    if (addr == 0) {
        ohci_die(ohci);
        return 1;
    }

    if (ohci_read_iso_td(ohci, addr, &iso_td)) {
        trace_usb_ohci_iso_td_read_failed(addr);
        ohci_die(ohci);
        return 1;
    }

    starting_frame = OHCI_BM(iso_td.flags, TD_SF);
    frame_count = OHCI_BM(iso_td.flags, TD_FC);
    relative_frame_number = USUB(ohci->frame_number, starting_frame);

    trace_usb_ohci_iso_td_head(
           ed->head & OHCI_DPTR_MASK, ed->tail & OHCI_DPTR_MASK,
           iso_td.flags, iso_td.bp, iso_td.next, iso_td.be,
           ohci->frame_number, starting_frame,
           frame_count, relative_frame_number);
    trace_usb_ohci_iso_td_head_offset(
           iso_td.offset[0], iso_td.offset[1],
           iso_td.offset[2], iso_td.offset[3],
           iso_td.offset[4], iso_td.offset[5],
           iso_td.offset[6], iso_td.offset[7]);

    if (relative_frame_number < 0) {
        trace_usb_ohci_iso_td_relative_frame_number_neg(relative_frame_number);
        return 1;
    } else if (relative_frame_number > frame_count) {
        /*
         * ISO TD expired - retire the TD to the Done Queue and continue with
         * the next ISO TD of the same ED
         */
        trace_usb_ohci_iso_td_relative_frame_number_big(relative_frame_number,
                                                        frame_count);
        if (OHCI_CC_DATAOVERRUN == OHCI_BM(iso_td.flags, TD_CC)) {
            /* avoid infinite loop */
            return 1;
        }
        OHCI_SET_BM(iso_td.flags, TD_CC, OHCI_CC_DATAOVERRUN);
        ed->head &= ~OHCI_DPTR_MASK;
        ed->head |= (iso_td.next & OHCI_DPTR_MASK);
        iso_td.next = ohci->done;
        ohci->done = addr;
        i = OHCI_BM(iso_td.flags, TD_DI);
        if (i < ohci->done_count) {
            ohci->done_count = i;
        }
        if (ohci_put_iso_td(ohci, addr, &iso_td)) {
            ohci_die(ohci);
            return 1;
        }
        return 0;
    }

    dir = OHCI_BM(ed->flags, ED_D);
    switch (dir) {
    case OHCI_TD_DIR_IN:
        str = "in";
        pid = USB_TOKEN_IN;
        break;
    case OHCI_TD_DIR_OUT:
        str = "out";
        pid = USB_TOKEN_OUT;
        break;
    case OHCI_TD_DIR_SETUP:
        str = "setup";
        pid = USB_TOKEN_SETUP;
        break;
    default:
        trace_usb_ohci_iso_td_bad_direction(dir);
        return 1;
    }

    if (!iso_td.bp || !iso_td.be) {
        trace_usb_ohci_iso_td_bad_bp_be(iso_td.bp, iso_td.be);
        return 1;
    }

    start_offset = iso_td.offset[relative_frame_number];
    if (relative_frame_number < frame_count) {
        next_offset = iso_td.offset[relative_frame_number + 1];
    } else {
        next_offset = iso_td.be;
    }

    if (!(OHCI_BM(start_offset, TD_PSW_CC) & 0xe) ||
        ((relative_frame_number < frame_count) &&
         !(OHCI_BM(next_offset, TD_PSW_CC) & 0xe))) {
        trace_usb_ohci_iso_td_bad_cc_not_accessed(start_offset, next_offset);
        return 1;
    }

    if ((relative_frame_number < frame_count) && (start_offset > next_offset)) {
        trace_usb_ohci_iso_td_bad_cc_overrun(start_offset, next_offset);
        return 1;
    }

    if ((start_offset & 0x1000) == 0) {
        start_addr = (iso_td.bp & OHCI_PAGE_MASK) |
            (start_offset & OHCI_OFFSET_MASK);
    } else {
        start_addr = (iso_td.be & OHCI_PAGE_MASK) |
            (start_offset & OHCI_OFFSET_MASK);
    }

    if (relative_frame_number < frame_count) {
        end_offset = next_offset - 1;
        if ((end_offset & 0x1000) == 0) {
            end_addr = (iso_td.bp & OHCI_PAGE_MASK) |
                (end_offset & OHCI_OFFSET_MASK);
        } else {
            end_addr = (iso_td.be & OHCI_PAGE_MASK) |
                (end_offset & OHCI_OFFSET_MASK);
        }
    } else {
        /* Last packet in the ISO TD */
        end_addr = next_offset;
    }

    if (start_addr > end_addr) {
        trace_usb_ohci_iso_td_bad_cc_overrun(start_addr, end_addr);
        return 1;
    }

    if ((start_addr & OHCI_PAGE_MASK) != (end_addr & OHCI_PAGE_MASK)) {
        len = (end_addr & OHCI_OFFSET_MASK) + 0x1001
            - (start_addr & OHCI_OFFSET_MASK);
    } else {
        len = end_addr - start_addr + 1;
    }
    if (len > sizeof(buf)) {
        len = sizeof(buf);
    }

    if (len && dir != OHCI_TD_DIR_IN) {
        if (ohci_copy_iso_td(ohci, start_addr, end_addr, buf, len,
                             DMA_DIRECTION_TO_DEVICE)) {
            ohci_die(ohci);
            return 1;
        }
    }

    dev = ohci_find_device(ohci, OHCI_BM(ed->flags, ED_FA));
    if (dev == NULL) {
        trace_usb_ohci_td_dev_error();
        return 1;
    }
    ep = usb_ep_get(dev, pid, OHCI_BM(ed->flags, ED_EN));
    pkt = g_new0(USBPacket, 1);
    usb_packet_init(pkt);
    int_req = relative_frame_number == frame_count &&
              OHCI_BM(iso_td.flags, TD_DI) == 0;
    usb_packet_setup(pkt, pid, ep, 0, addr, false, int_req);
    usb_packet_addbuf(pkt, buf, len);
    usb_handle_packet(dev, pkt);
    if (pkt->status == USB_RET_ASYNC) {
        usb_device_flush_ep_queue(dev, ep);
        g_free(pkt);
        return 1;
    }
    if (pkt->status == USB_RET_SUCCESS) {
        ret = pkt->actual_length;
    } else {
        ret = pkt->status;
    }
    g_free(pkt);

    trace_usb_ohci_iso_td_so(start_offset, end_offset, start_addr, end_addr,
                             str, len, ret);

    /* Writeback */
    if (dir == OHCI_TD_DIR_IN && ret >= 0 && ret <= len) {
        /* IN transfer succeeded */
        if (ohci_copy_iso_td(ohci, start_addr, end_addr, buf, ret,
                             DMA_DIRECTION_FROM_DEVICE)) {
            ohci_die(ohci);
            return 1;
        }
        OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_CC,
                    OHCI_CC_NOERROR);
        OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_SIZE, ret);
    } else if (dir == OHCI_TD_DIR_OUT && ret == len) {
        /* OUT transfer succeeded */
        OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_CC,
                    OHCI_CC_NOERROR);
        OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_SIZE, 0);
    } else {
        if (ret > (ssize_t) len) {
            trace_usb_ohci_iso_td_data_overrun(ret, len);
            OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_CC,
                        OHCI_CC_DATAOVERRUN);
            OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_SIZE,
                        len);
        } else if (ret >= 0) {
            trace_usb_ohci_iso_td_data_underrun(ret);
            OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_CC,
                        OHCI_CC_DATAUNDERRUN);
        } else {
            switch (ret) {
            case USB_RET_IOERROR:
            case USB_RET_NODEV:
                OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_CC,
                            OHCI_CC_DEVICENOTRESPONDING);
                OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_SIZE,
                            0);
                break;
            case USB_RET_NAK:
            case USB_RET_STALL:
                trace_usb_ohci_iso_td_nak(ret);
                OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_CC,
                            OHCI_CC_STALL);
                OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_SIZE,
                            0);
                break;
            default:
                trace_usb_ohci_iso_td_bad_response(ret);
                OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_CC,
                            OHCI_CC_UNDEXPETEDPID);
                break;
            }
        }
    }

    if (relative_frame_number == frame_count) {
        /* Last data packet of ISO TD - retire the TD to the Done Queue */
        OHCI_SET_BM(iso_td.flags, TD_CC, OHCI_CC_NOERROR);
        ed->head &= ~OHCI_DPTR_MASK;
        ed->head |= (iso_td.next & OHCI_DPTR_MASK);
        iso_td.next = ohci->done;
        ohci->done = addr;
        i = OHCI_BM(iso_td.flags, TD_DI);
        if (i < ohci->done_count) {
            ohci->done_count = i;
        }
    }
    if (ohci_put_iso_td(ohci, addr, &iso_td)) {
        ohci_die(ohci);
    }
    return 1;
}

#define HEX_CHAR_PER_LINE 16

static void ohci_td_pkt(const char *msg, const uint8_t *buf, size_t len)
{
    return;
    bool print16;
    bool printall;
    int i;
    char tmp[3 * HEX_CHAR_PER_LINE + 1];
    char *p = tmp;

    print16 = !!trace_event_get_state_backends(TRACE_USB_OHCI_TD_PKT_SHORT);
    printall = !!trace_event_get_state_backends(TRACE_USB_OHCI_TD_PKT_FULL);

    if (!printall && !print16) {
        return;
    }

    for (i = 0; ; i++) {
        if (i && (!(i % HEX_CHAR_PER_LINE) || (i == len))) {
            if (!printall) {
                trace_usb_ohci_td_pkt_short(msg, tmp);
                break;
            }
            trace_usb_ohci_td_pkt_full(msg, tmp);
            p = tmp;
            *p = 0;
        }
        if (i == len) {
            break;
        }

        p += sprintf(p, " %.2x", buf[i]);
    }
}

/*
 * Service a transport descriptor.
 * Returns nonzero to terminate processing of this endpoint.
 */
static int ohci_service_td(OHCIState *ohci, struct ohci_ed *ed)
{
    extern uint64_t perf_cnt_ohci_td;
    perf_cnt_ohci_td++;
    int dir;
    size_t len = 0, pktlen = 0;
    const char *str = NULL;
    int pid;
    int ret;
    int i;
    USBDevice *dev;
    USBEndpoint *ep;
    struct ohci_td td;
    uint32_t addr;
    int flag_r;
    int completion;

    addr = ed->head & OHCI_DPTR_MASK;
    if (addr == 0) {
        ohci_die(ohci);
        return 1;
    }

    /* See if this TD has already been submitted to the device. */
    completion = (addr == ohci->async_td);
    if (completion && !ohci->async_complete) {
        trace_usb_ohci_td_skip_async();
        return 1;
    }
    if (ohci_read_td(ohci, addr, &td)) {
        trace_usb_ohci_td_read_error(addr);
        ohci_die(ohci);
        return 1;
    }

    if (TS_MS > 5900)
        if(0) printf("[%07lld] OHCI TD: ed_addr=0x%08X td_addr=0x%08X FA=%d EN=%d "
               "cbp=0x%08X be=0x%08X flags=0x%08X next=0x%08X\n",
               TS_MS, ed->head & OHCI_DPTR_MASK, addr,
               OHCI_BM(ed->flags, ED_FA), OHCI_BM(ed->flags, ED_EN),
               td.cbp, td.be, td.flags, td.next);

    dir = OHCI_BM(ed->flags, ED_D);
    switch (dir) {
    case OHCI_TD_DIR_OUT:
    case OHCI_TD_DIR_IN:
        /* Same value. */
        break;
    default:
        dir = OHCI_BM(td.flags, TD_DP);
        break;
    }

    switch (dir) {
    case OHCI_TD_DIR_IN:
        str = "in";
        pid = USB_TOKEN_IN;
        break;
    case OHCI_TD_DIR_OUT:
        str = "out";
        pid = USB_TOKEN_OUT;
        break;
    case OHCI_TD_DIR_SETUP:
        str = "setup";
        pid = USB_TOKEN_SETUP;
        if (OHCI_BM(ed->flags, ED_EN) > 0) {  /* setup only allowed to ep 0 */
            trace_usb_ohci_td_bad_pid(str, ed->flags, td.flags);
            ohci_die(ohci);
            return 1;
        }
        break;
    default:
        trace_usb_ohci_td_bad_direction(dir);
        return 1;
    }
    if (td.cbp && td.be) {
        if ((td.cbp & 0xfffff000) != (td.be & 0xfffff000)) {
            len = (td.be & 0xfff) + 0x1001 - (td.cbp & 0xfff);
        } else {
            if (td.cbp - 1 > td.be) {  /* rely on td.cbp != 0 */
                trace_usb_ohci_td_bad_buf(td.cbp, td.be);
                ohci_die(ohci);
                return 1;
            }
            len = (td.be - td.cbp) + 1;
        }
        if (len > sizeof(ohci->usb_buf)) {
            len = sizeof(ohci->usb_buf);
        }

        pktlen = len;
        if (len && dir != OHCI_TD_DIR_IN) {
            /* The endpoint may not allow us to transfer it all now */
            pktlen = (ed->flags & OHCI_ED_MPS_MASK) >> OHCI_ED_MPS_SHIFT;
            if (pktlen > len) {
                pktlen = len;
            }
            if (!completion) {
                if (ohci_copy_td(ohci, &td, ohci->usb_buf, pktlen,
                                 DMA_DIRECTION_TO_DEVICE)) {
                    ohci_die(ohci);
                }
            }
        }
    }

    flag_r = (td.flags & OHCI_TD_R) != 0;
    trace_usb_ohci_td_pkt_hdr(addr, (int64_t)pktlen, (int64_t)len, str,
                              flag_r, td.cbp, td.be);
    ohci_td_pkt("OUT", ohci->usb_buf, pktlen);

    if (TS_MS > 5900 && pid == USB_TOKEN_SETUP && pktlen == 8) {
        uint8_t *s = ohci->usb_buf;
        uint16_t wVal = s[2]|(s[3]<<8);
        uint16_t wLen = s[6]|(s[7]<<8);
        const char *tag = "other";
        if (s[1]==0x05) tag = "SET_ADDRESS";
        else if (s[1]==0x09) tag = "SET_CONFIG";
        else if (s[1]==0x06 && wVal==0x0100 && wLen<=8) tag = "GET_DESCRIPTOR(DEV8)";
        else if (s[1]==0x06 && wVal==0x0100 && wLen>8) tag = "GET_DESCRIPTOR(DEV_FULL)";
        else if (s[1]==0x06 && wVal==0x0200) tag = "GET_DESCRIPTOR(CONFIG)★";
        else if (s[1]==0x06) tag = "GET_DESCRIPTOR";
        if(0) printf("[%07lld] OHCI SETUP: bmReq=%02X bReq=%02X wVal=%04X wIdx=%04X wLen=%04X "
               "FA=%d EN=%d [%s]\n",
               TS_MS, s[0], s[1], wVal, s[4]|(s[5]<<8), wLen,
               OHCI_BM(ed->flags, ED_FA), OHCI_BM(ed->flags, ED_EN), tag);

        /* v291: At SET_ADDRESS SETUP time, read sub-state from verified PA */
        if (s[1] == 0x05) {
            uint8_t sub, err, cls;
            cpu_physical_memory_read(0x0F1D0B, &sub, 1);
            cpu_physical_memory_read(0x0F1D09, &err, 1);
            cpu_physical_memory_read(0x0F1D90, &cls, 1);
            if(0) printf("[%07lld] @SET_ADDR: substate=%d err=%d class=0x%02X wVal=%d\n",
                   TS_MS, sub, err, cls, wVal);
        }
    }

    if (completion) {
        ohci->async_td = 0;
        ohci->async_complete = false;
    } else {
        dev = ohci_find_device(ohci, OHCI_BM(ed->flags, ED_FA));
        if (dev == NULL) {
            trace_usb_ohci_td_dev_error();
            return 1;
        }
        ep = usb_ep_get(dev, pid, OHCI_BM(ed->flags, ED_EN));
        if (ohci->async_td) {
            /*
             * ??? The hardware should allow one active packet per
             * endpoint.  We only allow one active packet per controller.
             * This should be sufficient as long as devices respond in a
             * timely manner.
             */
            trace_usb_ohci_td_too_many_pending(ep->nr);
            return 1;
        }
        usb_packet_setup(&ohci->usb_packet, pid, ep, 0, addr, !flag_r,
                         OHCI_BM(td.flags, TD_DI) == 0);
        usb_packet_addbuf(&ohci->usb_packet, ohci->usb_buf, pktlen);
        usb_handle_packet(dev, &ohci->usb_packet);
        if (TS_MS > 5900)
            if(0) printf("[%07lld] OHCI TD RESULT: FA=%d EN=%d dir=%s status=%d len=%zd\n", TS_MS,
                   OHCI_BM(ed->flags, ED_FA), OHCI_BM(ed->flags, ED_EN),
                   str, ohci->usb_packet.status,
                   ohci->usb_packet.actual_length);
        trace_usb_ohci_td_packet_status(ohci->usb_packet.status);

        if (ohci->usb_packet.status == USB_RET_ASYNC) {
            usb_device_flush_ep_queue(dev, ep);
            ohci->async_td = addr;
            return 1;
        }
    }
    if (ohci->usb_packet.status == USB_RET_SUCCESS) {
        ret = ohci->usb_packet.actual_length;
    } else {
        ret = ohci->usb_packet.status;
    }

    if (ret >= 0) {
        if (dir == OHCI_TD_DIR_IN) {
            if (ohci_copy_td(ohci, &td, ohci->usb_buf, ret,
                             DMA_DIRECTION_FROM_DEVICE)) {
                ohci_die(ohci);
            }
            ohci_td_pkt("IN", ohci->usb_buf, pktlen);
            if (TS_MS > 5900 && ret > 0 && ret <= 18 &&
                OHCI_BM(ed->flags, ED_EN) == 0) {
                if(0) printf("[%07lld] OHCI IN-DATA FA=%d ret=%d:",
                       TS_MS, OHCI_BM(ed->flags, ED_FA), ret);
                for (int _i = 0; _i < ret && _i < 18; _i++)
                    if(0) printf(" %02X", ohci->usb_buf[_i]);
                if(0) printf("\n");
            }
        } else {
            ret = pktlen;
        }
    }

    if (ret >= 0) {
        if ((td.cbp & 0xfff) + ret > 0xfff) {
            td.cbp = (td.be & ~0xfff) + ((td.cbp + ret) & 0xfff);
        } else {
            td.cbp += ret;
        }
    }

    /* Writeback */
    if (ret == pktlen || (dir == OHCI_TD_DIR_IN && ret >= 0 && flag_r)) {
        /* Transmission succeeded. */
        if (ret == len) {
            td.cbp = 0;
        }
        td.flags |= OHCI_TD_T1;
        td.flags ^= OHCI_TD_T0;
        OHCI_SET_BM(td.flags, TD_CC, OHCI_CC_NOERROR);
        OHCI_SET_BM(td.flags, TD_EC, 0);

        if ((dir != OHCI_TD_DIR_IN) && (ret != len)) {
            /* Partial packet transfer: TD not ready to retire yet */
            goto exit_no_retire;
        }

        /* Setting ED_C is part of the TD retirement process */
        ed->head &= ~OHCI_ED_C;
        if (td.flags & OHCI_TD_T0) {
            ed->head |= OHCI_ED_C;
        }
    } else {
        if (ret >= 0) {
            trace_usb_ohci_td_underrun();
            OHCI_SET_BM(td.flags, TD_CC, OHCI_CC_DATAUNDERRUN);
        } else {
            switch (ret) {
            case USB_RET_IOERROR:
            case USB_RET_NODEV:
                trace_usb_ohci_td_dev_error();
                if (TS_MS > 5900)
                    if(0) printf("[%07lld] OHCI TD NODEV: FA=%d EN=%d dir=%s — device not responding!\n",
                           TS_MS, OHCI_BM(ed->flags, ED_FA), OHCI_BM(ed->flags, ED_EN), str);
                OHCI_SET_BM(td.flags, TD_CC, OHCI_CC_DEVICENOTRESPONDING);
                break;
            case USB_RET_NAK:
                trace_usb_ohci_td_nak();
                /* v202: Throttled NAK logging */
                {
                    static uint32_t ohci_nak_count = 0;
                    ohci_nak_count++;
                    if (TS_MS > 5900 && (ohci_nak_count <= 3 || (ohci_nak_count % 1000) == 0)) {
                        if(0) printf("[%07lld] OHCI TD NAK: FA=%d EN=%d dir=%s (nak#%u)\n",
                               TS_MS,
                               OHCI_BM(ed->flags, ED_FA),
                               OHCI_BM(ed->flags, ED_EN),
                               str, ohci_nak_count);
                    }
                }
                return 1;
            case USB_RET_STALL:
                trace_usb_ohci_td_stall();
                if (TS_MS > 5900)
                    if(0) printf("[%07lld] OHCI TD STALL: FA=%d EN=%d dir=%s — endpoint stalled!\n",
                           TS_MS, OHCI_BM(ed->flags, ED_FA), OHCI_BM(ed->flags, ED_EN), str);
                OHCI_SET_BM(td.flags, TD_CC, OHCI_CC_STALL);
                break;
            case USB_RET_BABBLE:
                trace_usb_ohci_td_babble();
                OHCI_SET_BM(td.flags, TD_CC, OHCI_CC_DATAOVERRUN);
                break;
            default:
                trace_usb_ohci_td_bad_device_response(ret);
                OHCI_SET_BM(td.flags, TD_CC, OHCI_CC_UNDEXPETEDPID);
                OHCI_SET_BM(td.flags, TD_EC, 3);
                break;
            }
            /*
             * An error occurred so we have to clear the interrupt counter.
             * See spec at 6.4.4 on page 104
             */
            ohci->done_count = 0;
        }
        ed->head |= OHCI_ED_H;
    }

    /* Retire this TD */
    ed->head &= ~OHCI_DPTR_MASK;
    ed->head |= td.next & OHCI_DPTR_MASK;
    td.next = ohci->done;
    ohci->done = addr;
    i = OHCI_BM(td.flags, TD_DI);
    if (i < ohci->done_count) {
        ohci->done_count = i;
    }
    if (TS_MS > 5900)
        if(0) printf("[%07lld] OHCI TD RETIRE: td=0x%08X CC=%d DI=%d done_count=%d "
               "ed_FA=%d ed_EN=%d\n",
               TS_MS, addr, OHCI_BM(td.flags, TD_CC), i, ohci->done_count,
               OHCI_BM(ed->flags, ED_FA), OHCI_BM(ed->flags, ED_EN));
    if (OHCI_BM(td.flags, TD_CC) != OHCI_CC_NOERROR) {
        ohci->done_count = 0;
    }
exit_no_retire:
    if (ohci_put_td(ohci, addr, &td)) {
        ohci_die(ohci);
        return 1;
    }
    return OHCI_BM(td.flags, TD_CC) != OHCI_CC_NOERROR;
}

/* Service an endpoint list.  Returns nonzero if active TD were found. */
static int ohci_service_ed_list(OHCIState *ohci, uint32_t head)
{
    struct ohci_ed ed;
    uint32_t next_ed;
    uint32_t cur;
    int active;
    uint32_t link_cnt = 0;
    active = 0;

    if (head == 0) {
        return 0;
    }
    for (cur = head; cur && link_cnt++ < ED_LINK_LIMIT; cur = next_ed) {
        if (ohci_read_ed(ohci, cur, &ed)) {
            trace_usb_ohci_ed_read_error(cur);
            ohci_die(ohci);
            return 0;
        }

        next_ed = ed.next & OHCI_DPTR_MASK;

        if ((ed.head & OHCI_ED_H) || (ed.flags & OHCI_ED_K)) {
            uint32_t addr;
            /* v202: Log halted/skipped EDs */
            static uint32_t skip_count = 0;
            skip_count++;
            if (TS_MS > 5900 && (skip_count <= 5 || (skip_count % 100) == 0)) {
                if(0) printf("[%07lld] OHCI ED SKIP: @0x%08X FA=%d EN=%d H=%d K=%d (skip#%u)\n",
                       TS_MS, cur,
                       OHCI_BM(ed.flags, ED_FA), OHCI_BM(ed.flags, ED_EN),
                       (ed.head & OHCI_ED_H) ? 1 : 0,
                       (ed.flags & OHCI_ED_K) ? 1 : 0,
                       skip_count);
            }
            /* Cancel pending packets for ED that have been paused. */
            addr = ed.head & OHCI_DPTR_MASK;
            if (ohci->async_td && addr == ohci->async_td) {
                usb_cancel_packet(&ohci->usb_packet);
                ohci->async_td = 0;
                usb_device_ep_stopped(ohci->usb_packet.ep->dev,
                                      ohci->usb_packet.ep);
            }
            continue;
        }

        while ((ed.head & OHCI_DPTR_MASK) != ed.tail) {
            if (TS_MS > 5900)
                if(0) printf("[%07lld] OHCI ED: @0x%08X FA=%d EN=%d D=%d K=%d F=%d MPS=%d "
                       "head=0x%08X tail=0x%08X\n",
                       TS_MS, cur,
                       OHCI_BM(ed.flags, ED_FA), OHCI_BM(ed.flags, ED_EN),
                       OHCI_BM(ed.flags, ED_D),
                       (ed.flags & OHCI_ED_K) ? 1 : 0,
                       (ed.flags & OHCI_ED_F) ? 1 : 0,
                       OHCI_BM(ed.flags, ED_MPS),
                       ed.head, ed.tail);
            trace_usb_ohci_ed_pkt(cur, (ed.head & OHCI_ED_H) != 0,
                    (ed.head & OHCI_ED_C) != 0, ed.head & OHCI_DPTR_MASK,
                    ed.tail & OHCI_DPTR_MASK, ed.next & OHCI_DPTR_MASK);
            trace_usb_ohci_ed_pkt_flags(
                    OHCI_BM(ed.flags, ED_FA), OHCI_BM(ed.flags, ED_EN),
                    OHCI_BM(ed.flags, ED_D), (ed.flags & OHCI_ED_S) != 0,
                    (ed.flags & OHCI_ED_K) != 0, (ed.flags & OHCI_ED_F) != 0,
                    OHCI_BM(ed.flags, ED_MPS));

            active = 1;

            if ((ed.flags & OHCI_ED_F) == 0) {
                if (ohci_service_td(ohci, &ed)) {
                    break;
                }
            } else {
                /* Handle isochronous endpoints */
                if (ohci_service_iso_td(ohci, &ed)) {
                    break;
                }
            }
        }

        if (ohci_put_ed(ohci, cur, &ed)) {
            ohci_die(ohci);
            return 0;
        }
    }

    return active;
}

/* set a timer for EOF */
static void ohci_eof_timer(OHCIState *ohci)
{
    timer_mod(ohci->eof_timer, ohci->sof_time + usb_frame_time);
}
/* Set a timer for EOF and generate a SOF event */
static void ohci_sof(OHCIState *ohci)
{
    ohci->sof_time += usb_frame_time;
    ohci_eof_timer(ohci);
    ohci_set_interrupt(ohci, OHCI_INTR_SF);
}

/* Process Control and Bulk lists. */
static void ohci_process_lists(OHCIState *ohci)
{
    if ((ohci->ctl & OHCI_CTL_CLE) && (ohci->status & OHCI_STATUS_CLF)) {
        if (TS_MS > 5900)
            if(0) printf("[%07lld] OHCI: === Processing CONTROL list head=0x%08X ===\n", TS_MS, ohci->ctrl_head);
        if (ohci->ctrl_cur && ohci->ctrl_cur != ohci->ctrl_head) {
            trace_usb_ohci_process_lists(ohci->ctrl_head, ohci->ctrl_cur);
        }
        if (!ohci_service_ed_list(ohci, ohci->ctrl_head)) {
            ohci->ctrl_cur = 0;
            ohci->status &= ~OHCI_STATUS_CLF;
        }
    }

    if ((ohci->ctl & OHCI_CTL_BLE) && (ohci->status & OHCI_STATUS_BLF)) {
        if (TS_MS > 5900)
            if(0) printf("[%07lld] OHCI: === Processing BULK list head=0x%08X ===\n", TS_MS, ohci->bulk_head);
        if (!ohci_service_ed_list(ohci, ohci->bulk_head)) {
            ohci->bulk_cur = 0;
            ohci->status &= ~OHCI_STATUS_BLF;
        }
    }
}

/* Do frame processing on frame boundary */
static void ohci_frame_boundary(void *opaque)
{
    extern uint64_t perf_cnt_ohci_frame;
    perf_cnt_ohci_frame++;

    /* NOTE: upstream-chihiro drove the NV2A PCRTC vblank from here (every
     * 17 OHCI frames). This fork has a dedicated adaptive vblank timer in
     * nv2a.c, so that tick is deliberately omitted -- adding it back would
     * double-fire vblanks. */

    OHCIState *ohci = opaque;
    struct ohci_hcca hcca;

    /* Log HcControl changes and ED heads after SEGABOOT loads */
    static uint32_t prev_ctl = 0xFFFFFFFF;
    static int64_t last_heads_log = 0;
    if (TS_MS > 5900 && ohci->ctl != prev_ctl) {
        if(0) printf("[%07lld] OHCI FRAME: ctl=0x%08X (HCFS=%d CLE=%d BLE=%d PLE=%d) "
               "ctrl_head=0x%08X bulk_head=0x%08X hcca=0x%08X\n", TS_MS,
               ohci->ctl, (ohci->ctl >> 6) & 3,
               (ohci->ctl >> 4) & 1, (ohci->ctl >> 5) & 1, (ohci->ctl >> 2) & 1,
               ohci->ctrl_head, ohci->bulk_head, ohci->hcca);
        prev_ctl = ohci->ctl;
    }
    if (TS_MS > 5900 && (ohci->ctrl_head || ohci->bulk_head) &&
        TS_MS - last_heads_log > 500) {
        if(0) printf("[%07lld] OHCI ACTIVE: ctrl_head=0x%08X ctrl_cur=0x%08X "
               "bulk_head=0x%08X bulk_cur=0x%08X intr=0x%08X\n", TS_MS,
               ohci->ctrl_head, ohci->ctrl_cur,
               ohci->bulk_head, ohci->bulk_cur, ohci->intr_status);
        last_heads_log = TS_MS;
    }

    if (0 && TS_MS > 5900) {
        static uint8_t prev_substate = 0, prev_errflag = 0, prev_devclass = 0;
        static bool watchers_active = false;
        uint8_t cur_substate, cur_errflag, cur_devclass;
        cpu_physical_memory_read(0x0F1D0B, &cur_substate, 1);
        cpu_physical_memory_read(0x0F1D09, &cur_errflag, 1);
        cpu_physical_memory_read(0x0F1D90, &cur_devclass, 1);
        if (!watchers_active) {
            if(0) printf("[%07lld] USB-WATCH: init substate=%d err=%d class=0x%02X\n",
                   TS_MS, cur_substate, cur_errflag, cur_devclass);
            prev_substate = cur_substate;
            prev_errflag = cur_errflag;
            prev_devclass = cur_devclass;
            watchers_active = true;
        }
        if (cur_substate != prev_substate) {
            uint32_t ctx_ptr = 0;
            cpu_physical_memory_read(0x0F1D18, &ctx_ptr, 4);
            uint32_t slot = 0xDEAD;
            uint8_t ctx_addr_byte = 0xFF;
            uint8_t ctx_state = 0xFF;
            uint32_t ctx_hced = 0xDEAD;
            uint32_t ctx_hcbase = 0xDEAD;
            uint32_t ctx_classdrv = 0xDEAD;
            uint32_t ed_freelist = 0xDEAD;
            uint8_t devtab_flags = 0xFF;
            uint32_t devtab_class = 0xDEAD;
            uint32_t cdrv_tbl1 = 0xDEAD;
            extern uint32_t chihiro_va_to_pa(uint32_t va);
            if (ctx_ptr != 0) {
                uint32_t ctx_pa = chihiro_va_to_pa(ctx_ptr);
                if (ctx_pa != 0xFFFFFFFF) {
                    cpu_physical_memory_read(ctx_pa + 0x00, &ctx_state, 1);
                    cpu_physical_memory_read(ctx_pa + 0x05, &ctx_addr_byte, 1);
                    cpu_physical_memory_read(ctx_pa + 0x08, &ctx_hced, 4);
                    cpu_physical_memory_read(ctx_pa + 0x0C, &ctx_hcbase, 4);
                    cpu_physical_memory_read(ctx_pa + 0x10, &ctx_classdrv, 4);
                    cpu_physical_memory_read(ctx_pa + 0x14, &slot, 4);
                }
            }
            uint32_t fl_pa = chihiro_va_to_pa(0xEFBC8);
            if (fl_pa != 0xFFFFFFFF) {
                cpu_physical_memory_read(fl_pa, &ed_freelist, 4);
            }
            uint32_t dt_pa = chihiro_va_to_pa(0xE9BD4);
            if (dt_pa != 0xFFFFFFFF) {
                cpu_physical_memory_read(dt_pa, &devtab_flags, 1);
            }
            uint32_t dc_pa = chihiro_va_to_pa(0xE9DDC);
            if (dc_pa != 0xFFFFFFFF) {
                cpu_physical_memory_read(dc_pa, &devtab_class, 4);
            }
            uint32_t ct_pa = chihiro_va_to_pa(0x112E8);
            if (ct_pa != 0xFFFFFFFF) {
                cpu_physical_memory_read(ct_pa, &cdrv_tbl1, 4);
            }
            /* v293: Read classdrv table entries + entry bytes at runtime */
            uint8_t ctx_maxpkt = 0;
            if (ctx_ptr != 0) {
                uint32_t ctx_pa2 = chihiro_va_to_pa(ctx_ptr);
                if (ctx_pa2 != 0xFFFFFFFF)
                    cpu_physical_memory_read(ctx_pa2 + 0x06, &ctx_maxpkt, 1);
            }
            uint32_t cdt[5] = {0};
            uint8_t cde_bytes[4] = {0};
            uint32_t tbl_pa = chihiro_va_to_pa(0x112E4);
            if (tbl_pa != 0xFFFFFFFF) {
                cpu_physical_memory_read(tbl_pa, cdt, 20);
            }
            if (cdt[1] != 0) {
                uint32_t ent_pa = chihiro_va_to_pa(cdt[1]);
                if (ent_pa != 0xFFFFFFFF)
                    cpu_physical_memory_read(ent_pa, cde_bytes, 4);
            }
            if(0) printf("[%07lld] USB-WATCH: substate %d→%d (err=%d class=0x%02X ctx=0x%X"
                   " st=%u addr=%u mps=%u slot=%u hced=0x%X hcb=0x%X cdrv=0x%X"
                   " edfl=0x%X dtf=0x%02X dtc=0x%X)\n",
                   TS_MS, prev_substate, cur_substate, cur_errflag, cur_devclass,
                   ctx_ptr, ctx_state, ctx_addr_byte, ctx_maxpkt, slot, ctx_hced,
                   ctx_hcbase, ctx_classdrv, ed_freelist, devtab_flags, devtab_class);
            /* v299: Compare outer ctx (0xA6D18) vs inner dispatch ctx (0xA6D88) */
            {
                uint32_t inner_ctx = 0;
                uint8_t disp_state = 0xFF;
                uint32_t inner_pa_0a6d88 = chihiro_va_to_pa(0xA6D88);
                uint32_t inner_pa_0a6d80 = chihiro_va_to_pa(0xA6D80);
                if (inner_pa_0a6d88 != 0xFFFFFFFF)
                    cpu_physical_memory_read(inner_pa_0a6d88, &inner_ctx, 4);
                if (inner_pa_0a6d80 != 0xFFFFFFFF)
                    cpu_physical_memory_read(inner_pa_0a6d80, &disp_state, 1);
                uint32_t i_slot = 0xDEAD;
                uint8_t i_st = 0xFF, i_par = 0xFF, i_spd = 0xFF;
                if (inner_ctx != 0) {
                    uint32_t ipa = chihiro_va_to_pa(inner_ctx);
                    if (ipa != 0xFFFFFFFF) {
                        cpu_physical_memory_read(ipa + 0x00, &i_st, 1);
                        cpu_physical_memory_read(ipa + 0x01, &i_par, 1);
                        cpu_physical_memory_read(ipa + 0x04, &i_spd, 1);
                        cpu_physical_memory_read(ipa + 0x14, &i_slot, 4);
                    }
                }
                if(0) printf("  v299-DUAL: outer_ctx=0x%X inner_ctx=0x%X %s disp=%d"
                       " inner:[0]=%u [1]=0x%02X [4]=0x%02X slot=0x%X\n",
                       ctx_ptr, inner_ctx,
                       (ctx_ptr == inner_ctx) ? "SAME" : "DIFFER",
                       disp_state, i_st, i_par, i_spd, i_slot);
            }
            /* v294: Read first 2 bytes of ALL 5 classdrv entries */
            uint8_t eall[10] = {0};
            for (int ei = 0; ei < 5; ei++) {
                if (cdt[ei] != 0) {
                    uint32_t epa = chihiro_va_to_pa(cdt[ei]);
                    if (epa != 0xFFFFFFFF)
                        cpu_physical_memory_read(epa, &eall[ei*2], 2);
                }
            }
            /* Also read callback ptr from matched entry (CDT[1]+8) */
            uint32_t cb_ptr = 0;
            if (cdt[1] != 0) {
                uint32_t cbpa = chihiro_va_to_pa(cdt[1] + 8);
                if (cbpa != 0xFFFFFFFF)
                    cpu_physical_memory_read(cbpa, &cb_ptr, 4);
            }
            if(0) printf("  CDT={0x%X,0x%X,0x%X,0x%X,0x%X}"
                   " E0=[%02X%02X] E1=[%02X%02X] E2=[%02X%02X] E3=[%02X%02X] E4=[%02X%02X]"
                   " cb1=0x%X\n",
                   cdt[0], cdt[1], cdt[2], cdt[3], cdt[4],
                   eall[0], eall[1], eall[2], eall[3], eall[4], eall[5],
                   eall[6], eall[7], eall[8], eall[9], cb_ptr);
            /* v295: CHAIN-DUMP — at substate 3 or 132, dump the parent chain
               FUN_000135b1: reads obj[1] as parent_index, returns DAT_000a6de8 + idx*32
               FUN_00013b51/000139ef check chain entries' [0](state) and [4](speed) */
            if ((cur_substate == 3 || cur_substate == 132) && ctx_ptr != 0) {
                uint32_t base_tbl = 0;
                uint32_t bt_pa = chihiro_va_to_pa(0xA6DE8);
                if (bt_pa != 0xFFFFFFFF)
                    cpu_physical_memory_read(bt_pa, &base_tbl, 4);
                uint8_t gflag = 0xFF;
                uint32_t gf_pa = chihiro_va_to_pa(0x112FC);
                if (gf_pa != 0xFFFFFFFF)
                    cpu_physical_memory_read(gf_pa, &gflag, 1);
                uint32_t thunk_val = 0;
                uint32_t tk_pa = chihiro_va_to_pa(0x110D0);
                if (tk_pa != 0xFFFFFFFF)
                    cpu_physical_memory_read(tk_pa, &thunk_val, 4);
                uint8_t thunk_flag = 0xFF;
                if (thunk_val != 0) {
                    uint32_t tf_pa = chihiro_va_to_pa(thunk_val);
                    if (tf_pa != 0xFFFFFFFF)
                        cpu_physical_memory_read(tf_pa, &thunk_flag, 1);
                }
                if(0) printf("  v295-CHAIN: base=0x%X gflag=%u thunk=0x%X *thunk=0x%02X(%s)\n",
                       base_tbl, gflag, thunk_val, thunk_flag,
                       (thunk_flag & 1) ? "PATH_A" : "PATH_B");
                uint32_t cur_va = ctx_ptr;
                for (int hop = 0; hop < 6; hop++) {
                    uint32_t epa = chihiro_va_to_pa(cur_va);
                    if (epa == 0xFFFFFFFF) {
                        if(0) printf("  v295-CHAIN[%d]: va=0x%X → PA fail\n", hop, cur_va);
                        break;
                    }
                    uint8_t es = 0, ep = 0x80, espd = 0;
                    cpu_physical_memory_read(epa + 0x00, &es, 1);
                    cpu_physical_memory_read(epa + 0x01, &ep, 1);
                    cpu_physical_memory_read(epa + 0x04, &espd, 1);
                    if(0) printf("  v295-CHAIN[%d]: va=0x%X [0]=%u [1]=%02X [4]=0x%02X\n",
                           hop, cur_va, es, ep, espd);
                    if (ep == 0x80) break;
                    if (base_tbl == 0) break;
                    cur_va = base_tbl + (uint32_t)ep * 32;
                }
            }
            /* v305-FIX removed: PATH_B is now selected via LPC bridge PCI
               revision >= 0xB4 (set in xbox.c for Chihiro mode), which clears
               XboxHardwareInfo bit 0 in the kernel at boot. Pure LLE. */
            /* v297: At substate 132 (callback), dump ED free list status + first free ED */
            if (cur_substate == 132) {
                uint32_t fl_va_pa = chihiro_va_to_pa(0xEFBC8);
                uint32_t fl_head = 0;
                if (fl_va_pa != 0xFFFFFFFF)
                    cpu_physical_memory_read(fl_va_pa, &fl_head, 4);
                if(0) printf("  v297-EDPOOL@132: EDFL_head=0x%X", fl_head);
                if (fl_head != 0) {
                    uint32_t fl_head_pa = chihiro_va_to_pa(fl_head);
                    if (fl_head_pa != 0xFFFFFFFF) {
                        uint32_t fe[4] = {0};
                        cpu_physical_memory_read(fl_head_pa, fe, 16);
                        if(0) printf(" → [flags=0x%08X tail=0x%08X head=0x%08X next=0x%08X]",
                               fe[0], fe[1], fe[2], fe[3]);
                    }
                }
                uint32_t pool_base_pa = chihiro_va_to_pa(0xEFBC0);
                uint32_t pool_base = 0;
                if (pool_base_pa != 0xFFFFFFFF)
                    cpu_physical_memory_read(pool_base_pa, &pool_base, 4);
                if(0) printf(" pool_va_base=0x%X\n", pool_base);
            }
            prev_substate = cur_substate;
        }
        if (cur_errflag != prev_errflag) {
            if(0) printf("[%07lld] USB-WATCH: ERROR FLAG %d→%d (substate=%d)\n",
                   TS_MS, prev_errflag, cur_errflag, cur_substate);
            prev_errflag = cur_errflag;
        }
        if (cur_devclass != prev_devclass) {
            if(0) printf("[%07lld] USB-WATCH: devclass 0x%02X→0x%02X (substate=%d)\n",
                   TS_MS, prev_devclass, cur_devclass, cur_substate);
            prev_devclass = cur_devclass;
        }
        {
            static uint32_t prev_slot_val = 0xDEADDEAD;
            static uint8_t prev_ctx_st = 0xFF;
            uint32_t ctx_ptr_w = 0;
            cpu_physical_memory_read(0x0F1D18, &ctx_ptr_w, 4);
            if (ctx_ptr_w != 0) {
                extern uint32_t chihiro_va_to_pa(uint32_t va);
                uint32_t cpa = chihiro_va_to_pa(ctx_ptr_w);
                if (cpa != 0xFFFFFFFF) {
                    uint32_t sv = 0;
                    uint8_t stv = 0;
                    cpu_physical_memory_read(cpa + 0x14, &sv, 4);
                    cpu_physical_memory_read(cpa + 0x00, &stv, 1);
                    if (sv != prev_slot_val || stv != prev_ctx_st) {
                        if(0) printf("[%07lld] SLOT-WATCH: ctx+0x14: 0x%X→0x%X  ctx[0]: %u→%u (substate=%d)\n",
                               TS_MS, prev_slot_val, sv, prev_ctx_st, stv, cur_substate);
                        prev_slot_val = sv;
                        prev_ctx_st = stv;
                    }
                }
            }
        }
        /* v295: HCED-WATCH — detect EP handle at context+0x08 change per frame */
        {
            static uint32_t prev_hced = 0xDEADDEAD;
            uint32_t ctx_ptr_h = 0;
            cpu_physical_memory_read(0x0F1D18, &ctx_ptr_h, 4);
            if (ctx_ptr_h != 0) {
                extern uint32_t chihiro_va_to_pa(uint32_t va);
                uint32_t cpa_h = chihiro_va_to_pa(ctx_ptr_h);
                if (cpa_h != 0xFFFFFFFF) {
                    uint32_t hv = 0;
                    cpu_physical_memory_read(cpa_h + 0x08, &hv, 4);
                    if (hv != prev_hced) {
                        if(0) printf("[%07lld] HCED-WATCH: ctx+0x08: 0x%X→0x%X (substate=%d)\n",
                               TS_MS, prev_hced, hv, cur_substate);
                        prev_hced = hv;
                    }
                }
            }
        }
        /* v294: CDRV-WATCH — detect classdrv result at context+0x10 per frame */
        {
            static uint32_t prev_cdrv = 0xDEADDEAD;
            uint32_t ctx_ptr_c = 0;
            cpu_physical_memory_read(0x0F1D18, &ctx_ptr_c, 4);
            if (ctx_ptr_c != 0) {
                extern uint32_t chihiro_va_to_pa(uint32_t va);
                uint32_t cpa_c = chihiro_va_to_pa(ctx_ptr_c);
                if (cpa_c != 0xFFFFFFFF) {
                    uint32_t cv = 0;
                    cpu_physical_memory_read(cpa_c + 0x10, &cv, 4);
                    if (cv != prev_cdrv) {
                        if(0) printf("[%07lld] CDRV-WATCH: ctx+0x10: 0x%X→0x%X (substate=%d)\n",
                               TS_MS, prev_cdrv, cv, cur_substate);
                        prev_cdrv = cv;
                    }
                }
            }
        }
        /* v292: EDFL-WATCH — detect any ED free list change per frame */
        {
            static uint32_t prev_edfl = 0xDEADDEAD;
            extern uint32_t chihiro_va_to_pa(uint32_t va);
            uint32_t fl_pa = chihiro_va_to_pa(0xEFBC8);
            if (fl_pa != 0xFFFFFFFF) {
                uint32_t cur_edfl = 0;
                cpu_physical_memory_read(fl_pa, &cur_edfl, 4);
                if (cur_edfl != prev_edfl) {
                    if(0) printf("[%07lld] EDFL-WATCH: 0x%X→0x%X (substate=%d)\n",
                           TS_MS, prev_edfl, cur_edfl, cur_substate);
                    prev_edfl = cur_edfl;
                }
            }
        }
        /* v295: DTFLAGS-WATCH — also monitor slot 2 and 3 flags */
        {
            static uint8_t prev_dtf0 = 0xFF;
            static uint8_t prev_dtf2 = 0xFF;
            static uint8_t prev_dtf3 = 0xFF;
            extern uint32_t chihiro_va_to_pa(uint32_t va);
            uint32_t dtf_pa = chihiro_va_to_pa(0xE9BD4);
            if (dtf_pa != 0xFFFFFFFF) {
                uint8_t cur_dtf0 = 0;
                cpu_physical_memory_read(dtf_pa, &cur_dtf0, 1);
                if (cur_dtf0 != prev_dtf0) {
                    if(0) printf("[%07lld] DTFLAGS-WATCH: slot0 0x%02X→0x%02X (substate=%d)\n",
                           TS_MS, prev_dtf0, cur_dtf0, cur_substate);
                    prev_dtf0 = cur_dtf0;
                }
            }
            uint32_t dtf2_pa = chihiro_va_to_pa(0xEA004);
            if (dtf2_pa != 0xFFFFFFFF) {
                uint8_t cur_dtf2 = 0;
                cpu_physical_memory_read(dtf2_pa, &cur_dtf2, 1);
                if (cur_dtf2 != prev_dtf2) {
                    if(0) printf("[%07lld] DTFLAGS-WATCH: slot2 0x%02X→0x%02X (substate=%d)\n",
                           TS_MS, prev_dtf2, cur_dtf2, cur_substate);
                    prev_dtf2 = cur_dtf2;
                }
            }
            uint32_t dtf3_pa = chihiro_va_to_pa(0xEA21C);
            if (dtf3_pa != 0xFFFFFFFF) {
                uint8_t cur_dtf3 = 0;
                cpu_physical_memory_read(dtf3_pa, &cur_dtf3, 1);
                if (cur_dtf3 != prev_dtf3) {
                    if(0) printf("[%07lld] DTFLAGS-WATCH: slot3 0x%02X→0x%02X (substate=%d)\n",
                           TS_MS, prev_dtf3, cur_dtf3, cur_substate);
                    prev_dtf3 = cur_dtf3;
                }
            }
        }
    }

    if (ohci_read_hcca(ohci, ohci->hcca, &hcca)) {
        trace_usb_ohci_hcca_read_error(ohci->hcca);
        ohci_die(ohci);
        return;
    }

    /* Process all the lists at the end of the frame */
    if (ohci->ctl & OHCI_CTL_PLE) {
        int n;

        n = ohci->frame_number & 0x1f;
        ohci_service_ed_list(ohci, le32_to_cpu(hcca.intr[n]));
    }

    /* Cancel all pending packets if either of the lists has been disabled. */
    if (ohci->old_ctl & (~ohci->ctl) & (OHCI_CTL_BLE | OHCI_CTL_CLE)) {
        ohci_stop_endpoints(ohci);
    }
    ohci->old_ctl = ohci->ctl;
    ohci_process_lists(ohci);

    /* Stop if UnrecoverableError happened or ohci_sof will crash */
    if (ohci->intr_status & OHCI_INTR_UE) {
        return;
    }

    /* Frame boundary, so do EOF stuf here */
    ohci->frt = ohci->fit;

    /* Increment frame number and take care of endianness. */
    ohci->frame_number = (ohci->frame_number + 1) & 0xffff;
    hcca.frame = cpu_to_le16(ohci->frame_number);
    /* When the HC updates frame number, set pad to 0. Ref OHCI Spec 4.4.1*/
    hcca.pad = 0;

    if (ohci->done_count == 0 && !(ohci->intr_status & OHCI_INTR_WD)) {
        if (!ohci->done) {
            abort();
        }
        if (ohci->intr & ohci->intr_status) {
            ohci->done |= 1;
        }
        if (TS_MS > 5900)
            if(0) printf("[%07lld] OHCI DONE->HCCA: done=0x%08X hcca=0x%08X\n",
                   TS_MS, ohci->done, ohci->hcca);
        hcca.done = cpu_to_le32(ohci->done);
        ohci->done = 0;
        ohci->done_count = 7;
        ohci_set_interrupt(ohci, OHCI_INTR_WD);
    }

    if (ohci->done_count != 7 && ohci->done_count != 0) {
        ohci->done_count--;
    }

    /* v289: Track WDH acknowledgment — detect if SEGABOOT ISR processes done list */
    {
        static int64_t wdh_set_time = 0;
        static bool wdh_warned = false;
        if (ohci->intr_status & OHCI_INTR_WD) {
            if (wdh_set_time == 0) wdh_set_time = TS_MS;
            if (!wdh_warned && TS_MS - wdh_set_time > 50) {
                if(0) printf("[%07lld] OHCI WDH-STALL: WDH set for %lldms — "
                       "guest ISR not acknowledging! intr_status=0x%08X "
                       "intr=0x%08X\n", TS_MS, TS_MS - wdh_set_time,
                       ohci->intr_status, ohci->intr);
                wdh_warned = true;
            }
        } else {
            if (wdh_set_time && TS_MS > 5900 && !wdh_warned) {
                if(0) printf("[%07lld] OHCI WDH-ACK: cleared after %lldms\n",
                       TS_MS, TS_MS - wdh_set_time);
            }
            wdh_set_time = 0;
            wdh_warned = false;
        }
    }

    /* Do SOF stuff here */
    ohci_sof(ohci);

    /* Writeback HCCA */
    if (ohci_put_hcca(ohci, ohci->hcca, &hcca)) {
        ohci_die(ohci);
    }
}

/*
 * Start sending SOF tokens across the USB bus, lists are processed in
 * next frame
 */
static int ohci_bus_start(OHCIState *ohci)
{
    printf("[%07lld] OHCI BUS START (HCFS -> OPERATIONAL)\n", TS_MS);
    trace_usb_ohci_start(ohci->name);

    /* v205: Notify Chihiro to schedule USB device hotplug.
     * Devices attach AFTER the kernel enables RHSC so fresh CSC
     * events trigger full USB enumeration including SET_CONFIG. */
    extern void chihiro_on_ohci_bus_start(void);
    chihiro_on_ohci_bus_start();

    /*
     * Delay the first SOF event by one frame time as linux driver is
     * not ready to receive it and can meet some race conditions
     */
    ohci->sof_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ohci_eof_timer(ohci);

    return 1;
}

/* Stop sending SOF tokens on the bus */
void ohci_bus_stop(OHCIState *ohci)
{
    printf("[%07lld] OHCI BUS STOP\n", TS_MS);
    trace_usb_ohci_stop(ohci->name);
    timer_del(ohci->eof_timer);
}

/* Frame interval toggle is manipulated by the hcd only */
static void ohci_set_frame_interval(OHCIState *ohci, uint16_t val)
{
    val &= OHCI_FMI_FI;

    if (val != ohci->fi) {
        trace_usb_ohci_set_frame_interval(ohci->name, ohci->fi, ohci->fi);
    }

    ohci->fi = val;
}

static void ohci_port_power(OHCIState *ohci, int i, int p)
{
    if (p) {
        ohci->rhport[i].ctrl |= OHCI_PORT_PPS;
    } else {
        ohci->rhport[i].ctrl &= ~(OHCI_PORT_PPS | OHCI_PORT_CCS |
                                  OHCI_PORT_PSS | OHCI_PORT_PRS);
    }
}

/* Set HcControlRegister */
static void ohci_set_ctl(OHCIState *ohci, uint32_t val)
{
    uint32_t old_state;
    uint32_t new_state;

    old_state = ohci->ctl & OHCI_CTL_HCFS;
    ohci->ctl = val;
    new_state = ohci->ctl & OHCI_CTL_HCFS;

    /* no state change */
    if (old_state == new_state) {
        return;
    }
    trace_usb_ohci_set_ctl(ohci->name, new_state);
    if (TS_MS > 5900) {
        if(0) printf("[%07lld] OHCI HcControl: 0x%08X -> 0x%08X (HCFS: %d -> %d) ports:",
               TS_MS, old_state | (ohci->ctl & ~OHCI_CTL_HCFS), val,
               old_state >> 6, new_state >> 6);
        for (int i = 0; i < ohci->num_ports; i++) {
            if(0) printf(" P%d=0x%08X[%s%s]", i, ohci->rhport[i].ctrl,
                   (ohci->rhport[i].ctrl & OHCI_PORT_CCS) ? "CCS" : "",
                   (ohci->rhport[i].ctrl & OHCI_PORT_PES) ? "+PES" : "");
        }
        if(0) printf("\n");
    }
    switch (new_state) {
    case OHCI_USB_OPERATIONAL:
        ohci_bus_start(ohci);
        break;
    case OHCI_USB_SUSPEND:
        ohci_bus_stop(ohci);
        /* clear pending SF otherwise linux driver loops in ohci_irq() */
        ohci->intr_status &= ~OHCI_INTR_SF;
        ohci_intr_update(ohci);
        break;
    case OHCI_USB_RESUME:
        trace_usb_ohci_resume(ohci->name);
        break;
    case OHCI_USB_RESET:
        ohci_roothub_reset(ohci);
        break;
    }
}

static uint32_t ohci_get_frame_remaining(OHCIState *ohci)
{
    uint16_t fr;
    int64_t tks;

    if ((ohci->ctl & OHCI_CTL_HCFS) != OHCI_USB_OPERATIONAL) {
        return ohci->frt << 31;
    }
    /* Being in USB operational state guarantees sof_time was set already. */
    tks = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - ohci->sof_time;
    if (tks < 0) {
        tks = 0;
    }

    /* avoid muldiv if possible */
    if (tks >= usb_frame_time) {
        return ohci->frt << 31;
    }
    tks = tks / usb_bit_time;
    fr = (uint16_t)(ohci->fi - tks);

    return (ohci->frt << 31) | fr;
}


/* Set root hub status */
static void ohci_set_hub_status(OHCIState *ohci, uint32_t val)
{
    uint32_t old_state;

    old_state = ohci->rhstatus;

    /* write 1 to clear OCIC */
    if (val & OHCI_RHS_OCIC) {
        ohci->rhstatus &= ~OHCI_RHS_OCIC;
    }
    if (val & OHCI_RHS_LPS) {
        int i;

        for (i = 0; i < ohci->num_ports; i++) {
            ohci_port_power(ohci, i, 0);
        }
        trace_usb_ohci_hub_power_down();
    }

    if (val & OHCI_RHS_LPSC) {
        int i;

        if (TS_MS > 5900) {
            if(0) printf("[%07lld] OHCI HcRhStatus LPSC: powering on %d ports\n",
                   TS_MS, ohci->num_ports);
        }
        for (i = 0; i < ohci->num_ports; i++) {
            ohci_port_power(ohci, i, 1);
            if (TS_MS > 5900) {
                if(0) printf("[%07lld]   port%d after power: ctrl=0x%08X [%s%s%s]\n",
                       TS_MS, i, ohci->rhport[i].ctrl,
                       (ohci->rhport[i].ctrl & OHCI_PORT_CCS) ? "CCS " : "",
                       (ohci->rhport[i].ctrl & OHCI_PORT_PES) ? "PES " : "",
                       (ohci->rhport[i].ctrl & OHCI_PORT_PPS) ? "PPS" : "");
            }
        }
        trace_usb_ohci_hub_power_up();
    }

    if (val & OHCI_RHS_DRWE) {
        ohci->rhstatus |= OHCI_RHS_DRWE;
    }
    if (val & OHCI_RHS_CRWE) {
        ohci->rhstatus &= ~OHCI_RHS_DRWE;
    }
    if (old_state != ohci->rhstatus) {
        ohci_set_interrupt(ohci, OHCI_INTR_RHSC);
    }
}

/* This is the one state transition the controller can do by itself */
static bool ohci_resume(OHCIState *s)
{
    if ((s->ctl & OHCI_CTL_HCFS) == OHCI_USB_SUSPEND) {
        trace_usb_ohci_remote_wakeup(s->name);
        s->ctl &= ~OHCI_CTL_HCFS;
        s->ctl |= OHCI_USB_RESUME;
        return true;
    }
    return false;
}

/*
 * Sets a flag in a port status reg but only set it if the port is connected.
 * If not set ConnectStatusChange flag. If flag is enabled return 1.
 */
static int ohci_port_set_if_connected(OHCIState *ohci, int i, uint32_t val)
{
    int ret = 1;

    /* writing a 0 has no effect */
    if (val == 0) {
        return 0;
    }
    /* If CurrentConnectStatus is cleared we set ConnectStatusChange */
    if (!(ohci->rhport[i].ctrl & OHCI_PORT_CCS)) {
        ohci->rhport[i].ctrl |= OHCI_PORT_CSC;
        if (ohci->rhstatus & OHCI_RHS_DRWE) {
            /* CSC is a wakeup event */
            if (ohci_resume(ohci)) {
                ohci_set_interrupt(ohci, OHCI_INTR_RD);
            }
        }
        return 0;
    }

    if (ohci->rhport[i].ctrl & val) {
        ret = 0;
    }
    /* set the bit */
    ohci->rhport[i].ctrl |= val;

    return ret;
}

/* Set root hub port status */
static void ohci_port_set_status(OHCIState *ohci, int portnum, uint32_t val)
{
    uint32_t old_state;
    OHCIPort *port;

    port = &ohci->rhport[portnum];
    old_state = port->ctrl;

    if (TS_MS > 5900) {
        if(0) printf("[%07lld] OHCI PORT%d WRITE: val=0x%08X before=0x%08X [%s%s%s%s%s%s]\n", TS_MS,
               portnum, val, old_state,
               (val & (1<<0))  ? "ClearPortEnable " : "",
               (val & (1<<1))  ? "SetPortEnable " : "",
               (val & (1<<4))  ? "SetPortReset " : "",
               (val & (1<<8))  ? "SetPortPower " : "",
               (val & (1<<16)) ? "ClearCSC " : "",
               (val & (1<<20)) ? "ClearPRSC " : "");
    }

    /* Write to clear CSC, PESC, PSSC, OCIC, PRSC */
    if (val & OHCI_PORT_WTC) {
        port->ctrl &= ~(val & OHCI_PORT_WTC);
    }
    if (val & OHCI_PORT_CCS) {
        /* v204: ClearPortEnable NO LONGER blocked for Chihiro.
         * Previously blocked during SEGABOOT phase to keep devices enabled,
         * but this prevented the kernel from completing USB enumeration
         * (GET_DESC→SET_ADDRESS→ClearPortEnable→re-enable→SET_CONFIG).
         * Now allowing normal OHCI port disable/enable flow. */
        port->ctrl &= ~OHCI_PORT_PES;
        port->ctrl |= OHCI_PORT_PESC;
        extern uint32_t chihiro_usb_sm_pa;
        if (0 && TS_MS > 5900 && chihiro_usb_sm_pa != 0) {
            uint8_t sm[4] = {0};
            cpu_physical_memory_read(chihiro_usb_sm_pa, sm, 4);
            if(0) printf("[%07lld]   ClearPE USB-SM: active=%u sub=%u state=%u flags=0x%02X\n",
                   TS_MS, sm[0], sm[1], sm[2], sm[3]);
        }
    }
    ohci_port_set_if_connected(ohci, portnum, val & OHCI_PORT_PES);

    if (ohci_port_set_if_connected(ohci, portnum, val & OHCI_PORT_PSS)) {
        trace_usb_ohci_port_suspend(portnum);
    }

    if (ohci_port_set_if_connected(ohci, portnum, val & OHCI_PORT_PRS)) {
        trace_usb_ohci_port_reset(portnum);
        usb_device_reset(port->port.dev);
        port->ctrl &= ~OHCI_PORT_PRS;
        /* ??? Should this also set OHCI_PORT_PESC. */
        port->ctrl |= OHCI_PORT_PES | OHCI_PORT_PRSC;
    }

    /* Invert order here to ensure in ambiguous case, device is powered up. */
    if (val & OHCI_PORT_LSDA) {
        ohci_port_power(ohci, portnum, 0);
    }
    if (val & OHCI_PORT_PPS) {
        ohci_port_power(ohci, portnum, 1);
    }
    if (old_state != port->ctrl) {
        ohci_set_interrupt(ohci, OHCI_INTR_RHSC);
    }
}

static uint64_t ohci_mem_read(void *opaque,
                              hwaddr addr,
                              unsigned size)
{
    OHCIState *ohci = opaque;
    uint32_t retval;

    /* Only aligned reads are allowed on OHCI */
    if (addr & 3) {
        trace_usb_ohci_mem_read_unaligned(addr);
        return 0xffffffff;
    } else if (addr >= 0x54 && addr < 0x54 + ohci->num_ports * 4) {
        /* HcRhPortStatus */
        retval = ohci->rhport[(addr - 0x54) >> 2].ctrl | OHCI_PORT_PPS;
        trace_usb_ohci_mem_port_read(size, "HcRhPortStatus", (addr - 0x50) >> 2,
                                     addr, addr >> 2, retval);
    } else {
        switch (addr >> 2) {
        case 0: /* HcRevision */
            retval = 0x10;
            break;

        case 1: /* HcControl */
            retval = ohci->ctl;
            break;

        case 2: /* HcCommandStatus */
            retval = ohci->status;
            break;

        case 3: /* HcInterruptStatus */
            retval = ohci->intr_status;
            break;

        case 4: /* HcInterruptEnable */
        case 5: /* HcInterruptDisable */
            retval = ohci->intr;
            break;

        case 6: /* HcHCCA */
            retval = ohci->hcca;
            break;

        case 7: /* HcPeriodCurrentED */
            retval = ohci->per_cur;
            break;

        case 8: /* HcControlHeadED */
            retval = ohci->ctrl_head;
            break;

        case 9: /* HcControlCurrentED */
            retval = ohci->ctrl_cur;
            break;

        case 10: /* HcBulkHeadED */
            retval = ohci->bulk_head;
            break;

        case 11: /* HcBulkCurrentED */
            retval = ohci->bulk_cur;
            break;

        case 12: /* HcDoneHead */
            retval = ohci->done;
            break;

        case 13: /* HcFmInterretval */
            retval = (ohci->fit << 31) | (ohci->fsmps << 16) | (ohci->fi);
            break;

        case 14: /* HcFmRemaining */
            retval = ohci_get_frame_remaining(ohci);
            break;

        case 15: /* HcFmNumber */
            retval = ohci->frame_number;
            break;

        case 16: /* HcPeriodicStart */
            retval = ohci->pstart;
            break;

        case 17: /* HcLSThreshold */
            retval = ohci->lst;
            break;

        case 18: /* HcRhDescriptorA */
            retval = ohci->rhdesc_a;
            break;

        case 19: /* HcRhDescriptorB */
            retval = ohci->rhdesc_b;
            break;

        case 20: /* HcRhStatus */
            retval = ohci->rhstatus;
            break;

        /* PXA27x specific registers */
        case 24: /* HcStatus */
            retval = ohci->hstatus & ohci->hmask;
            break;

        case 25: /* HcHReset */
            retval = ohci->hreset;
            break;

        case 26: /* HcHInterruptEnable */
            retval = ohci->hmask;
            break;

        case 27: /* HcHInterruptTest */
            retval = ohci->htest;
            break;

        default:
            trace_usb_ohci_mem_read_bad_offset(addr);
            retval = 0xffffffff;
        }
        if (addr != 0xc || retval) {
            trace_usb_ohci_mem_read(size, ohci_reg_name(addr), addr, addr >> 2,
                                    retval);
        }
    }

    if (TS_MS > 5900 && addr != 0x38 && addr != 0x3C) {
        if(0) printf("[%07lld] OHCI READ  [0x%03X] %-20s = 0x%08X\n", TS_MS,
               (uint32_t)addr, ohci_reg_name(addr), retval);
    }

    /* v298: disabled — per-access monitoring too expensive */
    if (0 && TS_MS > 7800 && TS_MS < 8100) {
        static uint32_t r_prev_slot = 0xDEAD, r_prev_cdrv = 0xDEAD;
        static uint8_t r_prev_sub = 0xFF;
        uint8_t r_sub = 0;
        cpu_physical_memory_read(0x0F1D0B, &r_sub, 1);
        uint32_t r_ctx = 0;
        cpu_physical_memory_read(0x0F1D18, &r_ctx, 4);
        if (r_ctx != 0) {
            extern uint32_t chihiro_va_to_pa(uint32_t va);
            uint32_t r_pa = chihiro_va_to_pa(r_ctx);
            if (r_pa != 0xFFFFFFFF) {
                uint32_t r_sl = 0, r_cd = 0;
                cpu_physical_memory_read(r_pa + 0x14, &r_sl, 4);
                cpu_physical_memory_read(r_pa + 0x10, &r_cd, 4);
                if (r_sl != r_prev_slot || r_cd != r_prev_cdrv || r_sub != r_prev_sub) {
                    if(0) printf("[%07lld] v298-FINE: slot=0x%X→0x%X cdrv=0x%X→0x%X sub=%d→%d @READ[0x%03X]\n",
                           TS_MS, r_prev_slot, r_sl, r_prev_cdrv, r_cd, r_prev_sub, r_sub, (uint32_t)addr);
                    r_prev_slot = r_sl;
                    r_prev_cdrv = r_cd;
                    r_prev_sub = r_sub;
                }
            }
        }
    }

    return retval;
}

static void ohci_mem_write(void *opaque,
                           hwaddr addr,
                           uint64_t val,
                           unsigned size)
{
    OHCIState *ohci = opaque;

    /* Only aligned reads are allowed on OHCI */
    if (addr & 3) {
        trace_usb_ohci_mem_write_unaligned(addr);
        return;
    }

    if (TS_MS > 5900) {
        if(0) printf("[%07lld] OHCI WRITE [0x%03X] %-20s = 0x%08X\n", TS_MS,
               (uint32_t)addr, ohci_reg_name(addr), (uint32_t)val);
    }

    /* v298: disabled — per-access monitoring too expensive */
    if (0 && TS_MS > 7800 && TS_MS < 8100) {
        static uint32_t w_prev_slot = 0xDEAD, w_prev_cdrv = 0xDEAD;
        static uint8_t w_prev_sub = 0xFF;
        uint8_t w_sub = 0;
        cpu_physical_memory_read(0x0F1D0B, &w_sub, 1);
        uint32_t w_ctx = 0;
        cpu_physical_memory_read(0x0F1D18, &w_ctx, 4);
        if (w_ctx != 0) {
            extern uint32_t chihiro_va_to_pa(uint32_t va);
            uint32_t w_pa = chihiro_va_to_pa(w_ctx);
            if (w_pa != 0xFFFFFFFF) {
                uint32_t w_sl = 0, w_cd = 0;
                cpu_physical_memory_read(w_pa + 0x14, &w_sl, 4);
                cpu_physical_memory_read(w_pa + 0x10, &w_cd, 4);
                if (w_sl != w_prev_slot || w_cd != w_prev_cdrv || w_sub != w_prev_sub) {
                    if(0) printf("[%07lld] v298-FINE: slot=0x%X→0x%X cdrv=0x%X→0x%X sub=%d→%d @WRITE[0x%03X]\n",
                           TS_MS, w_prev_slot, w_sl, w_prev_cdrv, w_cd, w_prev_sub, w_sub, (uint32_t)addr);
                    w_prev_slot = w_sl;
                    w_prev_cdrv = w_cd;
                    w_prev_sub = w_sub;
                }
            }
        }
    }

    if (addr >= 0x54 && addr < 0x54 + ohci->num_ports * 4) {
        /* HcRhPortStatus */
        trace_usb_ohci_mem_port_write(size, "HcRhPortStatus",
                                      (addr - 0x50) >> 2, addr, addr >> 2, val);
        ohci_port_set_status(ohci, (addr - 0x54) >> 2, val);
        return;
    }

    trace_usb_ohci_mem_write(size, ohci_reg_name(addr), addr, addr >> 2, val);
    switch (addr >> 2) {
    case 1: /* HcControl */
        ohci_set_ctl(ohci, val);
        break;

    case 2: /* HcCommandStatus */
        if (TS_MS > 5900 && (val & 0x06))
            if(0) printf("[%07lld] OHCI HcCommandStatus: val=0x%08X [%s%s] ctrl_head=0x%08X bulk_head=0x%08X\n",
                   TS_MS, val,
                   (val & 0x02) ? "CLF " : "", (val & 0x04) ? "BLF " : "",
                   ohci->ctrl_head, ohci->bulk_head);
        /* SOC is read-only */
        val = (val & ~OHCI_STATUS_SOC);

        /* Bits written as '0' remain unchanged in the register */
        ohci->status |= val;

        if (ohci->status & OHCI_STATUS_HCR) {
            ohci_soft_reset(ohci);
        }
        break;

    case 3: /* HcInterruptStatus */
        if (0 && TS_MS > 5900 && (val & OHCI_INTR_WD)) {
            if(0) printf("[%07lld] OHCI WDH ACK: guest clears WDH (val=0x%08lX)\n",
                   TS_MS, (unsigned long)val);
            extern uint32_t chihiro_usb_sm_pa;
            if (chihiro_usb_sm_pa != 0) {
                uint8_t sm[4] = {0};
                uint8_t sm88 = 0;
                uint32_t sm8c = 0, sm90 = 0;
                cpu_physical_memory_read(chihiro_usb_sm_pa, sm, 4);
                cpu_physical_memory_read(chihiro_usb_sm_pa + 0x78, &sm88, 1);
                cpu_physical_memory_read(chihiro_usb_sm_pa + 0x7C, &sm8c, 4);
                cpu_physical_memory_read(chihiro_usb_sm_pa + 0x80, &sm90, 4);
                if(0) printf("[%07lld]   USB-SM: active=%u sub=%u state=%u "
                       "flags=0x%02X cnt=%u queue=0x%08X req=0x%08X\n",
                       TS_MS, sm[0], sm[1], sm[2], sm[3],
                       sm88, sm8c, sm90);
            }
        }
        ohci->intr_status &= ~val;
        ohci_intr_update(ohci);
        break;

    case 4: /* HcInterruptEnable */
        ohci->intr |= val;
        ohci_intr_update(ohci);
        break;

    case 5: /* HcInterruptDisable */
        ohci->intr &= ~val;
        ohci_intr_update(ohci);
        break;

    case 6: /* HcHCCA */
        ohci->hcca = val & OHCI_HCCA_MASK;
        break;

    case 7: /* HcPeriodCurrentED */
        /* Ignore writes to this read-only register, Linux does them */
        break;

    case 8: /* HcControlHeadED */
        {
            uint32_t new_head = val & OHCI_EDPTR_MASK;
            ohci->ctrl_head = new_head;
        }
        break;

    case 9: /* HcControlCurrentED */
        ohci->ctrl_cur = val & OHCI_EDPTR_MASK;
        break;

    case 10: /* HcBulkHeadED */
        {
            uint32_t new_bhead = val & OHCI_EDPTR_MASK;
            ohci->bulk_head = new_bhead;
        }
        break;

    case 11: /* HcBulkCurrentED */
        ohci->bulk_cur = val & OHCI_EDPTR_MASK;
        break;

    case 13: /* HcFmInterval */
        ohci->fsmps = (val & OHCI_FMI_FSMPS) >> 16;
        ohci->fit = (val & OHCI_FMI_FIT) >> 31;
        ohci_set_frame_interval(ohci, val);
        break;

    case 15: /* HcFmNumber */
        break;

    case 16: /* HcPeriodicStart */
        ohci->pstart = val & 0xffff;
        break;

    case 17: /* HcLSThreshold */
        ohci->lst = val & 0xffff;
        break;

    case 18: /* HcRhDescriptorA */
        ohci->rhdesc_a &= ~OHCI_RHA_RW_MASK;
        ohci->rhdesc_a |= val & OHCI_RHA_RW_MASK;
        break;

    case 19: /* HcRhDescriptorB */
        break;

    case 20: /* HcRhStatus */
        ohci_set_hub_status(ohci, val);
        break;

    /* PXA27x specific registers */
    case 24: /* HcStatus */
        ohci->hstatus &= ~(val & ohci->hmask);
        break;

    case 25: /* HcHReset */
        ohci->hreset = val & ~OHCI_HRESET_FSBIR;
        if (val & OHCI_HRESET_FSBIR) {
            ohci_hard_reset(ohci);
        }
        break;

    case 26: /* HcHInterruptEnable */
        ohci->hmask = val;
        break;

    case 27: /* HcHInterruptTest */
        ohci->htest = val;
        break;

    default:
        trace_usb_ohci_mem_write_bad_offset(addr);
        break;
    }
}

static const MemoryRegionOps ohci_mem_ops = {
    .read = ohci_mem_read,
    .write = ohci_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* USBPortOps */
static void ohci_attach(USBPort *port1)
{
    OHCIState *s = port1->opaque;
    OHCIPort *port = &s->rhport[port1->index];
    uint32_t old_state = port->ctrl;

    printf("[%07lld] OHCI ATTACH: port%d device=%s\n", TS_MS,
           port1->index,
           port1->dev ? port1->dev->product_desc : "NULL");

    /* set connect status */
    port->ctrl |= OHCI_PORT_CCS | OHCI_PORT_CSC;

    /* update speed */
    if (port->port.dev->speed == USB_SPEED_LOW) {
        port->ctrl |= OHCI_PORT_LSDA;
    } else {
        port->ctrl &= ~OHCI_PORT_LSDA;
    }

    /* notify of remote-wakeup */
    if ((s->ctl & OHCI_CTL_HCFS) == OHCI_USB_SUSPEND) {
        ohci_set_interrupt(s, OHCI_INTR_RD);
    }

    trace_usb_ohci_port_attach(port1->index);

    if (old_state != port->ctrl) {
        ohci_set_interrupt(s, OHCI_INTR_RHSC);
    }
}

static void ohci_child_detach(USBPort *port1, USBDevice *dev)
{
    OHCIState *ohci = port1->opaque;

    if (ohci->async_td &&
        usb_packet_is_inflight(&ohci->usb_packet) &&
        ohci->usb_packet.ep->dev == dev) {
        usb_cancel_packet(&ohci->usb_packet);
        ohci->async_td = 0;
    }
}

static void ohci_detach(USBPort *port1)
{
    OHCIState *s = port1->opaque;
    OHCIPort *port = &s->rhport[port1->index];
    uint32_t old_state = port->ctrl;

    printf("[%07lld] OHCI DETACH: port%d\n", TS_MS, port1->index);

    ohci_child_detach(port1, port1->dev);

    /* set connect status */
    if (port->ctrl & OHCI_PORT_CCS) {
        port->ctrl &= ~OHCI_PORT_CCS;
        port->ctrl |= OHCI_PORT_CSC;
    }
    /* disable port */
    if (port->ctrl & OHCI_PORT_PES) {
        port->ctrl &= ~OHCI_PORT_PES;
        port->ctrl |= OHCI_PORT_PESC;
    }
    trace_usb_ohci_port_detach(port1->index);

    if (old_state != port->ctrl) {
        ohci_set_interrupt(s, OHCI_INTR_RHSC);
    }
}

static void ohci_wakeup(USBPort *port1)
{
    OHCIState *s = port1->opaque;
    OHCIPort *port = &s->rhport[port1->index];
    uint32_t intr = 0;
    if (port->ctrl & OHCI_PORT_PSS) {
        trace_usb_ohci_port_wakeup(port1->index);
        port->ctrl |= OHCI_PORT_PSSC;
        port->ctrl &= ~OHCI_PORT_PSS;
        intr = OHCI_INTR_RHSC;
    }
    /* Note that the controller can be suspended even if this port is not */
    if (ohci_resume(s)) {
        /*
         * In suspend mode only ResumeDetected is possible, not RHSC:
         * see the OHCI spec 5.1.2.3.
         */
        intr = OHCI_INTR_RD;
    }
    ohci_set_interrupt(s, intr);
}

static void ohci_async_complete_packet(USBPort *port, USBPacket *packet)
{
    OHCIState *ohci = container_of(packet, OHCIState, usb_packet);

    trace_usb_ohci_async_complete();
    ohci->async_complete = true;
    ohci_process_lists(ohci);
}

static USBPortOps ohci_port_ops = {
    .attach = ohci_attach,
    .detach = ohci_detach,
    .child_detach = ohci_child_detach,
    .wakeup = ohci_wakeup,
    .complete = ohci_async_complete_packet,
};

static USBBusOps ohci_bus_ops = {
};

void usb_ohci_init(OHCIState *ohci, DeviceState *dev, uint32_t num_ports,
                   dma_addr_t localmem_base, char *masterbus,
                   uint32_t firstport, AddressSpace *as,
                   void (*ohci_die_fn)(OHCIState *), Error **errp)
{
    Error *err = NULL;
    int i;

    ohci->as = as;
    ohci->ohci_die = ohci_die_fn;

    if (num_ports > OHCI_MAX_PORTS) {
        error_setg(errp, "OHCI num-ports=%u is too big (limit is %u ports)",
                   num_ports, OHCI_MAX_PORTS);
        return;
    }

    if (usb_frame_time == 0) {
#ifdef OHCI_TIME_WARP
        usb_frame_time = NANOSECONDS_PER_SECOND;
        usb_bit_time = NANOSECONDS_PER_SECOND / (USB_HZ / 1000);
#else
        usb_frame_time = NANOSECONDS_PER_SECOND / 1000;
        if (NANOSECONDS_PER_SECOND >= USB_HZ) {
            usb_bit_time = NANOSECONDS_PER_SECOND / USB_HZ;
        } else {
            usb_bit_time = 1;
        }
#endif
        trace_usb_ohci_init_time(usb_frame_time, usb_bit_time);
    }

    ohci->num_ports = num_ports;
    if (masterbus) {
        USBPort *ports[OHCI_MAX_PORTS];
        for (i = 0; i < num_ports; i++) {
            ports[i] = &ohci->rhport[i].port;
        }
        usb_register_companion(masterbus, ports, num_ports,
                               firstport, ohci, &ohci_port_ops,
                               USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL,
                               &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }
    } else {
        usb_bus_new(&ohci->bus, sizeof(ohci->bus), &ohci_bus_ops, dev);
        for (i = 0; i < num_ports; i++) {
            usb_register_port(&ohci->bus, &ohci->rhport[i].port,
                              ohci, i, &ohci_port_ops,
                              USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL);
        }
    }

    memory_region_init_io(&ohci->mem, OBJECT(dev), &ohci_mem_ops,
                          ohci, "ohci", 256);
    ohci->localmem_base = localmem_base;

    ohci->name = object_get_typename(OBJECT(dev));
    usb_packet_init(&ohci->usb_packet);

    ohci->async_td = 0;

    ohci->eof_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   ohci_frame_boundary, ohci);
}

/*
 * A typical OHCI will stop operating and set itself into error state
 * (which can be queried by MMIO) to signal that it got an error.
 */
void ohci_sysbus_die(struct OHCIState *ohci)
{
    trace_usb_ohci_die();

    ohci_set_interrupt(ohci, OHCI_INTR_UE);
    ohci_bus_stop(ohci);
}

static const VMStateDescription vmstate_ohci_state_port = {
    .name = "ohci-core/port",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, OHCIPort),
        VMSTATE_END_OF_LIST()
    },
};

static bool ohci_eof_timer_needed(void *opaque)
{
    OHCIState *ohci = opaque;

    return timer_pending(ohci->eof_timer);
}

static const VMStateDescription vmstate_ohci_eof_timer = {
    .name = "ohci-core/eof-timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = ohci_eof_timer_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(eof_timer, OHCIState),
        VMSTATE_END_OF_LIST()
    },
};

const VMStateDescription vmstate_ohci_state = {
    .name = "ohci-core",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(sof_time, OHCIState),
        VMSTATE_UINT32(ctl, OHCIState),
        VMSTATE_UINT32(status, OHCIState),
        VMSTATE_UINT32(intr_status, OHCIState),
        VMSTATE_UINT32(intr, OHCIState),
        VMSTATE_UINT32(hcca, OHCIState),
        VMSTATE_UINT32(ctrl_head, OHCIState),
        VMSTATE_UINT32(ctrl_cur, OHCIState),
        VMSTATE_UINT32(bulk_head, OHCIState),
        VMSTATE_UINT32(bulk_cur, OHCIState),
        VMSTATE_UINT32(per_cur, OHCIState),
        VMSTATE_UINT32(done, OHCIState),
        VMSTATE_INT32(done_count, OHCIState),
        VMSTATE_UINT16(fsmps, OHCIState),
        VMSTATE_UINT8(fit, OHCIState),
        VMSTATE_UINT16(fi, OHCIState),
        VMSTATE_UINT8(frt, OHCIState),
        VMSTATE_UINT16(frame_number, OHCIState),
        VMSTATE_UINT16(padding, OHCIState),
        VMSTATE_UINT32(pstart, OHCIState),
        VMSTATE_UINT32(lst, OHCIState),
        VMSTATE_UINT32(rhdesc_a, OHCIState),
        VMSTATE_UINT32(rhdesc_b, OHCIState),
        VMSTATE_UINT32(rhstatus, OHCIState),
        VMSTATE_STRUCT_ARRAY(rhport, OHCIState, OHCI_MAX_PORTS, 0,
                             vmstate_ohci_state_port, OHCIPort),
        VMSTATE_UINT32(hstatus, OHCIState),
        VMSTATE_UINT32(hmask, OHCIState),
        VMSTATE_UINT32(hreset, OHCIState),
        VMSTATE_UINT32(htest, OHCIState),
        VMSTATE_UINT32(old_ctl, OHCIState),
        VMSTATE_UINT8_ARRAY(usb_buf, OHCIState, 8192),
        VMSTATE_UINT32(async_td, OHCIState),
        VMSTATE_BOOL(async_complete, OHCIState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_ohci_eof_timer,
        NULL
    }
};
