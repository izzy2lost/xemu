/*
 * hakuX native core — a Chihiro's Type-1 media board (xemu's chihiro.c, the
 * LPC device and chihiro_mbcom_process).
 *
 * The board answers on the LPC ports 0x4000-0x40FF and through a mailbox two
 * sectors of the DIMM's system area, which the title opens as \??\mbcom:
 * (or reaches in the raw disk, partition 0): it writes a command to the command sector, the board answers
 * in the response sector and raises IRQ 10 with bit 0 of port 0x40E0, which
 * the title's DPC clears by writing the bit back. Ghost Squad refuses to run
 * without a board ('!' Type-1 or ')' Type-3, vsg.xbe 0x14c4e0); the others
 * run without one. The FPGA board (Type-1) has no V850 to run: the commands
 * are answered here, as xemu answers them.
 */
#include "native/core/irq.h"
#include "native/core/nc.h"
#include "native/core/vm.h"
#include "native/dev/usb.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAG "hakuX-native-chihiro"

#define MB_IRQ 10u
/* JP1/JP2: a 512 MB DIMM, which holds every game here but Gundam's. */
#define DIMM_FACTOR 2u
#define DIMM_SIZE   (0x08000000u << DIMM_FACTOR)
/* The mailbox: 0x8000 sectors below the DIMM's end, response then command. */
#define MBCOM_BASE  ((0x40000u << DIMM_FACTOR) - 0x8000u)
/* In it ("mbcom:", the title's \??\mbcom:), the response and command sectors. */
#define MBCOM_RESP  (0x4800ull * 512u)
#define MBCOM_CMD   (0x4801ull * 512u)

enum {
    MB_CMD_INIT = 0x0001,             /* the DIMM's size */
    MB_CMD_STATUS = 0x0100,           /* boot phase, percent done */
    MB_CMD_GET_VERSION = 0x0101,
    MB_CMD_SYSTEM_TYPE = 0x0102,      /* board type | firmware << 8 */
    MB_CMD_GET_SERIAL = 0x0103,
    MB_CMD_GET_NET_PROPERTY = 0x0104,
    MB_CMD_HARDWARE_TEST = 0x0301,    /* "TEST OK" at the pointer given */
};
#define MB_STATUS_READY 5u
#define MB_FW_VERSION   0x0317u
#define MB_BOARD_TYPE   4u            /* Chihiro */
#define MB_NET_IP       0x0100000Au   /* 10.0.0.1, cabinet 1 */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_e0;                  /* 0x40E0: bit 0, a message waits (IRQ 10) */
static uint16_t g_scratch;            /* 0x4026 */
/* 0x401E: SEGABOOT's first read is the DIMM base, the game's the "XBAM" it
 * checks; there is no SEGABOOT here, so its read is counted as done. */
static unsigned g_base_reads = 1;
static uint8_t g_resp[32];

/* A message for the title: bit 0 of 0x40E0, and a rise of IRQ 10, an edge
 * (xemu pulses it: lower, raise). */
static void raise_message(void)
{
    g_e0 |= 0x01;
    nc_irq_set_edge(MB_IRQ, true);
    nc_irq_set(MB_IRQ, false);
    nc_irq_set(MB_IRQ, true);
}

bool nc_mediaboard_port_in(uint16_t port, uint32_t *v)
{
    if (!nc_chihiro_enabled() || (port & 0xFF00u) != 0x4000u) {
        return false;
    }
    pthread_mutex_lock(&g_lock);
    switch (port & 0xFFu) {
    case 0x1E:  /* the DIMM base for SEGABOOT, then "XB" of the "XBAM" games check (HOTD3) */
        *v = g_base_reads++ ? 0x4258u : 0x0000u;
        break;
    case 0x20:
        *v = g_base_reads > 1 ? 0x4D41u : 0x0100u;
        break;
    case 0x22:
        *v = 0x4258u;
        break;
    case 0x24:
        *v = 0x4D41u;
        break;
    case 0x26:
        *v = g_scratch;
        break;
    case 0xE0:
        *v = g_e0;
        break;
    case 0xF0:  /* chip revision: FPGA (Type-1) revision 1 */
        *v = 0x0001u;
        break;
    case 0xF4:  /* the DIMM size jumpers */
        *v = DIMM_FACTOR;
        break;
    default:
        *v = 0;
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return true;
}

bool nc_mediaboard_port_out(uint16_t port, uint32_t value)
{
    if (!nc_chihiro_enabled() || (port & 0xFF00u) != 0x4000u) {
        return false;
    }
    pthread_mutex_lock(&g_lock);
    switch (port & 0xFFu) {
    case 0x26:
        g_scratch = (uint16_t)value;
        break;
    case 0xE0:  /* the bits written are taken */
        g_e0 &= (uint8_t)~value;
        if (!g_e0) {
            nc_irq_set(MB_IRQ, false);
        }
        break;
    case 0xE1:  /* the trigger: 0 lowers IRQ 10 (busy bit of the scratch cleared) */
    case 0xE2:
        if (!value || (port & 0xFFu) == 0xE2) {
            g_scratch &= (uint16_t)~0x0100u;
            nc_irq_set(MB_IRQ, false);
        }
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return true;
}

/* chihiro_mbcom_process: the board's answer to command w, in r. */
static void process(const uint8_t *w, uint8_t *r, NcVm *vm)
{
    const uint16_t cmd = (uint16_t)(w[2] | w[3] << 8);
    memset(r, 0, 32);
    r[0] = w[0];
    r[1] = w[1];
    r[2] = w[2];
    r[3] = (uint8_t)((w[3] & 0x7F) | 0x80);
    uint32_t u;
    switch (cmd) {
    case MB_CMD_INIT:
        u = DIMM_SIZE;
        memcpy(r + 4, &u, 4);
        break;
    case MB_CMD_STATUS:
        r[4] = MB_STATUS_READY;
        r[8] = 100;
        break;
    case MB_CMD_GET_VERSION:
        r[4] = MB_FW_VERSION & 0xFF;
        r[5] = MB_FW_VERSION >> 8;
        break;
    case MB_CMD_SYSTEM_TYPE:
        r[4] = MB_BOARD_TYPE;
        r[5] = MB_FW_VERSION & 0xFF;
        r[6] = MB_FW_VERSION >> 8;
        break;
    case MB_CMD_GET_SERIAL:     /* no flash: a zero serial */
    case MB_CMD_GET_NET_PROPERTY:
    case 0x0204:
    case 0x0601:
    case 0x0605:
    case 0x0606:
    case 0x0607:
        break;
    case MB_CMD_HARDWARE_TEST: {
        memcpy(r + 4, w + 4, 4);
        const uint32_t p = (uint32_t)(w[8] | w[9] << 8 | w[10] << 16 | (uint32_t)w[11] << 24);
        if (p >= 0x80000000u && nc_vm_phys(vm, p) != UINT32_MAX && nc_vm_phys(vm, p + 7) != UINT32_MAX) {
            nc_vm_will_write(p, 8);
            memcpy(nc_vm_host(vm, p), "TEST OK", 8);
        }
        break;
    }
    case 0x0602:                /* 0xFFFF: the title asks 0x0605 next */
        r[4] = r[5] = 0xFF;
        break;
    case 0x0415:
    case 0x0608:
        u = MB_NET_IP;
        memcpy(r + 4, &u, 4);
        break;
    default: {
        static unsigned said;
        if (said++ < 16) {
            nc_log(TAG, "media board: command %04x not known, answered empty", cmd);
        }
        break;
    }
    }
}

/* The DIMM's system area outside the mailbox ("mbsys:"): games keep records
 * there (OutRun 2 SP's ranking), battery-backed on the cabinet. Held here,
 * and written through to the save's .dimm beside it. */
#define MBSYS_SIZE (0x8000u * 512u)
static uint8_t *g_mbsys;
static int g_mbsys_fd = -2;

static void mbsys_open(void)
{
    if (g_mbsys) {
        return;
    }
    g_mbsys = calloc(1, MBSYS_SIZE);
    const char *save = getenv("NC_CHIHIRO_SAVE");
    if (!g_mbsys || !save || !*save) {
        g_mbsys_fd = -1;
        return;
    }
    char p[600];
    snprintf(p, sizeof(p), "%s.dimm", save);
    g_mbsys_fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0664);
    if (g_mbsys_fd >= 0 && pread(g_mbsys_fd, g_mbsys, MBSYS_SIZE, 0) > 0) {
        nc_log(TAG, "media board: the DIMM's system area from %s", p);
    }
}

static bool mailbox(NcVm *vm, uint64_t off, uint8_t *buf, uint32_t len, bool write)
{
    if (!write) {
        memset(buf, 0, len);
        if (off == MBCOM_RESP) {
            memcpy(buf, g_resp, sizeof(g_resp));
        }
    } else if (off == MBCOM_CMD && (buf[0] || buf[1])) {
        static unsigned said;
        if (said++ < 24) {
            nc_log(TAG, "media board: command %04x (seq %04x)", buf[2] | buf[3] << 8, buf[0] | buf[1] << 8);
        }
        process(buf, g_resp, vm);
        raise_message();
    }
    return true;
}

bool nc_mediaboard_mbcom(NcVm *vm, uint64_t off, uint8_t *buf, uint32_t len, bool write)
{
    if (!nc_chihiro_enabled()) {
        return false;
    }
    pthread_mutex_lock(&g_lock);
    if ((off == MBCOM_RESP || off == MBCOM_CMD) && len >= 32) {
        mailbox(vm, off, buf, len, write);
    } else {
        mbsys_open();
        const uint32_t n = !g_mbsys || off >= MBSYS_SIZE ? 0 : (uint32_t)(MBSYS_SIZE - off < len ? MBSYS_SIZE - off : len);
        if (!write) {
            memset(buf, 0, len);
            if (n) {
                memcpy(buf, g_mbsys + off, n);
            }
        } else if (n) {
            memcpy(g_mbsys + off, buf, n);
            if (g_mbsys_fd >= 0 && pwrite(g_mbsys_fd, buf, n, (off_t)off) != (ssize_t)n) {
                nc_log(TAG, "media board: the DIMM's system area couldn't be saved");
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return true;
}

bool nc_mediaboard_raw(NcVm *vm, uint64_t off, uint8_t *buf, uint32_t len, bool write)
{
    const uint64_t base = (uint64_t)MBCOM_BASE * 512u;
    if (!nc_chihiro_enabled() || off < base || off >= base + MBSYS_SIZE) {
        return false;
    }
    return nc_mediaboard_mbcom(vm, off - base, buf, len, write);
}
