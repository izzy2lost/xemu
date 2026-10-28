/*
 * hakuX native core — USB device side: endpoint 0 control transfers and the
 * standard requests (USB 1.1, chapter 9).
 */
#include "native/dev/usb.h"

#define REQ_GET_STATUS        0x00
#define REQ_CLEAR_FEATURE     0x01
#define REQ_SET_FEATURE       0x03
#define REQ_SET_ADDRESS       0x05
#define REQ_GET_DESCRIPTOR    0x06
#define REQ_GET_CONFIGURATION 0x08
#define REQ_SET_CONFIGURATION 0x09
#define REQ_GET_INTERFACE     0x0A
#define REQ_SET_INTERFACE     0x0B

static int string_desc(const NcUsbDev *d, unsigned index, uint8_t *out, int cap)
{
    if (index == 0) {
        const uint8_t lang[4] = {4, 3, 0x09, 0x04}; /* en-US */
        memcpy(out, lang, 4);
        return 4;
    }
    if (index > d->nstrings || !d->strings[index - 1]) {
        return NC_USB_STALL;
    }
    const char *s = d->strings[index - 1];
    int n = 2;
    for (; *s && n + 2 <= cap; s++) {
        out[n++] = (uint8_t)*s;
        out[n++] = 0;
    }
    out[0] = (uint8_t)n;
    out[1] = 3;
    return n;
}

/* Standard requests; -3 if not one. IN results go to d->buf. */
static int standard(NcUsbDev *d, const uint8_t *s, int wlen)
{
    const uint8_t type = s[0], req = s[1];
    const uint16_t value = nc_rd16(s + 2);
    if ((type & 0x60) != 0) {
        return -3; /* class or vendor */
    }
    if (d->raw_ep0 && req != REQ_SET_ADDRESS) {
        return -3; /* the device's firmware answers */
    }
    switch (req) {
    case REQ_GET_DESCRIPTOR:
        if (type != 0x80) {
            return -3;
        }
        switch (value >> 8) {
        case 1:
            memcpy(d->buf, d->dev_desc, d->dev_desc_len);
            return (int)d->dev_desc_len;
        case 2:
            memcpy(d->buf, d->cfg_desc, d->cfg_desc_len);
            return (int)d->cfg_desc_len;
        case 3:
            return string_desc(d, value & 0xff, d->buf, sizeof(d->buf));
        default:
            return -3;
        }
    case REQ_SET_ADDRESS:
        d->pending_addr = value & 0x7f; /* takes effect after the status stage */
        return 0;
    case REQ_SET_CONFIGURATION:
        d->config = (uint8_t)value;
        nc_log("hakuX-native-usb", "%s@%u: configuration %u", d->name, d->addr, d->config);
        return 0;
    case REQ_GET_CONFIGURATION:
        d->buf[0] = d->config;
        return 1;
    case REQ_GET_STATUS:
        d->buf[0] = d->buf[1] = 0;
        return 2;
    case REQ_GET_INTERFACE:
        d->buf[0] = 0;
        return 1;
    case REQ_SET_INTERFACE:
    case REQ_CLEAR_FEATURE:
    case REQ_SET_FEATURE:
        return 0;
    default:
        return -3;
    }
}

int nc_usb_setup(NcUsbDev *d, const uint8_t setup[8])
{
    memcpy(d->setup, setup, 8);
    const int wlen = nc_rd16(setup + 6);
    d->in = setup[0] & 0x80;
    d->pos = 0;
    d->len = 0;
    d->stalled = false;
    d->pending_addr = -1;
    d->data_stage = wlen > 0;
    if (d->in || !wlen) {
        /* Requests without OUT data run now; IN data is ready for the data stage. */
        int r = standard(d, setup, wlen);
        if (r == -3) {
            r = d->control ? d->control(d, setup, d->buf, MIN(wlen, (int)sizeof(d->buf))) : NC_USB_STALL;
        }
        if (r < 0) {
            d->stalled = true;
            return NC_USB_STALL;
        }
        d->len = MIN(r, wlen);
    }
    return 8;
}

int nc_usb_ep0_in(NcUsbDev *d, uint8_t *buf, int len)
{
    if (d->stalled) {
        return NC_USB_STALL;
    }
    if (d->in && d->data_stage) {
        const int n = MIN(len, d->len - d->pos);
        memcpy(buf, d->buf + d->pos, n);
        d->pos += n;
        return n;
    }
    /* The status stage of an OUT request: run it with its data now. */
    if (!d->in && d->data_stage && d->len >= 0) {
        int r = standard(d, d->setup, d->len);
        if (r == -3) {
            r = d->control ? d->control(d, d->setup, d->buf, d->len) : NC_USB_STALL;
        }
        if (r < 0) {
            d->stalled = true;
            return NC_USB_STALL;
        }
    }
    if (d->pending_addr >= 0) {
        d->addr = (uint8_t)d->pending_addr;
        d->pending_addr = -1;
        nc_log("hakuX-native-usb", "%s: address %u", d->name, d->addr);
    }
    return 0;
}

int nc_usb_ep0_out(NcUsbDev *d, const uint8_t *buf, int len)
{
    if (d->stalled) {
        return NC_USB_STALL;
    }
    if (!d->in && d->data_stage) {
        const int n = MIN(len, (int)sizeof(d->buf) - d->len);
        memcpy(d->buf + d->len, buf, n);
        d->len += n;
        return n;
    }
    return 0; /* the status stage of an IN request */
}

void nc_usb_reset(NcUsbDev *d)
{
    d->addr = 0;
    d->config = 0;
    d->stalled = false;
    d->pending_addr = -1;
    if (d->reset) {
        d->reset(d);
    }
}

NcUsbDev *nc_usb_find(NcUsbDev *d, uint8_t addr)
{
    if (d->addr == addr) {
        return d;
    }
    return d->find ? d->find(d, addr) : NULL;
}
