/*
 * Chihiro media board — the DIMM SPD EEPROMs and the I2C the firmware
 * bit-bangs to reach them
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
#include "qemu/osdep.h"
#include "chihiro-spd.h"

/* A serial EEPROM on an I2C bus: seven address bits, then read or write. The
 * SPD family sits at 0x50, one address per socket. */
#define SPD_I2C_BASE 0x50

enum {
    SPD_IDLE,        /* between a stop and the next start */
    SPD_ADDR,        /* shifting in the seven address bits and the direction */
    SPD_ADDR_ACK,
    SPD_PTR,         /* shifting in the byte pointer */
    SPD_PTR_ACK,
    SPD_READ,        /* shifting a byte out */
    SPD_READ_ACK,    /* reading the master's acknowledge */
    SPD_WRITE,       /* shifting a byte in — an SPD is read-only to us */
    SPD_WRITE_ACK
};

/* ------------------------------------------------------------------ content */

/* A JEDEC SPD for an unbuffered SDRAM DIMM; the firmware reads only bytes 31,
 * 5 and 63 (chihiro-spd.h). */
static void spd_fill(uint8_t *p, unsigned mb)
{
    unsigned sum = 0;

    memset(p, 0, 256);
    p[0]  = 0x80;               /* bytes written by the manufacturer */
    p[1]  = 0x08;               /* 256 bytes of EEPROM */
    p[2]  = 0x04;               /* SDRAM */
    p[3]  = 0x0C;               /* row addresses */
    p[4]  = 0x0A;               /* column addresses */
    p[5]  = 0x01;               /* one bank of chips: single sided */
    p[6]  = 0x40;               /* 64 data bits */
    p[7]  = 0x00;
    p[8]  = 0x01;               /* LVTTL */
    p[9]  = 0x75;               /* 7.5 ns cycle at the highest CAS latency */
    p[10] = 0x54;               /* 5.4 ns access time */
    p[11] = 0x00;               /* non-parity, non-ECC */
    p[12] = 0x82;               /* refresh 15.625 us, self refresh */
    p[13] = 0x08;               /* 8 bits wide per chip */
    p[15] = 0x01;               /* one clock of write recovery */
    p[16] = 0x0F;               /* burst lengths 1, 2, 4 and 8 */
    p[17] = 0x04;               /* four internal banks */
    p[18] = 0x0C;               /* CAS latencies 2 and 3 */
    p[19] = 0x01;               /* CS latency 0 */
    p[20] = 0x01;               /* write latency 0 */
    p[21] = 0x00;               /* unbuffered: bits 1 and 4 must agree */
    p[22] = 0x0E;               /* auto precharge, precharge all, write1/read */
    p[23] = 0x75;               /* 7.5 ns at one latency down */
    p[24] = 0x54;               /* its access time */
    p[25] = 0x75;
    p[26] = 0x54;
    p[27] = 0x14;               /* tRP 20 ns */
    p[28] = 0x14;               /* tRRD */
    p[29] = 0x14;               /* tRCD */
    p[30] = 0x2D;               /* tRAS 45 ns */
    p[32] = 0x20;               /* 2.0 ns set-up */
    p[33] = 0x10;               /* 1.0 ns hold */
    p[34] = 0x20;
    p[35] = 0x10;
    p[62] = 0x12;               /* SPD revision 1.2 */

    /* Byte 31 is the module bank density, one bit per size: bit 0 is 4 MB and
     * each bit up doubles it, with bit 7 meaning 512 MB. This is the byte the
     * firmware turns into the DIMM size it reports to the host. */
    if (mb >= 512)      p[31] = 0x80;
    else if (mb >= 256) p[31] = 0x40;
    else if (mb >= 128) p[31] = 0x20;
    else                p[31] = 0x10;

    for (int i = 0; i < 63; i++) sum += p[i];
    p[63] = (uint8_t)sum;       /* the checksum the firmware verifies */
}

void chihiro_spd_init(ChihiroSpd *s, unsigned slot0_mb, unsigned slot1_mb)
{
    memset(s, 0, sizeof(*s));
    s->scl = true;
    s->sda = true;
    s->slot = -1;
    s->state = SPD_IDLE;
    if (slot0_mb) { spd_fill(s->rom[0], slot0_mb); s->present[0] = true; }
    if (slot1_mb) { spd_fill(s->rom[1], slot1_mb); s->present[1] = true; }
}

/* ------------------------------------------------------------------ the bus */

static bool spd_sda_line(const ChihiroSpd *s)
{
    if (s->master_drives && !s->master_level) return false;  /* master pulls */
    if (s->slave_pulls) return false;                        /* we pull */
    return true;                                             /* the pull-up */
}

/* Called on the falling edge of SCL, where a slave is allowed to move SDA. */
static void spd_drive(ChihiroSpd *s)
{
    switch (s->state) {
    case SPD_ADDR:
        if (s->bit < 8) break;
        s->slot = -1;
        if ((s->shift >> 1) >= SPD_I2C_BASE
            && (s->shift >> 1) < SPD_I2C_BASE + CHIHIRO_SPD_SLOTS) {
            int n = (s->shift >> 1) - SPD_I2C_BASE;
            if (s->present[n]) s->slot = n;
        }
        s->slave_pulls = (s->slot >= 0);          /* acknowledge, or stay quiet */
        s->state = SPD_ADDR_ACK;
        break;

    case SPD_ADDR_ACK:
        s->slave_pulls = false;
        s->bit = 0;
        if (s->slot < 0) {
            s->state = SPD_IDLE;                  /* not for us */
        } else if (s->shift & 1) {                /* a read */
            s->out = s->rom[s->slot][s->ptr];
            s->state = SPD_READ;
            s->slave_pulls = !(s->out & 0x80);
            s->bit = 1;
        } else {
            s->state = SPD_PTR;
        }
        break;

    case SPD_PTR:
        if (s->bit < 8) break;
        s->ptr = s->shift;
        s->slave_pulls = true;                    /* acknowledge */
        s->state = SPD_PTR_ACK;
        break;

    case SPD_PTR_ACK:
        s->slave_pulls = false;
        s->bit = 0;
        s->state = SPD_WRITE;                     /* a write we will swallow */
        break;

    case SPD_READ:
        if (s->bit >= 8) {
            s->slave_pulls = false;               /* let the master answer */
            s->state = SPD_READ_ACK;
            break;
        }
        s->slave_pulls = !((s->out << s->bit) & 0x80);
        s->bit++;
        break;

    case SPD_READ_ACK:
        /* The bit sampled on the last rising edge said whether to go on;
         * with no slot addressed there is nothing to go on with. */
        if ((s->shift & 1) || s->slot < 0) {      /* not acknowledged: done */
            s->state = SPD_IDLE;
            break;
        }
        s->ptr++;
        s->out = s->rom[s->slot][s->ptr];
        s->bit = 1;
        s->slave_pulls = !(s->out & 0x80);
        s->state = SPD_READ;
        break;

    case SPD_WRITE:
        if (s->bit < 8) break;
        s->slave_pulls = true;                    /* acknowledge and drop it */
        s->state = SPD_WRITE_ACK;
        break;

    case SPD_WRITE_ACK:
        s->slave_pulls = false;
        s->bit = 0;
        s->state = SPD_WRITE;
        break;

    default:
        break;
    }
}

/* Called on the rising edge of SCL, where the data is valid. */
static void spd_sample(ChihiroSpd *s, bool sda)
{
    switch (s->state) {
    case SPD_ADDR:
    case SPD_PTR:
    case SPD_WRITE:
        s->shift = (uint8_t)((s->shift << 1) | (sda ? 1 : 0));
        s->bit++;
        break;
    case SPD_READ_ACK:
        s->shift = sda ? 1 : 0;                   /* 1 is a NAK */
        break;
    default:
        break;
    }
}

static void spd_step(ChihiroSpd *s)
{
    bool scl = s->scl_out;
    bool sda = spd_sda_line(s);

    if (s->scl && scl) {                          /* SCL stayed high */
        if (s->sda && !sda) {                     /* start */
            s->state = SPD_ADDR;
            s->bit = 0;
            s->shift = 0;
            s->slave_pulls = false;
        } else if (!s->sda && sda) {              /* stop */
            s->state = SPD_IDLE;
            s->slave_pulls = false;
        }
    } else if (!s->scl && scl) {
        spd_sample(s, sda);
    } else if (s->scl && !scl) {
        spd_drive(s);
    }
    s->scl = scl;
    s->sda = spd_sda_line(s);
}

uint32_t chihiro_spd_read_lines(ChihiroSpd *s)
{
    return (s->scl_out ? 1u : 0u) | (spd_sda_line(s) ? 2u : 0u);
}

void chihiro_spd_write_lines(ChihiroSpd *s, uint32_t val)
{
    s->scl_out = (val & 1) != 0;
    s->master_level = (val & 2) != 0;
    spd_step(s);
}

void chihiro_spd_write_enable(ChihiroSpd *s, uint32_t val)
{
    s->master_drives = (val & 2) != 0;
    spd_step(s);
}
