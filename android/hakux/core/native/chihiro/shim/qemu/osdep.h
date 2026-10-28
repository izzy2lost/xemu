/*
 * hakuX native core — what xemu's Chihiro baseboard sources
 * (hw/xbox/chihiro: the 8051, the AN2131, JVS, the drive boards) take from
 * QEMU, so they build here unchanged. dev/chihiro.c supplies the rest.
 */
#ifndef NC_CHIHIRO_SHIM_OSDEP_H
#define NC_CHIHIRO_SHIM_OSDEP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif
/* Their reports (fprintf(stderr, ...)) go to the log (dev/chihiro.c). */
extern FILE *nc_chihiro_stderr;
#undef stderr
#define stderr nc_chihiro_stderr

#define QEMU_BUILD_BUG_ON(x) _Static_assert(!(x), #x)
#define qemu_fopen fopen

/* Wall-clock microseconds (GLib's; native/compat/glib has it). */
int64_t g_get_real_time(void);

static inline uint16_t lduw_le_p(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)(b[0] | b[1] << 8);
}
static inline void stw_le_p(void *p, uint16_t v)
{
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}
static inline void stl_le_p(void *p, uint32_t v)
{
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

#endif
