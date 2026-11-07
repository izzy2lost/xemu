/*
 * hakuX HLE GPU — Vulkan driver quirks (see quirks.h).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/vk/quirks.h"

#include "hw/xbox/hle/hle.h"

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

uint32_t g_hle_quirks;

typedef struct HleDriver {
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceDriverProperties drv;
} HleDriver;

typedef struct HleQuirk {
    uint32_t bit;
    const char *name;
    bool (*on)(const HleDriver *d);     /* the drivers it is on for; NULL: none */
} HleQuirk;

static bool qualcomm_proprietary(const HleDriver *d)
{
    return d->drv.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
}

static bool mesa_turnip(const HleDriver *d)
{
    return d->drv.driverID == VK_DRIVER_ID_MESA_TURNIP;
}

/* Mali hardware, any driver (vendor 0x13B5: Arm's, PanVK) */
static bool mali(const HleDriver *d)
{
    return d->props.vendorID == 0x13B5;
}

static bool mali_g57(const HleDriver *d)
{
    return mali(d) && strstr(d->props.deviceName, "Mali-G57") != NULL;
}

/* Arm's own Mali driver. malisx2 (a custom driver) reports Arm's vendor and
 * driver IDs too; "malisx2" in driverInfo tells it apart, and the blob's
 * bugs below are not its. */
static bool arm_blob(const HleDriver *d)
{
    return d->drv.driverID == VK_DRIVER_ID_ARM_PROPRIETARY &&
           strstr(d->drv.driverInfo, "malisx2") == NULL;
}

/* Arm's driver's release, from its driverInfo ("v1.r34p0-01eac0.<build>":
 * 34); 0 for another driver or info that names none */
static unsigned arm_release(const HleDriver *d)
{
    const char *r = arm_blob(d) ? d->drv.driverInfo : NULL;
    while (r && (r = strstr(r, ".r")) != NULL) {
        unsigned n = 0;
        for (r += 2; *r >= '0' && *r <= '9' && n < 1000; r++) {
            n = n * 10 + (unsigned)(*r - '0');
        }
        if (n && *r == 'p') {
            return n;
        }
    }
    return 0;
}

static bool arm_before_r40(const HleDriver *d)
{
    const unsigned r = arm_release(d);
    return r && r < 40;
}

static bool mali_g52_arm(const HleDriver *d)
{
    return arm_blob(d) && strstr(d->props.deviceName, "Mali-G52") != NULL;
}

/* The present CPU wait works around Qualcomm's driver, whose present holds
 * the queue's lock for 3-6 ms while it waits; the Mali-G52's (Arm's r34p0)
 * returns at once, so the wait only cost there (upstream: the presenter
 * spent 620-730 ms/s waiting for its passes, without it 0.3 ms/s). Arm's
 * other GPUs and releases are untested, so they keep it. */
static bool not_turnip_not_g52_arm(const HleDriver *d)
{
    return !mesa_turnip(d) && !mali_g52_arm(d);
}

static bool no_timestamps_on(const HleDriver *d)
{
    return arm_blob(d) && (strstr(d->props.deviceName, "Mali-G615") != NULL || mali_g52_arm(d));
}

/* Seen on (driver, version, what showed it). */
static const HleQuirk k_quirks[] = {
    /* Adreno 840 stock driver 0x8034a02c: Forza's menu text picks its glyph
     * layer with a per-vertex mask (v4); after a pipeline change the draw
     * read v4 from offset 144 of the vertex instead of 64 (wrong letters). */
    { HLE_QUIRK_VI_AFTER_PIPELINE, "vi_after_pipeline", qualcomm_proprietary },
    { HLE_QUIRK_STATE_AFTER_PIPELINE, "state_after_pipeline", NULL },
    /* Turnip: Halo's floors flashed unlit in single frames, its lighting
     * passes over the base pass split by a mid-frame kick; the stock Adreno
     * driver drew them right (A/B, 2026-10-14). */
    { HLE_QUIRK_EQ_DEPTH_STEP, "eq_depth_step", mesa_turnip },
    /* Turnip 26.3 (Adreno 650): Blood Wake's boat select and Crazy Taxi 3's
     * driver select showed the model in blocks only; both clear, draw a
     * backdrop with the depth test off, then the model. TU_DEBUG=nolrzfc
     * (LRZ cleared by a blit) drew them right (2026-10-22). */
    { HLE_QUIRK_LRZ_CLEAR, "lrz_clear", mesa_turnip },
    /* Turnip (tu_pipeline.cc: no colour attachments, a static dummy blend
     * group): Fatal Frame's afterimage (CONSTANT_ALPHA over the last frame)
     * dropped out on the frames whose post pass shared a command buffer with
     * its depth-only shadow pass, and the scene flickered (2026-10-23). */
    { HLE_QUIRK_BLEND_CONST_RESET, "blend_const_reset", mesa_turnip },
    /* Qualcomm's driver: a present waiting on the pass's semaphore waited
     * 3-6 ms a refresh in the driver, the queue's lock held (Halo 2).
     * Turnip's makes a sync file and returns (Mesa vk_android.c); the other
     * drivers are untested, so they keep the wait. */
    { HLE_QUIRK_PRESENT_CPU_WAIT, "present_cpu_wait", not_turnip_not_g52_arm },
    /* Mali-G57 MC2, Arm's r54p1 (an Infinix X6858 user's diag log,
     * 2026-10-26): Mortal Kombat Shaolin Monks' meshes exploded; its vertices
     * are 18 bytes, two floats at offset 10, and the diag build counted 7.8
     * million misaligned attribute fetches in 230 s, nothing else wrong.
     * The driver has no VK_EXT_legacy_vertex_attributes. */
    { HLE_QUIRK_VB_ALIGN, "vb_align", mali_g57 },
    /* Mali-G57 MC4, Arm's r40p0 (upstream: an Anbernic RG556, Unisoc T820,
     * 2026-10-31): Morrowind lost the device 0.2 s into its loading screen
     * every time, the kernel log "JS: Job Hard-Stopped" and "GPU fault 0x4002
     * from job slot 0" (fragment). Its draws are W-buffered (ZENABLE 2);
     * HAKUX_HLE_DBGFLAGS=8 alone (W-buffering off) ran it at 60 fps. */
    { HLE_QUIRK_NO_WBUF, "no_wbuf", mali_g57 },
    /* Mali-G52 MP2 and Mali-G57, Arm's r34p0 (upstream, 2026-10-26..30): the
     * host wait on the timeline semaphore returned VK_ERROR_DEVICE_LOST
     * mid-game, across titles, while the GPU had run everything up to the
     * call; the counter query never failed, so the waits poll it: ~40
     * minutes of play without a loss after. r40p0 and r54p1 ran without it,
     * the releases between are untested: on for Arm's driver before r40. */
    { HLE_QUIRK_NO_SEM_WAIT, "no_sem_wait", arm_before_r40 },
    /* ARMSX2: "Mali-G615's timestamp queries never resolve" under Arm's
     * driver; the Mali-G52's r34p0 the same (upstream) */
    { HLE_QUIRK_NO_TIMESTAMPS, "no_timestamps", no_timestamps_on },
};

static uint32_t g_defaults;

static void names(uint32_t mask, char *out, size_t len)
{
    size_t n = 0;
    out[0] = 0;
    for (size_t i = 0; i < ARRAY_SIZE(k_quirks); i++) {
        if (mask & k_quirks[i].bit) {
            n += (size_t)snprintf(out + n, n < len ? len - n : 0, "%s%s", n ? "," : "",
                                  k_quirks[i].name);
        }
    }
    if (!n) {
        snprintf(out, len, "-");
    }
}

/* The defaults with an override's "+name" / "-name" applied. */
static uint32_t apply(uint32_t mask, const char *spec)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec ? spec : "");
    for (char *save = NULL, *t = strtok_r(buf, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
        bool off = *t == '-';
        const char *name = *t == '-' || *t == '+' ? t + 1 : t;
        uint32_t bits = 0;
        if (!strcmp(name, "all")) {
            for (size_t i = 0; i < ARRAY_SIZE(k_quirks); i++) {
                bits |= k_quirks[i].bit;
            }
        }
        for (size_t i = 0; i < ARRAY_SIZE(k_quirks); i++) {
            if (!strcmp(name, k_quirks[i].name)) {
                bits = k_quirks[i].bit;
            }
        }
        if (!bits) {
            hle_log("hakuX-hle-back", "quirks: \"%s\" is not a quirk (ignored)", name);
        }
        mask = off ? mask & ~bits : mask | bits;
    }
    return mask;
}

static void set(uint32_t mask, const char *why)
{
    char on[256], def[256];
    g_hle_quirks = mask;
    names(mask, on, sizeof(on));
    names(g_defaults, def, sizeof(def));
    hle_log("hakuX-hle-back", "quirks: %s (driver defaults %s%s%s)", on, def, why ? "; " : "",
            why ? why : "");
}

void hle_quirks_init(VkPhysicalDevice phys)
{
    HleDriver d = { .drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES } };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                       .pNext = &d.drv };
    vkGetPhysicalDeviceProperties2(phys, &p2);
    d.props = p2.properties;
    g_defaults = 0;
    for (size_t i = 0; i < ARRAY_SIZE(k_quirks); i++) {
        if (k_quirks[i].on && k_quirks[i].on(&d)) {
            g_defaults |= k_quirks[i].bit;
        }
    }
    hle_log("hakuX-hle-back", "vk: driver \"%s\" (id %u) %s", d.drv.driverName, d.drv.driverID,
            d.drv.driverInfo);
    const char *e = getenv("HAKUX_HLE_QUIRKS");
    set(apply(g_defaults, e), e ? "HAKUX_HLE_QUIRKS" : NULL);
}

void hle_quirks_poll(void)
{
#ifdef __ANDROID__
    static char last[PROP_VALUE_MAX];
    static uint32_t polls;
    if (polls++ % 30) {
        return;
    }
    char v[PROP_VALUE_MAX] = "";
    __system_property_get("debug.hakux.quirks", v);
    if (strcmp(v, last)) {
        strcpy(last, v);
        set(apply(g_defaults, v), v[0] ? "debug.hakux.quirks" : NULL);
    }
#endif
}

