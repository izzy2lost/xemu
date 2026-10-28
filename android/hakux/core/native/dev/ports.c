/*
 * hakuX native core — x86 I/O ports.
 *
 * Devices claim ports as the frontier shows them used; the rest read as all
 * ones and drop writes, as unclaimed ports do on the bus. The first accesses
 * to each port are logged.
 *   0x8008  ACPI power-management timer: free-running at 3.375 MHz
 *   0x80C0  MCPX GPIO: bit 5 the video field, odd / even by turns at each
 *           vblank (the other bits as an unclaimed port's)
 */
#include "native/dev/dev.h"
#include "native/dev/usb.h"

#include <time.h>

#define ACPI_PM_TIMER 0x8008u
/* D3D's vblank DPC reads the field from bit 5 of this port into the
 * device (5849 LTCG: 0x4c07bd, [dev+0x1df8]), and GetDisplayFieldStatus
 * reports it. Read as all ones, every field was the same one, and the XMV
 * player, which starts a movie on the other field, never started
 * (Riddick's movies stayed black in push-buffer mode). */
#define MCPX_GPIO_FIELD 0x80C0u
#define FIELD_BIT       0x20u
#define LOGGED_PORTS  64

static struct {
    uint16_t port;
    unsigned count;
} g_seen[LOGGED_PORTS];

static void note(uint16_t port, bool out, unsigned bytes, uint32_t value)
{
    for (int i = 0; i < LOGGED_PORTS; i++) {
        if (g_seen[i].count && g_seen[i].port == port) {
            if (++g_seen[i].count <= 4) {
                break;
            }
            return;
        }
        if (!g_seen[i].count) {
            g_seen[i].port = port;
            g_seen[i].count = 1;
            break;
        }
    }
    nc_log("hakuX-native-io", "port %s %04x %s %0*x (%u)", out ? "out" : "in", port, out ? "<-" : "->",
           (int)(2 * bytes), value, bytes);
}

/* The guest's time (native/kernel/ke.c): it stands still while the
 * emulation is paused and goes on from a savestate's. */
uint64_t nc_ke_now_ns(void);

static uint32_t acpi_timer(void)
{
    const uint64_t ns = nc_ke_now_ns();
    return (uint32_t)((unsigned __int128)ns * 3375000u / 1000000000u);
}

uint32_t nc_port_in(uint16_t port, unsigned bytes)
{
    uint32_t v;
    switch (port) {
    case ACPI_PM_TIMER:
        return acpi_timer();
    case MCPX_GPIO_FIELD:
        v = (bytes == 4 ? 0xFFFFFFFFu : (1u << (8 * bytes)) - 1) & ~FIELD_BIT;
        return v | ((nc_dev_nv2a_vblanks() & 1) ? FIELD_BIT : 0);
    default:
        v = bytes == 4 ? 0xFFFFFFFFu : (1u << (8 * bytes)) - 1;
        if (nc_mediaboard_port_in(port, &v)) {
            v &= bytes == 4 ? 0xFFFFFFFFu : (1u << (8 * bytes)) - 1;
        }
        break;
    }
    note(port, false, bytes, v);
    return v;
}

void nc_port_out(uint16_t port, unsigned bytes, uint32_t value)
{
    note(port, true, bytes, value);
    nc_mediaboard_port_out(port, value);
}
