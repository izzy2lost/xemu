/*
 * hakuX native core — executive, HAL, AV and title services.
 *
 * Ex pool and non-volatile (EEPROM) settings, XBE section loading, object
 * references, DbgPrint, the HAL and AV calls the titles make, and the DPC and
 * interrupt objects (recorded; their guest routines run once the core can
 * call into guest code).
 */
#include "native/kernel/kernel.h"
#include "native/core/xbe_load.h"
#include "native/core/xdvdfs.h"
#include "native/dev/dev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define POOL_HDR 16u  /* our header before each Ex pool block: its size, then POOL_PAGES */
#define POOL_PAGES 0x53454741u /* "AGES": a block of whole pages, not in the pool */

/* ------------------------------------------------------------------ pool */

/* A block of a page or more takes whole pages of its own and gives them back
 * when freed, as the kernel's pool does (MiAllocatePoolPages): in the pool,
 * a large temporary buffer grew it for good, and that memory never went
 * back to the title (JSRF's stage loads ran out of RAM). */
static uint32_t pool_alloc(NcKernel *k, uint32_t size)
{
    if (size + POOL_HDR > NC_PAGE) {
        const uint32_t len = nc_align_up(size + POOL_HDR, NC_PAGE);
        const uint32_t pa = nc_vm_alloc_phys_high(k->vm, len);
        if (pa != UINT32_MAX) {
            const uint32_t p = NC_CONTIG_WINDOW | pa;
            memset(nc_vm_host(k->vm, p), 0, len);
            nc_vm_wr32(k->vm, p, size);
            nc_vm_wr32(k->vm, p + 4, POOL_PAGES);
            k->pool_ex += len;
            return p + POOL_HDR;
        }
    }
    uint32_t p = nc_pool_alloc(k, size + POOL_HDR);
    if (!p) {
        return 0;
    }
    nc_vm_wr32(k->vm, p, size);
    k->pool_ex += size + POOL_HDR;
    return p + POOL_HDR;
}

/* ExAllocatePool(NumberOfBytes) */
static uint64_t ExAllocatePool(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return pool_alloc(k, a[0]);
}

/* ExAllocatePoolWithTag(NumberOfBytes, Tag) */
static uint64_t ExAllocatePoolWithTag(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return pool_alloc(k, a[0]);
}

/* ExFreePool(P) */
static uint64_t ExFreePool(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    if (a[0]) {
        const uint32_t hdr = a[0] - POOL_HDR;
        const uint32_t size = nc_vm_rd32(k->vm, hdr) + POOL_HDR;
        if (nc_vm_rd32(k->vm, hdr + 4) == POOL_PAGES) {
            const uint32_t len = nc_align_up(size, NC_PAGE);
            nc_vm_wr32(k->vm, hdr + 4, 0);
            k->pool_ex -= len;
            nc_vm_free_phys(k->vm, hdr & ~NC_CONTIG_WINDOW, len);
        } else {
            k->pool_ex -= size;
            nc_pool_free(k, hdr, size);
        }
    }
    return 0;
}

/* ExQueryPoolBlockSize(PoolBlock) */
static uint64_t ExQueryPoolBlockSize(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return nc_vm_rd32(k->vm, a[0] - POOL_HDR);
}

/* ------------------------------------------------- non-volatile settings */

/* The console's EEPROM (256 bytes), kept as <hdd>/eeprom.bin so it stays the
 * same from boot to boot, like the chip: titles sign saves with keys derived
 * from it (XAPI reads the whole image, XC_MAX_ALL, for console-bound
 * signatures), and a different one each boot makes every save "damaged".
 * Every setting is served from this one image. Layout: encrypted section
 * (hash 0x00, confounder 0x14, HD key 0x1C, game region 0x2C), factory
 * section (checksum 0x30, serial 0x34, MAC 0x40, online key 0x48, AV region
 * 0x58), user section (checksum 0x60, time zone 0x64-0x8F, language 0x90,
 * video 0x94, audio 0x98, parental games 0x9C / password 0xA0 / movies 0xA4,
 * online addresses 0xA8-0xB7, misc 0xB8, DVD region 0xBC). */
#define EEPROM_SIZE 0x100u
#define REG_BINARY 3
#define REG_DWORD 4
#define XC_MAX_OS 0xFFu                 /* the user section, whole */
#define XC_MAX_FACTORY 0x1FFu           /* the factory section, whole */
#define XC_ENCRYPTED_SECTION 0xFFFEu
#define XC_MAX_ALL 0xFFFFu

typedef struct NvSetting {
    uint16_t index, offset, size;
} NvSetting;

static const NvSetting k_nv[] = {
    {0x00, 0x64, 4}, {0x01, 0x68, 4},  {0x02, 0x6C, 4},  {0x03, 0x78, 4},  {0x04, 0x7C, 4},
    {0x05, 0x88, 4}, {0x06, 0x8C, 4},  {0x07, 0x90, 4},  {0x08, 0x94, 4},  {0x09, 0x98, 4},
    {0x0A, 0x9C, 4}, {0x0B, 0xA0, 4},  {0x0C, 0xA4, 4},  {0x0D, 0xA8, 4},  {0x0E, 0xAC, 4},
    {0x0F, 0xB0, 4}, {0x10, 0xB4, 4},  {0x11, 0xB8, 4},  {0x12, 0xBC, 4},
    {0x100, 0x34, 12}, {0x101, 0x40, 6}, {0x102, 0x48, 16}, {0x103, 0x58, 4}, {0x104, 0x2C, 4},
    /* the sections whole: Splinter Cell Chaos Theory reads the user section
     * (XC_MAX_OS) and, refused, asked again and again */
    {XC_MAX_OS, 0x60, 0x60}, {XC_MAX_FACTORY, 0x30, 0x30},
    {XC_ENCRYPTED_SECTION, 0x00, 0x30}, {XC_MAX_ALL, 0x00, EEPROM_SIZE},
};

static uint8_t g_eeprom[EEPROM_SIZE];
static bool g_eeprom_ready;

/* The app's video settings (nc_kernel_set_video). */
static int64_t g_video_flags = -1;
static int g_av_pack = NC_AV_PACK_HDTV;

void nc_kernel_set_video(int64_t flags, int av_pack)
{
    g_video_flags = flags;
    g_av_pack = av_pack;
}

/* The app's language (nc_kernel_set_language); 0 keeps the stored one. */
static int g_language;

void nc_kernel_set_language(int language)
{
    g_language = language >= 1 && language <= 9 ? language : 0;
}

/* The app's time zone (nc_kernel_set_time_zone): minutes east of UTC. */
static int g_tz_minutes;

void nc_kernel_set_time_zone(int minutes_east)
{
    g_tz_minutes = minutes_east;
}

/* The Xbox EEPROM section checksum: ones' complement of the carry-folded sum. */
static uint32_t eeprom_checksum(const uint8_t *p, unsigned len)
{
    uint32_t high = 0, low = 0;
    for (unsigned i = 0; i + 4 <= len; i += 4) {
        const uint32_t v = nc_rd32(p + i);
        high += (uint32_t)(((uint64_t)low + v) >> 32);
        low += v;
    }
    return ~(high + low);
}

static void eeprom_seal(void)
{
    nc_wr32(g_eeprom + 0x30, eeprom_checksum(g_eeprom + 0x34, 0x2C));
    nc_wr32(g_eeprom + 0x60, eeprom_checksum(g_eeprom + 0x64, 0x5C));
}

static void eeprom_path(char *p, size_t len)
{
    snprintf(p, len, "%s/eeprom.bin", nc_io_hdd_root());
}

/* A new console's EEPROM: a fixed serial and MAC, English, stereo, no
 * parental limits, the region of the first title it runs. */
static void eeprom_create(NcKernel *k)
{
    const bool pal = k->game_region == 4;
    memset(g_eeprom, 0, sizeof(g_eeprom));
    nc_wr32(g_eeprom + 0x2C, k->game_region ? k->game_region : 1);
    memcpy(g_eeprom + 0x34, "000000000001", 12);
    static const uint8_t mac[6] = {0x00, 0x50, 0xF2, 0x00, 0x00, 0x01};
    memcpy(g_eeprom + 0x40, mac, sizeof(mac));
    nc_wr32(g_eeprom + 0x58, pal ? 0x00800300u : 0x00400100u); /* PAL-I / NTSC-M */
    nc_wr32(g_eeprom + 0x90, 1);                                 /* English */
    nc_wr32(g_eeprom + 0xBC, pal ? 2 : 1);                       /* DVD region */
    eeprom_seal();
}

static void eeprom_store(void)
{
    char p[320];
    eeprom_path(p, sizeof(p));
    FILE *out = fopen(p, "wb");
    if (!out || fwrite(g_eeprom, 1, sizeof(g_eeprom), out) != sizeof(g_eeprom)) {
        nc_log("hakuX-native", "ex: can't keep the EEPROM in %s", p);
    }
    if (out) {
        fflush(out);
        fsync(fileno(out));
        fclose(out);
    }
}

static uint8_t *eeprom(NcKernel *k)
{
    if (!g_eeprom_ready) {
        char p[320];
        eeprom_path(p, sizeof(p));
        FILE *in = fopen(p, "rb");
        const bool have = in && fread(g_eeprom, 1, sizeof(g_eeprom), in) == sizeof(g_eeprom);
        if (in) {
            fclose(in);
        }
        if (!have) {
            eeprom_create(k);
            eeprom_store();
            nc_log("hakuX-native", "ex: new console EEPROM in %s", p);
        }
        /* Every console got the same MAC (eeprom_create's, 00:50:F2:00:00:01):
         * two on one LAN must differ for System Link, so this one takes a
         * random one of the prefix, kept. Saves are signed with the HD key,
         * not the MAC: they stay valid. */
        static const uint8_t k_shared_mac[6] = {0x00, 0x50, 0xF2, 0x00, 0x00, 0x01};
        if (!memcmp(g_eeprom + 0x40, k_shared_mac, sizeof(k_shared_mac))) {
            do {
                arc4random_buf(g_eeprom + 0x43, 3);
            } while (!memcmp(g_eeprom + 0x40, k_shared_mac, sizeof(k_shared_mac)));
            eeprom_seal();
            eeprom_store();
            nc_log("hakuX-native", "ex: this console's MAC is now %02x:%02x:%02x:%02x:%02x:%02x", g_eeprom[0x40],
                   g_eeprom[0x41], g_eeprom[0x42], g_eeprom[0x43], g_eeprom[0x44], g_eeprom[0x45]);
        }
        /* The regions follow the running title (a library mixes NTSC and
         * PAL discs); the rest stays as stored. A title always sees the
         * same image. */
        const bool pal = k->game_region == 4;
        nc_wr32(g_eeprom + 0x2C, k->game_region ? k->game_region : 1);
        nc_wr32(g_eeprom + 0x58, pal ? 0x00800300u : 0x00400100u);
        nc_wr32(g_eeprom + 0xBC, pal ? 2 : 1);
        /* So do the video settings: the app's (widescreen, 480p...). */
        if (g_video_flags >= 0) {
            nc_wr32(g_eeprom + 0x94, (uint32_t)g_video_flags);
        }
        if (g_language) {
            nc_wr32(g_eeprom + 0x90, (uint32_t)g_language);
        }
        /* And the time zone, the phone's: its offset from UTC now as the
         * bias (UTC = local time + bias, minutes), without daylight saving
         * rules of its own (no names, dates or standard / daylight biases). */
        nc_wr32(g_eeprom + 0x64, (uint32_t)-g_tz_minutes);
        memset(g_eeprom + 0x68, 0, 8);
        memset(g_eeprom + 0x78, 0, 8);
        memset(g_eeprom + 0x88, 0, 8);
        eeprom_seal();
        g_eeprom_ready = true;
        nc_log("hakuX-native", "ex: video flags %08x, AV pack %d, language %u, time zone bias %d min",
               nc_rd32(g_eeprom + 0x94), g_av_pack, nc_rd32(g_eeprom + 0x90), (int32_t)nc_rd32(g_eeprom + 0x64));
    }
    return g_eeprom;
}

static const NvSetting *nv_find(uint32_t index)
{
    static int no_sections = -1;        /* debug: NC_NV_NO_SECTIONS=1, XC_MAX_OS / FACTORY unknown (A/B) */
    if (no_sections < 0) {
        no_sections = getenv("NC_NV_NO_SECTIONS") != NULL;
    }
    if (no_sections && (index == XC_MAX_OS || index == XC_MAX_FACTORY)) {
        return NULL;
    }
    for (unsigned i = 0; i < sizeof(k_nv) / sizeof(k_nv[0]); i++) {
        if (k_nv[i].index == index) {
            return &k_nv[i];
        }
    }
    return NULL;
}

/* ExQueryNonVolatileSetting(ValueIndex, Type, Value, ValueLength, ResultLength) */
static uint64_t ExQueryNonVolatileSetting(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const NvSetting *s = nv_find(a[0]);
    if (!s) {
        nc_log("hakuX-native", "ex: non-volatile setting 0x%x not known", a[0]);
        return 0xC0000034u; /* STATUS_OBJECT_NAME_NOT_FOUND */
    }
    if (a[1]) {
        nc_vm_wr32(k->vm, a[1], s->size == 4 ? REG_DWORD : REG_BINARY);
    }
    if (a[4]) {
        nc_vm_wr32(k->vm, a[4], s->size);
    }
    if (a[3] < s->size) {
        return 0xC0000023u; /* STATUS_BUFFER_TOO_SMALL */
    }
    memcpy(nc_vm_host(k->vm, a[2]), eeprom(k) + s->offset, s->size);
    return NC_STATUS_SUCCESS;
}

/* ExSaveNonVolatileSetting(ValueIndex, Type, Value, ValueLength): the user
 * section (and the factory AV region, which the dashboard may set) only. */
static uint64_t ExSaveNonVolatileSetting(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const NvSetting *s = nv_find(a[0]);
    if (!s || s->index > 0x103 || (s->index >= 0x100 && s->index != 0x103)) {
        return 0xC0000022u; /* STATUS_ACCESS_DENIED */
    }
    uint8_t *e = eeprom(k);
    const uint32_t n = a[3] < s->size ? a[3] : s->size;
    memset(e + s->offset, 0, s->size);
    memcpy(e + s->offset, nc_vm_host(k->vm, a[2]), n);
    eeprom_seal();
    eeprom_store();
    return NC_STATUS_SUCCESS;
}

/* ---------------------------------------------------------- XBE sections */

/* XBE section header: Flags 0x00, VirtualAddress 0x04, VirtualSize 0x08, FileAddress 0x0C,
 * FileSize 0x10, SectionName 0x14, SectionReferenceCount 0x18. */
/* A section header's name (SectionName 0x14), for the logs. */
static const char *section_name(NcKernel *k, uint32_t s)
{
    const uint32_t name = nc_vm_rd32(k->vm, s + 0x14);
    return name && nc_vm_phys(k->vm, name) != UINT32_MAX ? (const char *)nc_vm_host(k->vm, name) : "?";
}

static uint64_t XeLoadSection(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t s = a[0];
    uint32_t refs = nc_vm_rd32(k->vm, s + 0x18);
    nc_vm_wr32(k->vm, s + 0x18, refs + 1);
    const uint32_t va = nc_vm_rd32(k->vm, s + 0x04), vsize = nc_vm_rd32(k->vm, s + 0x08);
    nc_log("hakuX-native", "ex: load section %.8s %08x+%x (refs %u)", section_name(k, s), va, vsize, refs + 1);
    if (refs) {
        return NC_STATUS_SUCCESS;
    }
    const uint32_t raw = nc_vm_rd32(k->vm, s + 0x0C), rsize = nc_vm_rd32(k->vm, s + 0x10);
    if (!nc_xbe_back(k->vm, va, vsize)) {
        nc_vm_wr32(k->vm, s + 0x18, 0);
        return NC_STATUS_NO_MEMORY;
    }
    uint8_t *dst = nc_vm_host(k->vm, va);
    memset(dst, 0, vsize);
    if (!nc_disc_read(k->disc, k->xbe_file, raw, dst, rsize < vsize ? rsize : vsize)) {
        return 0xC000009Cu; /* STATUS_DEVICE_DATA_ERROR */
    }
    nc_cpu_invalidate(k->cpu, va, vsize);
    return NC_STATUS_SUCCESS;
}

/* An unloaded section's pages go back, except those it shares with the
 * headers or with a section still loaded. */
static void section_release(NcKernel *k, uint32_t s, uint32_t va, uint32_t vsize)
{
    const uint32_t base = k->xbe_base, hdr_end = base + nc_vm_rd32(k->vm, base + 0x108);
    const uint32_t nsec = nc_vm_rd32(k->vm, base + 0x11C), secs = nc_vm_rd32(k->vm, base + 0x120);
    for (uint64_t p = va & ~(NC_PAGE - 1); p < (uint64_t)va + vsize; p += NC_PAGE) {
        bool shared = p < hdr_end;
        for (uint32_t i = 0; i < nsec && !shared; i++) {
            const uint32_t o = secs + i * 0x38;
            if (o == s || !nc_vm_rd32(k->vm, o + 0x18)) {
                continue;
            }
            const uint32_t ova = nc_vm_rd32(k->vm, o + 0x04), osize = nc_vm_rd32(k->vm, o + 0x08);
            shared = ova < p + NC_PAGE && (uint64_t)ova + osize > p;
        }
        const uint32_t pa = nc_vm_phys(k->vm, (uint32_t)p);
        if (!shared && pa != UINT32_MAX) {
            nc_vm_unmap(k->vm, (uint32_t)p, NC_PAGE);
            nc_vm_free_phys(k->vm, pa, NC_PAGE);
        }
    }
}

static uint64_t XeUnloadSection(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t s = a[0];
    uint32_t refs = nc_vm_rd32(k->vm, s + 0x18);
    if (!refs) {
        return NC_STATUS_INVALID_PARAMETER;
    }
    nc_vm_wr32(k->vm, s + 0x18, refs - 1);
    nc_log("hakuX-native", "ex: unload section %.8s (refs %u)", section_name(k, s), refs - 1);
    if (refs == 1) {
        const uint32_t va = nc_vm_rd32(k->vm, s + 0x04), vsize = nc_vm_rd32(k->vm, s + 0x08);
        section_release(k, s, va, vsize);
        nc_cpu_invalidate(k->cpu, va, vsize);
    }
    return NC_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- objects */

#define NT_CURRENT_THREAD 0xFFFFFFFEu

/* ObReferenceObjectByHandle(Handle, ObjectType, ReturnedObject) */
static uint64_t ObReferenceObjectByHandle(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t obj;
    if (a[0] == NT_CURRENT_THREAD) {
        obj = k->current->ethread;
    } else {
        obj = nc_ob_lookup(k, a[0], NULL);
        if (!obj) {
            return NC_STATUS_INVALID_HANDLE;
        }
    }
    const uint32_t hdr = obj - 0x10;
    if (a[1] && nc_vm_rd32(k->vm, hdr + 8) != a[1]) {
        return NC_STATUS_OBJECT_TYPE_MISMATCH;
    }
    nc_vm_wr32(k->vm, hdr, nc_vm_rd32(k->vm, hdr) + 1);
    nc_vm_wr32(k->vm, a[2], obj);
    return NC_STATUS_SUCCESS;
}

/* ObfDereferenceObject(Object) */
static uint64_t ObfDereferenceObject(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_ob_dereference(k, a[0]);
    return 0;
}

/* ------------------------------------------------------------------- Dbg */

/* The guest's string at va in buf (at most len - 1 bytes), as far as its
 * pages are mapped: a title's bad pointer ends it rather than faulting the
 * host. */
static const char *guest_str(NcKernel *k, uint32_t va, char *buf, size_t len)
{
    size_t n = 0;
    for (; n + 1 < len; n++) {
        if ((!n || !((va + n) & (NC_PAGE - 1))) && nc_vm_phys(k->vm, va + (uint32_t)n) == UINT32_MAX) {
            break;
        }
        const char c = *(const char *)nc_vm_host(k->vm, va + (uint32_t)n);
        if (!c) {
            break;
        }
        buf[n] = c;
    }
    buf[n] = 0;
    return buf;
}

/* DbgPrint(Format, ...): the common printf conversions, arguments from the guest stack. */
static uint64_t DbgPrint(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t esp = nc_frame_gpr(f, NC_ESP);
    uint32_t argp = esp + 8; /* after the return address and Format */
    char fmtbuf[512], arg[256];
    const char *fmt = guest_str(k, a[0], fmtbuf, sizeof(fmtbuf));
    char out[512];
    size_t n = 0;
    for (const char *p = fmt; *p && n < sizeof(out) - 32; p++) {
        if (*p != '%') {
            out[n++] = *p;
            continue;
        }
        char spec[16] = "%";
        size_t s = 1;
        p++;
        while (*p && strchr("-+ #0123456789.lh", *p) && s < sizeof(spec) - 2) {
            if (*p != 'l' && *p != 'h') {
                spec[s++] = *p;
            }
            p++;
        }
        if (!*p) {
            break;
        }
        spec[s++] = *p;
        spec[s] = 0;
        uint32_t v = nc_vm_rd32(k->vm, argp);
        switch (*p) {
        case 's': n += (size_t)snprintf(out + n, sizeof(out) - n, spec, v ? guest_str(k, v, arg, sizeof(arg)) : "(null)"); argp += 4; break;
        case 'c': case 'd': case 'i': case 'u': case 'x': case 'X':
            n += (size_t)snprintf(out + n, sizeof(out) - n, spec, v); argp += 4; break;
        case 'p': n += (size_t)snprintf(out + n, sizeof(out) - n, "%08x", v); argp += 4; break;
        case '%': out[n++] = '%'; break;
        default: n += (size_t)snprintf(out + n, sizeof(out) - n, "<%s>", spec); argp += 4; break;
        }
    }
    n = n < sizeof(out) ? n : sizeof(out) - 1;     /* snprintf counts what did not fit */
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
        n--;
    }
    out[n] = 0;
    nc_log("hakuX-native-dbg", "%s", out);
    return 0;
}

static uint64_t DbgBreakPoint(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_log("hakuX-native-dbg", "DbgBreakPoint from %08x", nc_vm_rd32(k->vm, nc_frame_gpr(f, NC_ESP)));
    return 0;
}

/* ------------------------------------------------------------- HAL, AV */

static uint32_t g_av_saved;

uint32_t nc_kernel_av_saved(void)
{
    return g_av_saved;
}

void nc_kernel_av_saved_set(uint32_t va)
{
    g_av_saved = va;
}

static uint64_t AvGetSavedDataAddress(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    static unsigned s_logged;
    if (s_logged++ < 4) {
        nc_log("hakuX-native", "AvGetSavedDataAddress: %08x", g_av_saved);
    }
    return g_av_saved;
}

static uint64_t AvSetSavedDataAddress(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_log("hakuX-native", "AvSetSavedDataAddress %08x", a[0]);
    g_av_saved = a[0];
    return 0;
}

#define AV_QUERY_AV_CAPABILITIES 6u
#define AV_STANDARD_MASK         0x00000F00u
#define AV_REFRESH_MASK          0x00C00000u

/* The AV pack as the TV encoder's capabilities (AV_PACK_*) and as the system
 * controller's AV pack register (SMC 0x04) report it. */
static uint32_t av_pack_caps(void)
{
    switch (g_av_pack) {
    case NC_AV_PACK_STANDARD: return 1;
    case NC_AV_PACK_SCART: return 3;
    case NC_AV_PACK_VGA: return 5;
    case NC_AV_PACK_SVIDEO: return 6;
    default: return 4; /* HDTV (component) */
    }
}

static uint32_t av_pack_smc(void)
{
    switch (g_av_pack) {
    case NC_AV_PACK_STANDARD: return 0x06;
    case NC_AV_PACK_SCART: return 0x00;
    case NC_AV_PACK_VGA: return 0x02;
    case NC_AV_PACK_SVIDEO: return 0x04;
    default: return 0x01; /* HDTV (component) */
    }
}

/* AvSendTVEncoderOption(RegisterBase, Option, Param, Result) */
static uint64_t AvSendTVEncoderOption(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t r = 0;
    if (a[1] == AV_QUERY_AV_CAPABILITIES) {
        /* The AV pack (the app's: component by default), the factory video
         * standard and refresh, and the user's video flags (480p, 720p,
         * widescreen...): D3D lists its display modes from these. */
        const uint8_t *e = eeprom(k);
        r = av_pack_caps() | (nc_rd32(e + 0x58) & (AV_STANDARD_MASK | AV_REFRESH_MASK)) |
            (nc_rd32(e + 0x94) & ~(AV_STANDARD_MASK | 0xFFu));
    }
    static uint32_t s_logged;
    if (!(s_logged & (1u << (a[1] & 31)))) {
        s_logged |= 1u << (a[1] & 31);
        nc_log("hakuX-native", "av: TV encoder option %u param %08x -> %08x", a[1], a[2], r);
    }
    if (a[3]) {
        nc_vm_wr32(k->vm, a[3], r);
    }
    return 0;
}

/* AvSetDisplayMode(RegisterBase, Step, Mode, Format, Pitch, FrameBuffer) */
static uint64_t AvSetDisplayMode(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_log("hakuX-native", "av: display mode %08x format %08x pitch %u fb %08x", a[2], a[3], a[4], a[5]);
    if (a[2]) {
        /* AV mode bit 30: the 50 Hz modes (PAL 576-line timings, as
         * 0x58030314 is); the 480-line, 480p and HD modes are 60 Hz. */
        nc_dev_nv2a_refresh(a[2] & 0x40000000u ? 50 : 60);
        nc_dev_nv2a_display_mode(a[4], a[3], a[2] & 0x40000000u ? 50 : 60);
    }
    return 0;
}

/* HalReadWritePCISpace(BusNumber, SlotNumber, RegisterNumber, Buffer, Length, WritePCISpace) */
static uint64_t HalReadWritePCISpace(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_log("hakuX-native", "hal: PCI %s bus %u slot %u reg %02x len %u", a[5] & 0xff ? "write" : "read", a[0], a[1],
           a[2], a[4]);
    if (!(a[5] & 0xff)) {
        memset(nc_vm_host(k->vm, a[3]), 0, a[4]);
    }
    return 0;
}

static uint64_t HalNothing(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return 0;
}

/* PhyGetLinkState(Update): XNET_ETHERNET_LINK_* flags, a 100 Mb/s full-duplex
 * link while System Link has a wire (nvnet.c), else 0: the cable is out. */
static uint64_t PhyGetLinkState(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t state = nc_dev_nvnet_link_up() ? 0x01u | 0x02u | 0x08u : 0; /* ACTIVE | 100MBPS | FULL_DUPLEX */
    static bool logged;
    if (!logged) {
        logged = true;
        nc_log("hakuX-native", "phy: the title asks for the link: %s", state ? "up, 100 Mb/s full duplex" : "down");
    }
    return state;
}

static uint32_t data_va(NcKernel *k, const char *name);

void nc_kernel_relaunch_file(char *out, size_t n)
{
    snprintf(out, n, "%s/../relaunch.bin", nc_io_hdd_root());
}

/* LAUNCH_DATA_PAGE: Header { LaunchDataType, TitleId, szLaunchPath[520], Flags },
 * then LaunchData at +0x400. XAPI's XLaunchNewImage stores the path as the
 * device directory, ';', the file name ("\Device\CdRom0;morrowind.xbe"). */
#define LAUNCH_PATH_OFF 8
#define LAUNCH_PATH_LEN 520

bool nc_kernel_launch_xbe(const uint8_t page[NC_LAUNCH_PAGE], char *out, size_t n)
{
    char p[LAUNCH_PATH_LEN + 1];
    memcpy(p, page + LAUNCH_PATH_OFF, LAUNCH_PATH_LEN);
    p[LAUNCH_PATH_LEN] = 0;
    static const char cd[] = "\\Device\\CdRom0";
    if (strncasecmp(p, cd, sizeof(cd) - 1) != 0) {
        return false;
    }
    const char *rest = p + sizeof(cd) - 1;
    size_t m = 0;
    if (*rest != '\\' && *rest != ';' && m + 1 < n) {
        out[m++] = '\\';
    }
    for (; *rest && m + 1 < n; rest++) {
        out[m++] = *rest == ';' ? '\\' : *rest;
    }
    out[m] = 0;
    return m > 1;
}

void nc_kernel_launch_page_restore(NcKernel *k, const uint8_t page[NC_LAUNCH_PAGE])
{
    const uint32_t slot = data_va(k, "LaunchDataPage");
    const uint32_t va = slot ? nc_mm_contig_alloc(k, NC_LAUNCH_PAGE) : 0;
    if (!va) {
        nc_log("hakuX-native", "relaunch: no launch data page for the title");
        return;
    }
    memcpy(nc_vm_host(k->vm, va), page, NC_LAUNCH_PAGE);
    nc_vm_wr32(k->vm, slot, va);
}

/* HalReturnToFirmware(Routine): 2 (ReturnFirmwareQuickReboot) with a launch
 * data page is XLaunchNewImage. The core can't reboot in place (its state
 * lives for one run), so the page goes to a file and the app starts the core
 * again (NativeCoreActivity), which boots the XBE the page names. */
static uint64_t HalReturnToFirmware(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t slot = data_va(k, "LaunchDataPage");
    const uint32_t page = slot ? nc_vm_rd32(k->vm, slot) : 0;
    char xbe[LAUNCH_PATH_LEN] = "";
    if (a[0] == 2 && page && nc_kernel_launch_xbe(nc_vm_host(k->vm, page), xbe, sizeof(xbe))) {
        char path[512];
        nc_kernel_relaunch_file(path, sizeof(path));
        FILE *fp = fopen(path, "wb");
        const bool ok = fp && fwrite(nc_vm_host(k->vm, page), 1, NC_LAUNCH_PAGE, fp) == NC_LAUNCH_PAGE;
        if (fp) {
            fclose(fp);
        }
        if (ok) {
            k->relaunch = true;
            nc_mm_keep_save(k, g_av_saved);
            nc_kernel_request_stop(k, f, "relaunch into %s (launch type %u)", xbe, nc_vm_rd32(k->vm, page));
            return 0;
        }
        nc_log("hakuX-native", "relaunch: can't write %s", path);
    }
    char launch[LAUNCH_PATH_LEN + 1] = "";
    if (page) {
        memcpy(launch, nc_vm_host(k->vm, page + LAUNCH_PATH_OFF), LAUNCH_PATH_LEN);
    }
    /* the title is done (the dashboard, a reboot): the game closes */
    k->quit = true;
    nc_kernel_request_stop(k, f, "title returned to the firmware (routine %u%s%s)", a[0],
                           page ? ", launch path " : "", launch);
    return 0;
}

static uint64_t HalInitiateShutdown(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    k->quit = true;
    nc_kernel_request_stop(k, f, "title shut the console down");
    return 0;
}

/* HalReadSMBusValue(Address, Command, ReadWord, DataValue): SMC version, tray, AV pack. */
static uint64_t HalReadSMBusValue(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    static const char version[3] = {'P', '0', '1'};
    static unsigned version_idx;
    uint32_t v = 0;
    if ((a[0] & 0xFE) == 0x20) {
        switch (a[1]) {
        case 0x01: v = (uint8_t)version[version_idx++ % 3]; break;
        case 0x03: v = 0x60; break; /* tray closed, media present */
        case 0x04: v = av_pack_smc(); break; /* the AV pack, as the TV encoder reports it */
        default: break;
        }
    }
    if (a[3]) {
        nc_vm_wr32(k->vm, a[3], v);
    }
    return NC_STATUS_SUCCESS;
}

/* ------------------------------------------------- caches, network PHY */

static uint64_t FscGetCacheSize(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return 16;
}

static uint64_t KeBugCheck(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_kernel_request_stop(k, f, "KeBugCheck(%08x) from %08x", a[0], nc_vm_rd32(k->vm, nc_frame_gpr(f, NC_ESP)));
    return 0;
}

/* ----------------------------------------------------------- DPC objects */

/* KDPC: Type 0x00, Inserted 0x02, DpcListEntry 0x04, DeferredRoutine 0x0C, DeferredContext 0x10,
 * SystemArgument1 0x14, SystemArgument2 0x18. */
static uint64_t KeInitializeDpc(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint8_t *d = nc_vm_host(k->vm, a[0]);
    memset(d, 0, 0x1C);
    d[0] = 19; /* DpcObject */
    nc_vm_wr32(k->vm, a[0] + 0x0C, a[1]);
    nc_vm_wr32(k->vm, a[0] + 0x10, a[2]);
    return 0;
}

/* KeSaveFloatingPointState / KeRestoreFloatingPointState: guest FPU state stays put. */
static uint64_t KeFloatingPointState(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return NC_STATUS_SUCCESS;
}

/* ----------------------------------------------------- title information */

static uint32_t data_va(NcKernel *k, const char *name)
{
    for (unsigned o = 1; o < NC_KORDINAL_MAX; o++) {
        const NcKExport *e = nc_kernel_export(o);
        if (e && e->abi == NC_KABI_DATA && strcmp(e->name, name) == 0) {
            return k->data_va[o];
        }
    }
    return 0;
}

void nc_kernel_title_loaded(NcKernel *k, const NcDisc *disc, const NcDiscFile *xbe, uint32_t xbe_base,
                            uint32_t image_size)
{
    k->disc = disc;
    k->xbe_file = xbe;
    k->xbe_base = xbe_base;
    k->xbe_size = image_size;
    nc_mm_note_image(xbe_base, image_size);

    /* Certificate (header 0x118): GameRegion at 0xA0. */
    const uint32_t cert = nc_vm_rd32(k->vm, xbe_base + 0x118);
    const uint32_t region = nc_vm_rd32(k->vm, cert + 0xA0);
    k->game_region = region & 1 ? 1 : region & 2 ? 2 : region & 4 ? 4 : 1;

    /* XeImageFileName: ANSI_STRING { Length, MaximumLength, Buffer }. */
    const char *path = k->image_path[0] ? k->image_path : "\\Device\\CdRom0\\default.xbe";
    const uint32_t len = (uint32_t)strlen(path) + 1;
    uint32_t buf = nc_pool_alloc(k, len);
    memcpy(nc_vm_host(k->vm, buf), path, len);
    uint32_t s = data_va(k, "XeImageFileName");
    if (s) {
        *(uint16_t *)nc_vm_host(k->vm, s) = (uint16_t)(len - 1);
        *(uint16_t *)nc_vm_host(k->vm, s + 2) = (uint16_t)len;
        nc_vm_wr32(k->vm, s + 4, buf);
    }
    nc_kernel_set_hd_key(k);
}

void nc_kernel_set_hd_key(NcKernel *k)
{
    const uint32_t va = data_va(k, "XboxHDKey");
    if (va) {
        memcpy(nc_vm_host(k->vm, va), eeprom(k) + 0x36, 16);
    }
}

void nc_ex_register(NcKernel *k)
{
    nc_kernel_impl(k, "ExAllocatePool", ExAllocatePool);
    nc_kernel_impl(k, "ExAllocatePoolWithTag", ExAllocatePoolWithTag);
    nc_kernel_impl(k, "ExFreePool", ExFreePool);
    nc_kernel_impl(k, "ExQueryPoolBlockSize", ExQueryPoolBlockSize);
    nc_kernel_impl(k, "ExQueryNonVolatileSetting", ExQueryNonVolatileSetting);
    nc_kernel_impl(k, "ExSaveNonVolatileSetting", ExSaveNonVolatileSetting);
    nc_kernel_impl(k, "XeLoadSection", XeLoadSection);
    nc_kernel_impl(k, "XeUnloadSection", XeUnloadSection);
    nc_kernel_impl(k, "ObReferenceObjectByHandle", ObReferenceObjectByHandle);
    nc_kernel_impl(k, "ObfDereferenceObject", ObfDereferenceObject);
    nc_kernel_impl(k, "DbgPrint", DbgPrint);
    nc_kernel_impl(k, "DbgBreakPoint", DbgBreakPoint);
    nc_kernel_impl(k, "AvGetSavedDataAddress", AvGetSavedDataAddress);
    nc_kernel_impl(k, "AvSetSavedDataAddress", AvSetSavedDataAddress);
    nc_kernel_impl(k, "AvSendTVEncoderOption", AvSendTVEncoderOption);
    nc_kernel_impl(k, "AvSetDisplayMode", AvSetDisplayMode);
    nc_kernel_impl(k, "HalReadWritePCISpace", HalReadWritePCISpace);
    nc_kernel_impl(k, "HalRegisterShutdownNotification", HalNothing);
    nc_kernel_impl(k, "HalIsResetOrShutdownPending", HalNothing);
    nc_kernel_impl(k, "HalReturnToFirmware", HalReturnToFirmware);
    nc_kernel_impl(k, "HalInitiateShutdown", HalInitiateShutdown);
    nc_kernel_impl(k, "HalReadSMBusValue", HalReadSMBusValue);
    nc_kernel_impl(k, "HalWriteSMBusValue", HalNothing);
    nc_kernel_impl(k, "FscGetCacheSize", FscGetCacheSize);
    nc_kernel_impl(k, "FscSetCacheSize", HalNothing);
    nc_kernel_impl(k, "PhyGetLinkState", PhyGetLinkState);
    nc_kernel_impl(k, "PhyInitialize", HalNothing);     /* STATUS_SUCCESS: the PHY is ready (nvnet.c) */
    nc_kernel_impl(k, "KeBugCheck", KeBugCheck);
    nc_kernel_impl(k, "KeStallExecutionProcessor", HalNothing);
    nc_kernel_impl(k, "KeSaveFloatingPointState", KeFloatingPointState);
    nc_kernel_impl(k, "KeRestoreFloatingPointState", KeFloatingPointState);
    nc_kernel_impl(k, "KeInitializeDpc", KeInitializeDpc);
}
