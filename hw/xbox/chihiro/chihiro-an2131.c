/*
 * Cypress AN2131 (EZ-USB) register mapping for Chihiro baseboard.
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
 *
 * Maps XDATA-space USB registers, I2C bus master, and port I/O
 * to the generic 8051 CPU core. Runs the ic10/pc20 firmware natively
 * to handle USB vendor requests, EEPROM I2C, JVS relay and the backup
 * records (ACBU0001, Maximum Tune's SBHQ0000) without per-game HLE.
 *
 * Register addresses from Cypress EZ-USB TRM v1.9/1.10.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "chihiro-an2131.h"
#include "chihiro-jvs.h"
#include "chihiro-driveboard-sega838.h"
#include "chihiro-driveboard-v257.h"
#include "chihiro-cardreader-crp1231.h"
#include "chihiro.h"
#include "chihiro-cardreader-hw210.h"
#include "chihiro-cabinet.h"
#include "chihiro-log.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

/* SC UART1 reaches whichever board the cabinet hangs off it. */
static void    midi_peer_send(uint8_t val);
static bool    midi_peer_has_response(void);
static uint8_t midi_peer_get_response(void);

/* A card reader on the SC's UARTs with bytes still to give: work pending for
 * the firmware, whichever reader the cabinet has. */
static bool sc_reader_pending(AN2131State *s)
{
    if (s->is_qc)
        return false;
    if (chihiro_hw210_enabled && chihiro_card_reader_global &&
        (card_reader_has_response(&chihiro_card_reader_global[0]) ||
         card_reader_has_response(&chihiro_card_reader_global[1])))
        return true;
    return chihiro_crp1231_enabled && chihiro_crp1231_global &&
           crp1231_has_response(chihiro_crp1231_global);
}

/* ── helpers ──────────────────────────────────────────────────────── */

static void an2131_reset(AN2131State *s);
static void an2131_set_cpucs(AN2131State *s, uint8_t val);
static void jvs_rx_deliver(AN2131State *s);
static void midi_rx_deliver(AN2131State *s);
static void card_rx_deliver(AN2131State *s);

static inline uint16_t canon(uint16_t addr)
{
    if (addr >= 0x1B40 && addr <= 0x1FFF)
        return addr + 0x6000;
    return addr;
}

/* Where an external address lands in the SRAM: in the half the QC's port C
 * bit 3 selects, when the pin is a driven output (see extmem). */
static inline uint32_t extmem_index(AN2131State *s, uint16_t addr)
{
    bool high = s->is_qc && (s->oec & 0x08) && !(s->portccfg & 0x08) &&
                (s->outc & 0x08);

    return (high ? 0x10000u : 0) | addr;
}

/* ── I2C state machine ────────────────────────────────────────────── */

static void i2c_fire_done(AN2131State *s, bool ack)
{
    s->i2cs = I2CS_DONE | (ack ? I2CS_ACK : 0);
    if (s->i2c_lastrd) s->i2cs |= I2CS_LASTRD;
    s->i2c_irq_pending = true;
    s->cpu.irq_recheck = true;
}

static void i2c_select_device(AN2131State *s, uint8_t addr7)
{
    if (addr7 == 0x50 && s->ic10_eeprom) {
        s->i2c.eeprom = s->ic10_eeprom;
        s->i2c.eeprom_size = s->ic10_size;
        s->i2c.addr_bytes_needed = 2;   /* 24LC64: 2-byte address */
    } else if (addr7 == 0x51 && s->ic10_eeprom) {
        s->i2c.eeprom = s->ic10_eeprom;
        s->i2c.eeprom_size = s->ic10_size;
        s->i2c.addr_bytes_needed = 2;   /* ic10 24LC64: A0=1 on baseboard → addr 0x51 */
    } else if (addr7 == 0x55 && s->ic11_eeprom) {
        s->i2c.eeprom = s->ic11_eeprom;
        s->i2c.eeprom_size = s->ic11_size;
        s->i2c.addr_bytes_needed = 1;   /* ic11, the baseboard settings EEPROM */
    } else if (addr7 == 0x32) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        #define TO_BCD(v) (uint8_t)(((v)/10 << 4) | ((v)%10))
        s->rtc_regs[0] = TO_BCD(t->tm_sec);
        s->rtc_regs[1] = TO_BCD(t->tm_min);
        s->rtc_regs[2] = TO_BCD(t->tm_hour);
        s->rtc_regs[3] = t->tm_wday + 1;
        s->rtc_regs[4] = TO_BCD(t->tm_mday);
        s->rtc_regs[5] = TO_BCD(t->tm_mon + 1);
        s->rtc_regs[6] = TO_BCD(t->tm_year % 100);
        #undef TO_BCD
        s->i2c.eeprom = s->rtc_regs;
        s->i2c.eeprom_size = 16;
        s->i2c.addr_bytes_needed = 1;
    } else {
        s->i2c.eeprom = NULL;
        s->i2c.eeprom_size = 0;
        s->i2c.addr_bytes_needed = 0;
    }
}

static void i2c_dat_write(AN2131State *s, uint8_t val)
{
    switch (s->i2c.phase) {
    case I2C_ADDR: {
        uint8_t addr7 = val >> 1;
        s->i2c.reading = val & 1;
        i2c_select_device(s, addr7);
        s->i2c.addr_bytes_sent = 0;
        if (!s->i2c.eeprom) {
            CHIHIRO_ERRF("AN2131: I2C NACK, no device at 0x%02X\n", addr7);
            i2c_fire_done(s, false);
            return;
        }
        s->i2c.phase = s->i2c.reading ? I2C_DATA : I2C_MEM_ADDR;
        s->i2c.first_read = s->i2c.reading;
        i2c_fire_done(s, true);
        break;
    }
    case I2C_MEM_ADDR:
        if (s->i2c.addr_bytes_needed == 2 && s->i2c.addr_bytes_sent == 0) {
            s->i2c.mem_addr = (uint16_t)val << 8;
        } else if (s->i2c.addr_bytes_needed == 2) {
            s->i2c.mem_addr |= val;
        } else {
            s->i2c.mem_addr = val;
        }
        s->i2c.addr_bytes_sent++;
        if (s->i2c.addr_bytes_sent >= s->i2c.addr_bytes_needed)
            s->i2c.phase = I2C_DATA;
        i2c_fire_done(s, true);
        break;
    case I2C_DATA:
        if (s->i2c.eeprom && s->i2c.mem_addr < (uint16_t)s->i2c.eeprom_size) {
            s->i2c.eeprom[s->i2c.mem_addr] = val;
            s->backup_dirty = true;
        }
        s->i2c.mem_addr++;
        i2c_fire_done(s, true);
        break;
    default:
        break;
    }
}

static uint8_t i2c_dat_read(AN2131State *s)
{
    uint8_t val = 0xFF;
    if (s->i2c.first_read) {
        s->i2c.first_read = false;
        i2c_fire_done(s, true);
        return val;
    }
    if (s->i2c.reading && s->i2c.eeprom &&
        s->i2c.mem_addr < (uint16_t)s->i2c.eeprom_size) {
        val = s->i2c.eeprom[s->i2c.mem_addr];
        s->i2c.mem_addr++;
    }
    if (s->i2c.phase != I2C_IDLE) {
        i2c_fire_done(s, !s->i2c_lastrd);
    }
    return val;
}

/* ── XDATA read callback ─────────────────────────────────────────── */

static uint8_t an2131_xdata_read(Cpu8051State *cpu, uint16_t addr)
{
    AN2131State *s = (AN2131State *)cpu->opaque;

    if (addr < 0x1B40)
        return s->ram[addr];

    uint16_t ca = canon(addr);

    /* Endpoint buffers: 0x7B40-0x7F3F → ram[0x1B40-0x1F3F] */
    if (ca >= 0x7B40 && ca < 0x7F40)
        return s->ram[ca - 0x6000];

    /* Register space: 0x7F40-0x7FFF */
    if (ca >= 0x7F40 && ca <= 0x7FFF) {
        switch (ca) {
        case AN_CPUCS:    return s->cpucs;

        case AN_PORTACFG: return s->portacfg;
        case AN_PORTBCFG: return s->portbcfg;
        case AN_PORTCCFG: return s->portccfg;
        case AN_OUTA:     return s->outa;
        case AN_OUTB:     return s->outb;
        case AN_OUTC:     return s->outc;
        case AN_PINSA: {
            /* Port A: the baseboard DIP switches, read by the QC's firmware
             * at 0x06CA into byte 1 of a control reply (the SC's does not
             * read it); bits 1 and 2 are the monitor's scan frequency, from
             * the cabinet (chihiro-cabinet.h). */
            uint8_t freq = chihiro_cabinet_monitor() == CHIHIRO_MONITOR_31KHZ
                           ? 0x04 : 0x02;
            return (s->pinsa & (uint8_t)~0x06u) | freq;
        }
        case AN_PINSB: {
            /* JVS sense is on bit 0 only. Bit 1 is a separate hardware pin (always high). */
            uint8_t sense = chihiro_jvs_global ? (chihiro_jvs_global->sense & 0x01) : 0x01;
            return (s->pinsb & ~0x01) | sense;
        }
        case AN_PINSC:    return s->pinsc;
        case AN_OEA:      return s->oea;
        case AN_OEB:      return s->oeb;
        case AN_OEC:      return s->oec;

        case AN_I2CS:     return s->i2cs;
        case AN_I2DAT:    cpu->events++; return i2c_dat_read(s);

        case AN_IVEC:     return s->ivec;
        case AN_IN07IRQ:  return s->in07irq;
        case AN_OUT07IRQ: return s->out07irq;
        case AN_USBIRQ:   return s->usbirq;
        case AN_IN07IEN:  return s->in07ien;
        case AN_OUT07IEN: return s->out07ien;
        case AN_USBIEN:   return s->usbien;
        case AN_USBBAV:   return s->usbbav;

        case AN_EP0CS:    return s->ep0cs;
        case AN_IN0BC:    return s->ep[0].bc_in;
        case AN_OUT0BC:   return s->ep[0].bc_out;

        case AN_SUDPTRH:  return (uint8_t)(s->sudptr >> 8);
        case AN_SUDPTRL:  return (uint8_t)s->sudptr;
        case AN_USBCS:    return s->usbcs;
        case AN_TOGCTL:   return s->togctl;
        case AN_FNADDR:   return s->fnaddr;
        case AN_USBPAIR:  return s->usbpair;
        case AN_IN07VAL:  return s->in07val;
        case AN_OUT07VAL: return s->out07val;

        case AN_FASTXFR:  return s->fastxfr;
        case AN_AUTOPTRH: return (uint8_t)(s->autoptr >> 8);
        case AN_AUTOPTRL: return (uint8_t)s->autoptr;
        case AN_AUTODATA: {
            cpu->events++;
            /* Pointed at itself, it would read itself forever. */
            uint8_t v = canon(s->autoptr) == AN_AUTODATA
                            ? 0xFF : an2131_xdata_read(cpu, s->autoptr);
            s->autoptr++;
            return v;
        }

        case AN_SETUPDAT:     case AN_SETUPDAT + 1:
        case AN_SETUPDAT + 2: case AN_SETUPDAT + 3:
        case AN_SETUPDAT + 4: case AN_SETUPDAT + 5:
        case AN_SETUPDAT + 6: case AN_SETUPDAT + 7:
            return s->setupdat[ca - AN_SETUPDAT];

        default:
            break;
        }

        /* EP1-7 IN CS/BC: 0x7FB6-0x7FC3 */
        if (ca >= 0x7FB6 && ca <= 0x7FC3) {
            int n = (ca - 0x7FB6) / 2 + 1;
            return ((ca - 0x7FB6) & 1) ? s->ep[n].bc_in : s->ep[n].cs_in;
        }
        /* EP1-7 OUT CS/BC: 0x7FC6-0x7FD3 */
        if (ca >= 0x7FC6 && ca <= 0x7FD3) {
            int n = (ca - 0x7FC6) / 2 + 1;
            return ((ca - 0x7FC6) & 1) ? s->ep[n].bc_out : s->ep[n].cs_out;
        }
        return 0xFF;
    }

    /* External SRAM: 0x2000-0x7B3F and 0x8000-0xFFFF */
    if (s->extmem)
        return s->extmem[extmem_index(s, addr)];
    return 0xFF;
}

/* ── XDATA write callback ────────────────────────────────────────── */

static void an2131_xdata_write(Cpu8051State *cpu, uint16_t addr, uint8_t val)
{
    AN2131State *s = (AN2131State *)cpu->opaque;

    if (addr < 0x1B40) {
        cpu->events += s->ram[addr] != val;
        s->ram[addr] = val;
        return;
    }

    uint16_t ca = canon(addr);

    /* Endpoint buffers */
    if (ca >= 0x7B40 && ca < 0x7F40) {
        cpu->events += s->ram[ca - 0x6000] != val;
        s->ram[ca - 0x6000] = val;
        return;
    }

    /* Register space: 0x7F40-0x7FFF. Every write counts as an event, many
     * of them having one beyond the byte stored, and may enable an
     * interrupt already pending (USBIEN, IN07IEN, OUT07IEN). */
    if (ca >= 0x7F40 && ca <= 0x7FFF) {
        cpu->events++;
        cpu->irq_recheck = true;
        switch (ca) {
        case AN_CPUCS:    s->cpucs = val; return;

        case AN_PORTACFG: s->portacfg = val; return;
        case AN_PORTBCFG: s->portbcfg = val; return;
        case AN_PORTCCFG: s->portccfg = val; return;
        case AN_OUTA:     s->outa = val; return;
        case AN_OUTB:     s->outb = val; return;
        case AN_OUTC:     s->outc = val; return;
        case AN_OEA:      s->oea = val; return;
        case AN_OEB:      s->oeb = val; return;
        case AN_OEC:      s->oec = val; return;

        /* I2C */
        case AN_I2CS: {
            if (val & I2CS_START) s->i2c.phase = I2C_ADDR;
            if (val & I2CS_STOP) {
                s->i2c.phase = I2C_IDLE;
                s->i2c_lastrd = false;
            } else {
                s->i2c_lastrd = !!(val & I2CS_LASTRD);
            }
            return;
        }
        case AN_I2DAT:
            i2c_dat_write(s, val);
            return;

        /* Interrupt registers — write-1-to-clear for IRQ, direct for IEN */
        case AN_IVEC:     return;
        case AN_IN07IRQ:  s->in07irq  &= ~val; return;
        case AN_OUT07IRQ: s->out07irq &= ~val; return;
        case AN_USBIRQ:   s->usbirq   &= ~val; return;
        case AN_IN07IEN:  s->in07ien  = val; return;
        case AN_OUT07IEN: s->out07ien = val; return;
        case AN_USBIEN:   s->usbien   = val; return;
        case AN_USBBAV:   s->usbbav   = val; return;

        /* EP0 */
        case AN_EP0CS:
            s->ep0cs = val;
            return;
        case AN_IN0BC:
            s->ep[0].bc_in = val;
            s->ep[0].in_armed = true;
            s->ep0cs |= EP0CS_INBSY;
            return;
        case AN_OUT0BC:
            s->ep[0].bc_out = 0;
            s->ep0cs &= ~EP0CS_OUTBSY;
            return;

        /* USB global */
        case AN_SUDPTRH: s->sudptr = (s->sudptr & 0x00FF) | ((uint16_t)val << 8); return;
        case AN_SUDPTRL: s->sudptr = (s->sudptr & 0xFF00) | val; return;
        case AN_USBCS:   s->usbcs = val; return;
        case AN_TOGCTL:  s->togctl = val; return;
        case AN_FNADDR:  s->fnaddr = val; return;
        case AN_USBPAIR: s->usbpair = val; return;
        case AN_IN07VAL: s->in07val = val; return;
        case AN_OUT07VAL: s->out07val = val; return;

        /* Autopointer */
        case AN_FASTXFR:  s->fastxfr = val; return;
        case AN_AUTOPTRH: s->autoptr = (s->autoptr & 0x00FF) | ((uint16_t)val << 8); return;
        case AN_AUTOPTRL: s->autoptr = (s->autoptr & 0xFF00) | val; return;
        case AN_AUTODATA:
            if (canon(s->autoptr) != AN_AUTODATA) {
                an2131_xdata_write(cpu, s->autoptr, val);
            }
            s->autoptr++;
            return;

        /* SETUPDAT is read-only from firmware side */
        case AN_SETUPDAT:     case AN_SETUPDAT + 1:
        case AN_SETUPDAT + 2: case AN_SETUPDAT + 3:
        case AN_SETUPDAT + 4: case AN_SETUPDAT + 5:
        case AN_SETUPDAT + 6: case AN_SETUPDAT + 7:
            return;

        default:
            break;
        }

        /* EP1-7 IN CS/BC */
        if (ca >= 0x7FB6 && ca <= 0x7FC3) {
            int n = (ca - 0x7FB6) / 2 + 1;
            if ((ca - 0x7FB6) & 1) {
                s->ep[n].bc_in = val;
                s->ep[n].in_armed = true;
                s->ep[n].cs_in |= EPCS_BSY;
            } else {
                s->ep[n].cs_in = val;
            }
            return;
        }
        /* EP1-7 OUT CS/BC */
        if (ca >= 0x7FC6 && ca <= 0x7FD3) {
            int n = (ca - 0x7FC6) / 2 + 1;
            if ((ca - 0x7FC6) & 1) {
                s->ep[n].bc_out = 0;
                s->ep[n].cs_out &= ~EPCS_BSY;
            } else {
                s->ep[n].cs_out = val;
            }
            return;
        }
        return;
    }

    /* External SRAM: 0x2000-0x7B3F and 0x8000-0xFFFF. Only the game's
     * backup half is what the save file keeps. */
    if (s->extmem) {
        uint32_t i = extmem_index(s, addr);

        if (s->extmem[i] != val) {
            cpu->events++;
            if (i >= 0x10000)
                s->backup_dirty = true;
        }
        s->extmem[i] = val;
    }
}

/* ── SFR callbacks (AN2131-specific registers) ────────────────────── */

static uint8_t an2131_sfr_read(Cpu8051State *cpu, uint8_t addr)
{
    AN2131State *s = (AN2131State *)cpu->opaque;
    if (addr >= SFR_TL0 && addr <= SFR_TH1) {
        cpu->events++;    /* a timer count, which an idle skip computes */
    }
    switch (addr) {
    case SFR_EXIF:  return s->exif;
    case SFR_MPAGE: return s->mpage;
    case SFR_DPS:   return s->dps;
    case SFR_EIE:   return s->eie;
    case SFR_EIP:   return s->eip;
    default:        return cpu->sfr[addr - 0x80];
    }
}

static void an2131_sfr_write(Cpu8051State *cpu, uint8_t addr, uint8_t val)
{
    AN2131State *s = (AN2131State *)cpu->opaque;
    if ((addr >= SFR_TL0 && addr <= SFR_TH1) ||
        addr == 0x99 || addr == 0xC0 || addr == 0xC1) {
        cpu->events++;    /* timer counts, the UARTs' data and SCON1 */
    }
    switch (addr) {
    case SFR_EXIF:
        s->exif = val;
        return;
    case SFR_MPAGE:
        s->mpage = val;
        return;
    case SFR_DPS: {
        /* The active DPTR is swapped in, so with DPS=1 DPL/DPH read DPTR1
         * (silicon: always DPTR0), and DPL1/DPH1 (0x84/0x85) are plain SFR
         * bytes. Neither firmware sets DPS. */
        uint8_t old = s->dps;
        s->dps = val & 0x01;
        if ((val & 1) != (old & 1)) {
            uint16_t tmp = cpu->dptr;
            cpu->dptr = cpu->dptr_alt;
            cpu->dptr_alt = tmp;
        }
        return;
    }
    case SFR_EIE:
        s->eie = val;
        return;
    case SFR_EIP:
        s->eip = val;
        return;
    case 0x99: /* SBUF0 — UART0 TX = RS-232C card reader (slot 1) */
        cpu->sfr[addr - 0x80] = val;
        if (!s->is_qc && chihiro_crp1231_enabled && chihiro_crp1231_global) {
            /* The Maximum Tune cabinet has its CRP-1231 here instead. */
            crp1231_receive_byte(chihiro_crp1231_global, val);
            s->card_ti_cycles[0] = s->total_cycles | 1;
            return;
        }
        if (!s->is_qc && chihiro_hw210_enabled &&
            chihiro_card_reader_global) {
            card_reader_write_byte(&chihiro_card_reader_global[1], val);
            /* Real UART: TI rises when the byte finishes shifting out.
             * Raising it instantly re-enters the TX interrupt one time too
             * many at end-of-frame and the firmware's byte counter
             * underflows — the pump then retransmits forever. */
            s->card_ti_cycles[0] = s->total_cycles | 1;
            return;
        }
        cpu->sfr[0x98 - 0x80] |= 0x02;  /* set TI0 */
        return;
    case 0xC0: /* SCON1 — defer next RX byte until after RETI */
    {
        uint8_t old = cpu->sfr[addr - 0x80];
        cpu->sfr[addr - 0x80] = val;
        if ((old & 0x01) && !(val & 0x01)) {
            s->jvs_rx_pending = true;
        }
        return;
    }
    case 0xC1: /* SBUF1 — serial TX (channel 1) */
    {
        if (!s->is_qc && chihiro_hw210_enabled) {
            /* The card readers own this channel here; TI deferred as SBUF0. */
            if (chihiro_card_reader_global)
                card_reader_write_byte(&chihiro_card_reader_global[0], val);
            s->card_ti_cycles[1] = s->total_cycles | 1;
            return;
        }
        cpu->sfr[0xC0 - 0x80] |= 0x02;  /* set TI1 — byte "sent" */

        if (!s->is_qc) {
            /* SC: plain MIDI to whatever board this cabinet carries */
            CHIHIRO_LOGF(FFB, "SC UART1 TX %02X\n", val);
            midi_peer_send(val);
            if (!s->midi_response_ready && midi_peer_has_response()) {
                s->midi_response_ready = true;
                s->midi_response_set_cycles = s->total_cycles;
            }
            return;
        }

        /* QC: RS-485 unescape: firmware escapes 0xE0→{0xD0,0xDF}, 0xD0→{0xD0,0xCF} */
        if (val == 0xE0) {
            if (s->jvs_tx_len > 0) {
                s->jvs_tx_len = 0;
                s->jvs_tx_expected = 0;
            }
            s->jvs_tx_escape = false;
            s->jvs_tx_buf[s->jvs_tx_len++] = val;
            return;
        }
        if (s->jvs_tx_escape) {
            val = val + 1;
            s->jvs_tx_escape = false;
        } else if (val == 0xD0) {
            s->jvs_tx_escape = true;
            return;
        }

        if (s->jvs_tx_len < (int)sizeof(s->jvs_tx_buf)) {
            s->jvs_tx_buf[s->jvs_tx_len++] = val;
        }
        if (s->jvs_tx_len == 3 && s->jvs_tx_buf[0] == 0xE0) {
            s->jvs_tx_expected = 3 + s->jvs_tx_buf[2];
        }

        if (s->jvs_tx_expected > 0 && s->jvs_tx_len >= s->jvs_tx_expected) {

            if (chihiro_jvs_global) {
                uint8_t raw_resp[256];
                int rlen = chihiro_jvs_process(chihiro_jvs_global,
                                               s->jvs_tx_buf, s->jvs_tx_expected,
                                               raw_resp, sizeof(raw_resp));
                if (rlen > 0) {
                    /* RS-485 escape: bytes after SYNC that are 0xE0 or 0xD0 */
                    int epos = 0;
                    s->jvs_rx_buf[epos++] = raw_resp[0]; /* SYNC */
                    for (int i = 1; i < rlen && epos < (int)sizeof(s->jvs_rx_buf) - 1; i++) {
                        if (raw_resp[i] == 0xE0 || raw_resp[i] == 0xD0) {
                            s->jvs_rx_buf[epos++] = 0xD0;
                            s->jvs_rx_buf[epos++] = raw_resp[i] - 1;
                        } else {
                            s->jvs_rx_buf[epos++] = raw_resp[i];
                        }
                    }
                    s->jvs_rx_len = epos;
                    s->jvs_rx_pos = 0;
                    s->jvs_response_ready = true;
                    s->jvs_response_set_cycles = s->total_cycles;
                }
            }
            s->jvs_tx_len = 0;
            s->jvs_tx_expected = 0;
        }
        return;
    }
    default:
        cpu->sfr[addr - 0x80] = val;
        return;
    }
}

/* ── Interrupt logic ──────────────────────────────────────────────── */

static bool usb_irq_pending(AN2131State *s)
{
    if (s->usbirq & s->usbien) return true;
    if (s->in07irq & s->in07ien) return true;
    if (s->out07irq & s->out07ien) return true;
    return false;
}

static uint8_t usb_avec(AN2131State *s)
{
    /* USBIRQ sources (highest priority) */
    if (s->usbirq & s->usbien & USBIRQ_SUDAV)  return AVEC_SUDAV;
    if (s->usbirq & s->usbien & USBIRQ_SOF)    return AVEC_SOF;
    if (s->usbirq & s->usbien & USBIRQ_SUTOK)  return AVEC_SUTOK;
    if (s->usbirq & s->usbien & USBIRQ_SUSP)   return AVEC_SUSPEND;
    if (s->usbirq & s->usbien & USBIRQ_URES)   return AVEC_USBRESET;

    /* IN endpoint IRQs */
    for (int i = 0; i < 8; i++) {
        if (s->in07irq & s->in07ien & (1 << i))
            return (uint8_t)(AVEC_EP0IN + i * 8);
    }
    /* OUT endpoint IRQs */
    for (int i = 0; i < 8; i++) {
        if (s->out07irq & s->out07ien & (1 << i))
            return (uint8_t)(AVEC_EP0OUT + i * 8);
    }
    return 0xFF;
}

static void check_interrupts(AN2131State *s)
{
    Cpu8051State *cpu = &s->cpu;

    /* Need global interrupt enable (IE.EA = bit 7) */
    if (!(cpu->sfr[SFR_IE - 0x80] & 0x80)) return;

    /* Don't nest interrupts */
    if (cpu->in_interrupt) return;

    /* Timer 0 overflow (TF0 = TCON.5), enabled by IE.1 (ET0) */
    if ((cpu->sfr[SFR_IE - 0x80] & 0x02) &&
        (cpu->sfr[SFR_TCON - 0x80] & 0x20)) {
        cpu->sfr[SFR_TCON - 0x80] &= ~0x20;  /* clear TF0 */
        cpu8051_interrupt(cpu, 0x000B);
        return;
    }

    /* Timer 1 overflow (TF1 = TCON.7), enabled by IE.3 (ET1) */
    if ((cpu->sfr[SFR_IE - 0x80] & 0x08) &&
        (cpu->sfr[SFR_TCON - 0x80] & 0x80)) {
        cpu->sfr[SFR_TCON - 0x80] &= ~0x80;  /* clear TF1 */
        cpu8051_interrupt(cpu, 0x001B);
        return;
    }

    /* Serial interrupt (RI|TI in SCON), enabled by IE.4 (ES) */
    if ((cpu->sfr[SFR_IE - 0x80] & 0x10) &&
        (cpu->sfr[SFR_SCON - 0x80] & 0x03)) {
        cpu8051_interrupt(cpu, 0x0023);
        return;
    }

    /* Second serial channel (SFR 0xC0 bits 0-1), enabled by IE.6 */
    if ((cpu->sfr[SFR_IE - 0x80] & 0x40) &&
        (cpu->sfr[0xC0 - 0x80] & 0x03)) {
        cpu8051_interrupt(cpu, 0x003B);
        return;
    }

    /* USB interrupt (INT2), enabled by EIE.0 */
    if ((s->eie & 0x01) && usb_irq_pending(s)) {
        uint8_t avec = usb_avec(s);
        if (avec != 0xFF) {
            s->ivec = avec;
            /* Autovector: patch the LJMP target low byte at code[0x0045] */
            if (s->usbbav & USBBAV_AVEN) {
                s->ram[0x0045] = avec;
            }
            s->exif |= 0x10;
            cpu8051_interrupt(cpu, INT2_VECTOR);
            return;
        }
    }

    /* I2C interrupt (INT3), enabled by EIE.1 */
    if ((s->eie & 0x02) && s->i2c_irq_pending) {
        s->i2c_irq_pending = false;
        s->exif |= 0x20;
        cpu8051_interrupt(cpu, INT3_VECTOR);
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

/* The MIDI and card pacing stamps count against total_cycles and are not
 * saved: after a CPU release (total_cycles back to 0) or a load, an old stamp
 * would let one byte, or one TI, skip its pacing. They start over. */
static void an2131_pacing_reset(AN2131State *s)
{
    s->midi_response_ready = false;
    s->midi_response_set_cycles = 0;
    memset(s->card_resp_cycles, 0, sizeof(s->card_resp_cycles));
    memset(s->card_delivering, 0, sizeof(s->card_delivering));
    memset(s->card_ti_cycles, 0, sizeof(s->card_ti_cycles));
}

/* Re-attach pointers and callbacks after a load; an in-flight I2C transaction
 * drops to idle, and pending interrupts are looked at again. */
void an2131_relink(AN2131State *s)
{
    s->cpu.code = s->ram;
    s->cpu.code_size = AN2131_RAM_SIZE;
    s->cpu.xdata_read = an2131_xdata_read;
    s->cpu.xdata_write = an2131_xdata_write;
    s->cpu.sfr_read_cb = an2131_sfr_read;
    s->cpu.sfr_write_cb = an2131_sfr_write;
    s->cpu.opaque = s;
    s->cpu.irq_recheck = true;
    s->i2c.phase = I2C_IDLE;
    s->i2c.eeprom = NULL;
    memset(&s->idle, 0, sizeof(s->idle));
    an2131_pacing_reset(s);
}

void an2131_init(AN2131State *s)
{
    memset(s, 0, sizeof(*s));
    cpu8051_init(&s->cpu);

    s->cpu.code = s->ram;
    s->cpu.code_size = AN2131_RAM_SIZE;
    s->cpu.xdata_read = an2131_xdata_read;
    s->cpu.xdata_write = an2131_xdata_write;
    s->cpu.sfr_read_cb = an2131_sfr_read;
    s->cpu.sfr_write_cb = an2131_sfr_write;
    s->cpu.opaque = s;

    an2131_reset(s);
}

static void an2131_reset(AN2131State *s)
{
    cpu8051_reset(&s->cpu);
    memset(&s->idle, 0, sizeof(s->idle));

    s->cpu.code = s->ram;
    s->cpu.code_size = AN2131_RAM_SIZE;

    s->cpucs = CPUCS_8051RES;
    s->cpu_running = false;

    s->ep0cs = 0;
    s->usbbav = 0;
    s->usbcs = 0;
    s->usbpair = 0;
    s->in07val = 0x01;
    s->out07val = 0x01;
    s->fnaddr = 0;
    s->sudptr = 0;
    s->autoptr = 0;
    s->fastxfr = 0;
    s->togctl = 0;

    s->usbirq = 0;
    s->in07irq = 0;
    s->out07irq = 0;
    s->usbien = 0;
    s->in07ien = 0;
    s->out07ien = 0;
    s->ivec = 0;

    s->i2cs = I2CS_DONE;
    s->i2c_irq_pending = false;
    s->i2c_lastrd = false;
    memset(&s->i2c, 0, sizeof(s->i2c));
    s->i2c.phase = I2C_IDLE;

    /* Baseboard port defaults; the monitor bits come from AN_PINSA. */
    s->pinsa = 0xC9;
    s->pinsb = 0x52;
    s->pinsc = 0x00;
    s->outa = 0;
    s->outb = 0;
    s->outc = 0;
    s->oea = 0;
    s->oeb = 0;
    s->oec = 0;
    s->portacfg = 0;
    s->portbcfg = 0;
    s->portccfg = 0;

    s->exif = 0;
    s->eie = 0;
    s->eip = 0;
    s->mpage = 0;
    s->dps = 0;

    memset(s->ep, 0, sizeof(s->ep));
    memset(s->setupdat, 0, sizeof(s->setupdat));
}

void an2131_b2_boot(AN2131State *s, const uint8_t *eeprom, int eeprom_size)
{
    if (eeprom_size < 7 || eeprom[0] != 0xB2) {
        fprintf(stderr, "[AN2131] B2 boot skipped — invalid header (0x%02X)\n",
                eeprom_size > 0 ? eeprom[0] : 0);
        return;
    }

    uint16_t vid = eeprom[1] | ((uint16_t)eeprom[2] << 8);
    uint16_t pid = eeprom[3] | ((uint16_t)eeprom[4] << 8);
    fprintf(stderr, "[AN2131] B2 boot: VID=0x%04X PID=0x%04X\n", vid, pid);

    int pos = 7;
    int total_bytes = 0;

    while (pos + 4 <= eeprom_size) {
        uint8_t lenh = eeprom[pos];
        uint8_t lenl = eeprom[pos + 1];
        bool last = lenh & 0x80;
        uint16_t len  = ((uint16_t)(lenh & 0x7F) << 8) | lenl;
        uint16_t addr = ((uint16_t)eeprom[pos + 2] << 8) | eeprom[pos + 3];
        pos += 4;

        if (len == 0) break;
        if (pos + len > eeprom_size) break;

        an2131_anchor_load(s, addr, &eeprom[pos], len);
        total_bytes += len;
        pos += len;

        if (last)
            break;
    }

    fprintf(stderr, "[AN2131] B2 boot: loaded %d bytes, cpu_running=%d\n",
            total_bytes, s->cpu_running);
}

void an2131_anchor_load(AN2131State *s, uint16_t addr,
                        const uint8_t *data, int len)
{
    if (addr == 0x7F92) {
        if (len > 0)
            an2131_set_cpucs(s, data[0]);
        return;
    }
    for (int i = 0; i < len && (addr + i) < AN2131_RAM_SIZE; i++)
        s->ram[addr + i] = data[i];
}

static void an2131_set_cpucs(AN2131State *s, uint8_t val)
{
    bool was_reset = s->cpucs & CPUCS_8051RES;
    s->cpucs = val;

    if (was_reset && !(val & CPUCS_8051RES)) {
        cpu8051_reset(&s->cpu);

        s->total_cycles = 0;
        an2131_pacing_reset(s);
        s->jvs_response_set_cycles = 0;
        s->jvs_response_ready = false;
        s->jvs_rx_pending = false;
        s->jvs_rx_len = 0;
        s->jvs_rx_pos = 0;
        s->jvs_tx_len = 0;
        s->jvs_tx_expected = 0;
        s->jvs_tx_escape = false;

        s->cpu_running = true;
        fprintf(stderr, "[AN2131] CPU released — running firmware from 0x0000\n");
        an2131_run(s, 8000000);
        fprintf(stderr, "[AN2131] Init done — PC=0x%04X SP=0x%02X IE=0x%02X "
                "EIE=0x%02X USBIEN=0x%02X CKCON=0x%02X\n",
                s->cpu.pc, s->cpu.sp,
                s->cpu.sfr[SFR_IE - 0x80], s->eie, s->usbien,
                s->cpu.sfr[SFR_CKCON - 0x80]);
    } else if (!was_reset && (val & CPUCS_8051RES)) {
        s->cpu_running = false;
        fprintf(stderr, "[AN2131] CPU held in reset\n");
    }
}

int an2131_setup_packet(AN2131State *s, const uint8_t setup[8],
                        const uint8_t *out_data, int out_len,
                        uint8_t *resp_buf, int resp_max)
{
    if (!s->cpu_running) return -1;

    /* Force-clear stale interrupt state */
    s->cpu.in_interrupt = false;

    memcpy(s->setupdat, setup, 8);

    /* Pre-load OUT0BUF for OUT control transfers */
    if (out_data && out_len > 0) {
        int copy = MIN(out_len, AN2131_EP_BUFSZ);
        memcpy(&s->ram[AN_OUT0BUF - 0x6000], out_data, copy);
        s->ep[0].bc_out = copy;
        s->ep0cs |= EP0CS_OUTBSY;
        s->out07irq |= 0x01;
        s->cpu.irq_recheck = true;
    }

    s->usbirq |= USBIRQ_SUDAV;
    s->cpu.irq_recheck = true;
    s->ep[0].in_armed = false;
    /* Hold card RX delivery only while a card drain (0x1A/0x1B) runs. */
    s->in_setup = !s->is_qc && (setup[0] & 0x80) &&
                  (setup[1] == 0x1A || setup[1] == 0x1B);

    bool is_in = setup[0] & 0x80;
    bool need_ep4 = (setup[1] == 0x19 && is_in);
    bool need_ep2 = (setup[1] == 0x17 && is_in);
    /* SC card polls (0x1A/0x1B in, 0x22/0x23 out) take long, like the 0x17
     * EEPROM read, whichever reader answers them. */
    bool need_card = !s->is_qc &&
                     (chihiro_hw210_enabled || chihiro_crp1231_enabled) &&
                     (((setup[1] == 0x1A || setup[1] == 0x1B) && is_in) ||
                      setup[1] == 0x22 || setup[1] == 0x23);

    /* On the bus the device NAKs the data stage until the firmware arms EP0
     * IN, however long that takes; here the request runs until the firmware
     * answers. HEURISTIC: the cap, 100000 cycles (17 ms of 8051 time), past
     * any exchange of the firmware's own (a JVS round trip is under 45000). */
    int cycles = 0;
    int limit = 100000;
    int drain = 0;
    bool mainloop_hit = false;

    while (cycles < limit) {
        jvs_rx_deliver(s);
        midi_rx_deliver(s);
        card_rx_deliver(s);
        check_interrupts(s);
        int c = cpu8051_step(&s->cpu);
        cycles += c;
        s->total_cycles += c;

        if (drain && s->cpu.pc == 0x0696) mainloop_hit = true;

        if (is_in && s->ep[0].in_armed)
            s->ep0cs &= ~EP0CS_INBSY;

        if (!drain) {
            if (is_in && s->ep[0].in_armed) {
                if ((!need_ep4 || s->ep[4].in_armed) &&
                    (!need_ep2 || s->ep[2].in_armed))
                    drain = cycles;
            }
            if (!is_in && !(s->usbirq & USBIRQ_SUDAV)) drain = cycles;
        }
        if (drain && mainloop_hit && setup[1] == 0x20) break;
        if (drain && mainloop_hit && (cycles - drain) > 2000) break;
        /* Card requests: once the reply is armed the critical work is done
         * (the bulk feeder keeps running from an2131_run) — leaving the
         * window open to the 20000-cycle drain tail on every poll makes the
         * whole machine boot and run visibly slower. */
        if (drain && need_card && (cycles - drain) > 2000) break;
        if (drain && (cycles - drain) > 20000) break;
    }
    s->in_setup = false;

    if (is_in && s->ep[0].in_armed) {
        int bc = s->ep[0].bc_in;
        int copy = MIN(bc, resp_max);
        memcpy(resp_buf, &s->ram[AN_IN0BUF - 0x6000], copy);

        s->ep[0].in_armed = false;
        s->ep0cs &= ~EP0CS_INBSY;
        return copy;
    }

    /* SUDPTR-based EP0 IN (AN2131 silicon feature for standard USB requests):
     * Firmware sets SUDPTRH:SUDPTRL to point to descriptor data in RAM,
     * then sets EP0CS. The silicon reads from SUDPTR automatically.
     * Vendor requests bypass this — they write IN0BUF + IN0BC directly. */
    if (is_in && s->sudptr != 0) {
        uint16_t ptr = s->sudptr;
        if (ptr < AN2131_RAM_SIZE) {
            int desc_len = s->ram[ptr];
            if (ptr + 3 < AN2131_RAM_SIZE && s->ram[ptr + 1] == 0x02) {
                desc_len = s->ram[ptr + 2] | (s->ram[ptr + 3] << 8);
            }
            int wLength = s->setupdat[6] | (s->setupdat[7] << 8);
            if (desc_len > wLength) desc_len = wLength;
            if (desc_len > resp_max) desc_len = resp_max;
            if (ptr + desc_len <= AN2131_RAM_SIZE) {
                memcpy(resp_buf, &s->ram[ptr], desc_len);
                s->sudptr = 0;
                return desc_len;
            }
        }
        s->sudptr = 0;
    }

    return 0;
}

int an2131_ep_in_poll(AN2131State *s, int ep_nr)
{
    if (ep_nr < 0 || ep_nr >= AN2131_EP_COUNT) return 0;
    if (!s->cpu_running) return 0;

    an2131_run(s, 6000);

    return s->ep[ep_nr].in_armed ? s->ep[ep_nr].bc_in : -1;
}

int an2131_ep_in_read(AN2131State *s, int ep_nr,
                      uint8_t *buf, int max_len)
{
    if (ep_nr < 0 || ep_nr >= AN2131_EP_COUNT) return 0;
    if (!s->ep[ep_nr].in_armed) return 0;

    int bc = s->ep[ep_nr].bc_in;
    int copy = MIN(bc, max_len);

    /* INnBUF RAM offset = 0x1F00 - ep_nr * 0x80 */
    uint16_t buf_off = 0x1F00 - (uint16_t)ep_nr * 0x80;
    if (buf_off + copy <= AN2131_RAM_SIZE)
        memcpy(buf, &s->ram[buf_off], copy);

    /* HEURISTIC: a short host read leaves the rest armed for the next one
     * (OHCI would report an overrun). */
    if (copy < bc) {
        memmove(&s->ram[buf_off], &s->ram[buf_off + copy], bc - copy);
        s->ep[ep_nr].bc_in = bc - copy;
        return copy;
    }

    s->ep[ep_nr].in_armed = false;
    s->ep[ep_nr].bc_in = 0;
    s->ep[ep_nr].cs_in &= ~EPCS_BSY;

    /* Notify firmware: IN data was sent to host */
    s->in07irq |= (1 << ep_nr);
    s->cpu.irq_recheck = true;

    return copy;
}

void an2131_ep_out_write(AN2131State *s, int ep_nr,
                         const uint8_t *data, int len)
{
    if (ep_nr < 0 || ep_nr >= AN2131_EP_COUNT) return;
    if (!s->cpu_running) return;

    int copy = MIN(len, AN2131_EP_BUFSZ);

    /* OUTnBUF RAM offset = 0x1EC0 - ep_nr * 0x80 */
    uint16_t buf_off = 0x1EC0 - (uint16_t)ep_nr * 0x80;
    if (buf_off + copy <= AN2131_RAM_SIZE)
        memcpy(&s->ram[buf_off], data, copy);

    s->ep[ep_nr].bc_out = copy;
    s->ep[ep_nr].cs_out |= EPCS_BSY;

    s->out07irq |= (1 << ep_nr);
    s->cpu.irq_recheck = true;

    an2131_run(s, 2000);
}

static void jvs_rx_deliver(AN2131State *s)
{
    Cpu8051State *cpu = &s->cpu;
    if (cpu->in_interrupt) return;

    /* First byte: simulate RS-485 serial latency (~2ms at 115200 baud).
     * Use CPU cycles (not virtual time) so delay works during an2131_run bursts. */
    if (s->jvs_response_ready) {
        uint64_t elapsed = s->total_cycles - s->jvs_response_set_cycles;
        if (elapsed < 15000) return;
        if (cpu->sfr[0xC0 - 0x80] & 0x02) return;  /* TI1 still set */
        cpu->events++;
        cpu->sfr[0xC1 - 0x80] = s->jvs_rx_buf[0];
        s->jvs_rx_pos = 1;
        cpu->sfr[0xC0 - 0x80] |= 0x01;  /* set RI1 */
        cpu->irq_recheck = true;
        s->jvs_response_ready = false;
        s->jvs_rx_pending = false;
        return;
    }

    /* Subsequent bytes: deliver after ISR returns */
    if (s->jvs_rx_pending) {
        if (s->jvs_rx_pos < s->jvs_rx_len) {
            uint8_t rxbyte = s->jvs_rx_buf[s->jvs_rx_pos];
            cpu->events++;
            cpu->sfr[0xC1 - 0x80] = rxbyte;
            s->jvs_rx_pos++;
            cpu->sfr[0xC0 - 0x80] |= 0x01;  /* set RI1 */
            cpu->irq_recheck = true;
        }
        s->jvs_rx_pending = false;
    }
}

/* The board on the MIDI line is the cabinet's; a cabinet without one leaves
 * the line unanswered. */
static void midi_peer_send(uint8_t val)
{
    switch (chihiro_cabinet_drive_board()) {
    case CHIHIRO_DRIVE_V257:
        if (chihiro_v257_global)
            v257_receive_byte(chihiro_v257_global, val);
        break;
    case CHIHIRO_DRIVE_SEGA838:
        if (chihiro_driveboard_global)
            driveboard_receive_byte(chihiro_driveboard_global, val);
        break;
    default:
        break;
    }
}

static bool midi_peer_has_response(void)
{
    switch (chihiro_cabinet_drive_board()) {
    case CHIHIRO_DRIVE_V257:
        return chihiro_v257_global &&
               v257_has_response(chihiro_v257_global);
    case CHIHIRO_DRIVE_SEGA838:
        return chihiro_driveboard_global &&
               driveboard_has_response(chihiro_driveboard_global);
    default:
        return false;
    }
}

static uint8_t midi_peer_get_response(void)
{
    if (chihiro_cabinet_drive_board() == CHIHIRO_DRIVE_V257)
        return v257_get_response(chihiro_v257_global);
    return driveboard_get_response(chihiro_driveboard_global);
}

static void midi_rx_deliver(AN2131State *s)
{
    if (s->is_qc) return;
    if (chihiro_hw210_enabled) return;  /* MIDI belongs to the card reader */
    if (!midi_peer_has_response()) return;

    /* A board may speak unasked (the V257 reports its motor line), so pacing
     * starts from what is queued. */
    if (!s->midi_response_ready) {
        s->midi_response_ready = true;
        s->midi_response_set_cycles = s->total_cycles;
    }

    Cpu8051State *cpu = &s->cpu;
    if (cpu->in_interrupt) return;
    if (cpu->sfr[0xC0 - 0x80] & 0x01) return;  /* RI1 still set */

    if (s->total_cycles - s->midi_response_set_cycles < 5000) return;

    uint8_t byte = midi_peer_get_response();
    CHIHIRO_LOGF(FFB, "SC UART1 RX %02X\n", byte);
    cpu->events++;
    cpu->sfr[0xC1 - 0x80] = byte;
    cpu->sfr[0xC0 - 0x80] |= 0x01;  /* set RI1 */
    cpu->irq_recheck = true;

    if (midi_peer_has_response()) {
        s->midi_response_set_cycles = s->total_cycles;
    } else {
        s->midi_response_ready = false;
    }
}

/* HEURISTIC: a card reader's turnaround before the first byte of an answer,
 * then a serial byte rate; no HW210 or CRP-1231 datasheet gives them. */
#define CARD_TURNAROUND_CYCLES 60000
#define CARD_BYTE_CYCLES       1500
/* HEURISTIC: the delay from an SBUF write to TI (33 us; a byte at 115200 baud
 * would take about 520 cycles). */
#define CARD_TI_CYCLES         200

/* A byte from a card reader lands in a SC UART: SBUF, the even parity in RB8
 * (the 9-bit mode's ninth bit), RI, and the serial interrupt looked at again. */
static void sc_uart_post_rx(AN2131State *s, uint8_t sbuf, uint8_t scon,
                            uint8_t b)
{
    Cpu8051State *cpu = &s->cpu;
    uint8_t par = b;

    cpu->events++;
    cpu->sfr[sbuf - 0x80] = b;
    par ^= par >> 4; par ^= par >> 2; par ^= par >> 1;
    if (par & 1)
        cpu->sfr[scon - 0x80] |= 0x04;
    else
        cpu->sfr[scon - 0x80] &= ~0x04;
    cpu->sfr[scon - 0x80] |= 0x01;
    cpu->irq_recheck = true;
}

/* Deliver one pending card-response byte into a SC UART RX register: wait a
 * reader round-trip before the first byte, then pace at a real serial byte
 * rate (back-to-back delivery starves the firmware's SUDAV dispatch during
 * long responses), and gate on RI so the UART ISR consumes each byte before
 * the next is posted. The UARTs run in 9-bit mode: the firmware checks the
 * 9th bit (RB8, SCON bit 2) as even parity of each byte and reports a channel
 * error the game rejects on mismatch. */
static void card_rx_deliver_ch(AN2131State *s, int slot,
                               uint8_t sbuf, uint8_t scon, int idx)
{
    Cpu8051State *cpu = &s->cpu;
    CardReaderState *c = &chihiro_card_reader_global[slot];

    if (!card_reader_has_response(c)) {
        s->card_resp_cycles[idx] = 0;               /* nothing pending */
        s->card_delivering[idx] = false;
        return;
    }
    if (s->card_resp_cycles[idx] == 0)
        s->card_resp_cycles[idx] = s->total_cycles; /* response just became ready */
    if (c->tx_pos == 0 && s->card_delivering[idx]) {
        /* A repeated command replaced the response we were delivering (the
         * firmware retransmits frames): restart the turnaround delay so the
         * duplicates settle into one clean reply instead of piling up ACKs
         * until the channel FIFO overflows. */
        s->card_resp_cycles[idx] = s->total_cycles;
        s->card_delivering[idx] = false;
    }
    uint64_t need = c->tx_pos == 0 ? CARD_TURNAROUND_CYCLES : CARD_BYTE_CYCLES;
    if (s->total_cycles - s->card_resp_cycles[idx] < need)
        return;
    if (cpu->sfr[scon - 0x80] & 0x01)
        return;                                     /* RI set: previous byte unread */

    uint8_t b;
    if (card_reader_read(c, &b, 1) == 1) {
        s->card_resp_cycles[idx] = s->total_cycles; /* pace the next byte */
        s->card_delivering[idx] = true;
        sc_uart_post_rx(s, sbuf, scon, b);
    }
}

/* The CRP-1231 on the RS-232C line: same wire, framing and pacing as
 * card_rx_deliver_ch. */
static void crp1231_rx_deliver(AN2131State *s)
{
    Cpu8051State *cpu = &s->cpu;
    CRP1231State *r = chihiro_crp1231_global;

    if (!r || !crp1231_has_response(r)) {
        s->card_resp_cycles[0] = 0;
        return;
    }
    if (s->card_resp_cycles[0] == 0)
        s->card_resp_cycles[0] = s->total_cycles;

    uint64_t need = crp1231_mid_response(r) ? CARD_BYTE_CYCLES
                                             : CARD_TURNAROUND_CYCLES;
    if (s->total_cycles - s->card_resp_cycles[0] < need)
        return;
    if (cpu->sfr[SFR_SCON - 0x80] & 0x01)
        return;                                     /* RI set: byte unread */

    uint8_t b;
    if (crp1231_read(r, &b)) {
        s->card_resp_cycles[0] = s->total_cycles;
        sc_uart_post_rx(s, SFR_SBUF, SFR_SCON, b);
    }
}

static void card_rx_deliver(AN2131State *s)
{
    if (s->is_qc)
        return;
    bool mt = chihiro_crp1231_enabled;
    if (!mt && (!chihiro_hw210_enabled || !chihiro_card_reader_global))
        return;
    /* Deferred TX-complete: raise TI CARD_TI_CYCLES after the SBUF write,
     * as the silicon raises it once the byte is out. */
    for (int i = 0; i < 2; i++) {
        if (s->card_ti_cycles[i] &&
            s->total_cycles - s->card_ti_cycles[i] >= CARD_TI_CYCLES) {
            s->cpu.events++;
            s->cpu.sfr[(i == 0 ? 0x98 : 0xC0) - 0x80] |= 0x02;
            s->cpu.irq_recheck = true;
            s->card_ti_cycles[i] = 0;
        }
    }
    /* Hold delivery while a card-drain vendor request (0x1A/0x1B) is still
     * being serviced (reply not yet armed): a serial RX interrupt in the
     * middle of the drain/feeder copy clobbers shared firmware work
     * pointers and the game receives zeros. */
    if (s->in_setup && !s->ep[0].in_armed) return;
    if (s->cpu.in_interrupt) return;
    if (mt) {
        crp1231_rx_deliver(s);
        return;
    }
    card_rx_deliver_ch(s, 1, 0x99, 0x98, 0);   /* RS-232C: SBUF0/SCON0 */
    card_rx_deliver_ch(s, 0, 0xC1, 0xC0, 1);   /* MIDI:    SBUF1/SCON1 */
}

/* ── The main loop at rest ─────────────────────────────────────────── */

/* Both firmwares idle in a polling loop, never in the 8051's IDLE mode. A
 * turn that leaves the chip as it found it repeats, so such turns are charged
 * at the chip's rate and not run. Exact: the fingerprint (registers, internal
 * RAM, SFRs but the timer counts) must repeat AND nothing outside it moved
 * during the turn (cpu.events: an XDATA byte changed, a register write, a
 * read with a side effect, a UART byte, a timer count touched, anything from
 * outside the program). Timers advance by arithmetic, and a skip stops short
 * of an overflow that would raise a flag, of the budget, and of a turn begun
 * in an earlier call. */
/* Eight bytes at a time; each step is a bijection of the running value, so
 * two inputs that differ in a single word never fingerprint alike. */
static uint64_t an2131_mix(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i += 8) {
        uint64_t w;

        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    return h;
}

/* The chip at one point of its program, the timer counts aside. */
static uint64_t an2131_idle_hash(AN2131State *s)
{
    Cpu8051State *c = &s->cpu;
    uint8_t sfr[128];
    const uint8_t regs[16] = {
        c->sp, c->acc, c->b, c->psw, c->in_interrupt,
        (uint8_t)c->dptr, (uint8_t)(c->dptr >> 8),
        (uint8_t)c->dptr_alt, (uint8_t)(c->dptr_alt >> 8),
        s->exif, s->eie, s->eip, s->dps, s->mpage,
    };
    uint64_t h = 0x243F6A8885A308D3ull;

    memcpy(sfr, c->sfr, sizeof(sfr));
    sfr[SFR_TL0 - 0x80] = sfr[SFR_TH0 - 0x80] = 0;
    sfr[SFR_TL1 - 0x80] = sfr[SFR_TH1 - 0x80] = 0;
    /* an2131_mix takes whole eight-byte words. */
    QEMU_BUILD_BUG_ON(sizeof(regs) % 8 || sizeof(c->iram) % 8 ||
                      sizeof(sfr) % 8);
    h = an2131_mix(h, regs, sizeof(regs));
    h = an2131_mix(h, c->iram, sizeof(c->iram));
    return an2131_mix(h, sfr, sizeof(sfr));
}

/* Whether the chip is left alone while the turns last: nothing on its way
 * in, no transmitter finishing, no USB or I2C request, no interrupt it would
 * take now. */
static bool an2131_idle_quiet(AN2131State *s)
{
    Cpu8051State *c = &s->cpu;
    uint8_t ie = c->sfr[SFR_IE - 0x80], tcon = c->sfr[SFR_TCON - 0x80];

    if (c->in_interrupt) {
        return false;
    }
    if (s->jvs_response_ready || s->jvs_rx_pending || s->midi_response_ready ||
        s->card_ti_cycles[0] || s->card_ti_cycles[1] ||
        usb_irq_pending(s) || s->i2c_irq_pending ||
        (s->ep[0].in_armed && (s->ep0cs & EP0CS_INBSY))) {
        return false;
    }
    if ((!s->is_qc && !chihiro_hw210_enabled && midi_peer_has_response()) ||
        sc_reader_pending(s)) {
        return false;
    }
    /* An interrupt check_interrupts would take now. */
    return !((ie & 0x80) &&
             (((ie & 0x02) && (tcon & 0x20)) || ((ie & 0x08) && (tcon & 0x80)) ||
              ((ie & 0x10) && (c->sfr[SFR_SCON - 0x80] & 0x03)) ||
              ((ie & 0x40) && (c->sfr[SFR_SCON1 - 0x80] & 0x03))));
}

/* `ticks` counts on an 8-bit auto-reload counter; returns the overflows. */
static uint64_t an2131_count8(uint8_t *tl, uint8_t th, uint64_t ticks)
{
    uint64_t first = 256 - *tl, period = 256 - th;

    if (ticks < first) {
        *tl += ticks;
        return 0;
    }
    ticks -= first;
    *tl = th + ticks % period;
    return 1 + ticks / period;
}

/* `ticks` counts on a 16-bit counter; returns the overflows. */
static uint64_t an2131_count16(uint8_t *tl, uint8_t *th, uint64_t ticks)
{
    uint64_t t = *tl | (*th << 8), first = 0x10000 - t, overflows = 0;

    if (ticks < first) {
        t += ticks;
    } else {
        ticks -= first;
        overflows = 1 + ticks / 0x10000;
        t = ticks % 0x10000;
    }
    *tl = (uint8_t)t;
    *th = (uint8_t)(t >> 8);
    return overflows;
}

/* Instructions before a timer overflow that would raise a flag still clear,
 * the one thing a skip must not step over; UINT64_MAX when there is none.
 * Timer 0 counts one instruction in three unless CKCON.3 is set, timer 1
 * every one -- as timer_tick does, modes 1 and 2 only. */
static uint64_t an2131_steps_to_overflow(AN2131State *s)
{
    Cpu8051State *c = &s->cpu;
    uint8_t tcon = c->sfr[SFR_TCON - 0x80], tmod = c->sfr[SFR_TMOD - 0x80];
    uint8_t m0 = tmod & 3, m1 = (tmod >> 4) & 3;
    uint64_t limit = UINT64_MAX;

    if ((tcon & 0x10) && !(tcon & 0x20) && (m0 == 1 || m0 == 2)) {
        uint64_t ticks = m0 == 1
            ? 0x10000 - (c->sfr[SFR_TL0 - 0x80] | (c->sfr[SFR_TH0 - 0x80] << 8))
            : 256 - c->sfr[SFR_TL0 - 0x80];
        limit = (c->sfr[SFR_CKCON - 0x80] & 0x08) ? ticks
                                              : 3 * ticks - c->timer0_prescale;
    }
    if ((tcon & 0x40) && !(tcon & 0x80) && !(tmod & 0x40) &&
        (m1 == 1 || m1 == 2)) {
        uint64_t ticks = m1 == 1
            ? 0x10000 - (c->sfr[SFR_TL1 - 0x80] | (c->sfr[SFR_TH1 - 0x80] << 8))
            : 256 - c->sfr[SFR_TL1 - 0x80];
        limit = MIN(limit, ticks);
    }
    return limit;
}

/* Moves the timers on by `steps` instructions, as timer_tick would have. */
static void an2131_advance_timers(AN2131State *s, uint64_t steps)
{
    Cpu8051State *c = &s->cpu;
    uint8_t *sfr = c->sfr;
    uint8_t tcon = sfr[SFR_TCON - 0x80], tmod = sfr[SFR_TMOD - 0x80];
    uint8_t m0 = tmod & 3, m1 = (tmod >> 4) & 3;
    uint64_t overflows;

    if (tcon & 0x10) {
        uint64_t ticks = steps;
        if (!(sfr[SFR_CKCON - 0x80] & 0x08)) {
            ticks = (c->timer0_prescale + steps) / 3;
            c->timer0_prescale = (c->timer0_prescale + steps) % 3;
        }
        overflows = m0 == 1 ? an2131_count16(&sfr[SFR_TL0 - 0x80],
                                             &sfr[SFR_TH0 - 0x80], ticks)
                  : m0 == 2 ? an2131_count8(&sfr[SFR_TL0 - 0x80],
                                            sfr[SFR_TH0 - 0x80], ticks)
                  : 0;
        if (overflows) {
            tcon |= 0x20;
            c->irq_recheck = true;
        }
    }
    if ((tcon & 0x40) && !(tmod & 0x40)) {
        overflows = m1 == 1 ? an2131_count16(&sfr[SFR_TL1 - 0x80],
                                             &sfr[SFR_TH1 - 0x80], steps)
                  : m1 == 2 ? an2131_count8(&sfr[SFR_TL1 - 0x80],
                                            sfr[SFR_TH1 - 0x80], steps)
                  : 0;
        if (overflows) {
            tcon |= 0x80;
            c->irq_recheck = true;
        }
    }
    sfr[SFR_TCON - 0x80] = tcon;
}

/* Called after every instruction an2131_run steps; returns the cycles of the
 * turns it charged without running them. */
static int an2131_idle_turn(AN2131State *s, int budget_left)
{
    uint64_t hash, turn_c, turn_s, k, limit;
    bool same;

    if (s->cpu.pc != s->idle.pc) {
        /* Take a new candidate now and then: whichever point the chip is at
         * when it goes idle will come round again a turn later. */
        if (++s->idle.sample >= 4096) {
            s->idle.sample = 0;
            s->idle.pc = s->cpu.pc;
            s->idle.hash = 0;
        }
        return 0;
    }
    hash = an2131_idle_hash(s);
    turn_c = s->total_cycles - s->idle.cycles;
    turn_s = s->steps - s->idle.steps;
    same = hash == s->idle.hash && s->cpu.events == s->idle.events &&
           turn_c > 0 && turn_s > 0;
    s->idle.hash = hash;
    s->idle.cycles = s->total_cycles;
    s->idle.steps = s->steps;
    s->idle.events = s->cpu.events;
    if (!same || budget_left <= 0 || !an2131_idle_quiet(s)) {
        return 0;
    }
    k = (uint64_t)budget_left / turn_c;
    limit = an2131_steps_to_overflow(s);
    if (limit != UINT64_MAX) {
        k = MIN(k, (limit - 1) / turn_s);
    }
    if (k == 0) {
        return 0;
    }
    an2131_advance_timers(s, k * turn_s);
    s->total_cycles += k * turn_c;
    s->steps += k * turn_s;
    s->idle.cycles = s->total_cycles;
    s->idle.steps = s->steps;
    return (int)(k * turn_c);
}

int an2131_run(AN2131State *s, int max_cycles)
{
    if (!s->cpu_running) return 0;

    int total = 0;
    /* A turn is only trusted if it ran whole within this call. */
    s->idle.hash = 0;
    while (total < max_cycles) {
        /* Both are no-ops unless something is queued, so the guards are
         * exact rather than heuristic. */
        if (s->jvs_response_ready || s->jvs_rx_pending) {
            jvs_rx_deliver(s);
        }
        midi_rx_deliver(s);
        card_rx_deliver(s);
        if (s->cpu.irq_recheck) {
            s->cpu.irq_recheck = false;
            check_interrupts(s);
        }
        /* If the firmware arms an EP0-IN reply after the setup window already
         * returned (e.g. a vendor dispatch delayed by serial ISR load), it
         * spins on the EP0CS busy bit waiting for a host read that will never
         * come through that window. On silicon the host always drains EP0 —
         * model that here so the late (already-missed) reply is consumed and
         * the firmware returns to its main loop. */
        if (s->ep[0].in_armed && (s->ep0cs & EP0CS_INBSY)) {
            s->cpu.events++;
            s->ep0cs &= ~EP0CS_INBSY;
            s->ep[0].in_armed = false;
        }
        int c = cpu8051_step(&s->cpu);
        total += c;
        s->total_cycles += c;
        s->steps++;
        total += an2131_idle_turn(s, max_cycles - total);
    }
    return total;
}
