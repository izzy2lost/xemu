/*
 * hakuX native core — taking and restoring savestates (push-buffer mode).
 *
 * Save (nc_state_save, any thread but the guest CPU's): the decoder parks
 * at the next flip stall (the frame before it complete, its targets written
 * back), the emulation pauses, and the guest CPU takes the state at its
 * pause point (nc_state_cpu_work) once the device threads are parked and
 * the disc reads in flight completed. Then the decoder and the emulation go
 * on as they were.
 *
 * Load (nc_state_load): the core booted the state's XBE as for a fresh run,
 * up to its first instruction; with the devices paused, every section
 * replaces what the boot made, and the guest CPU starts from the state's
 * context (nc_kstate_start_context).
 */
#include "native/state/save.h"
#include "native/state/sections.h"
#include "native/core/core.h"
#include "native/core/irq.h"
#include "hw/xbox/hle/host.h"
#include "hw/xbox/hle/pb/pbmode.h"
#include "hw/xbox/hle/pb/pbstate.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#ifndef NC_GIT
#define NC_GIT "unknown"
#endif

static NcCore *g_core;
static char g_core_version[32] = "dev";
static char g_path[512];        /* the state being saved or loaded */

const char *nc_state_path(void)
{
    return g_path;
}

struct NcKernel *nc_state_kernel(void)
{
    return &g_core->kernel;
}

void nc_state_attach(NcCore *c)
{
    g_core = c;
}

void nc_state_set_core_version(const char *name)
{
    snprintf(g_core_version, sizeof(g_core_version), "%s", name);
}

/* In load order: memory before what refers to it, the kernel before the
 * devices, the NV2A's registers before the decoder. */
static const NcStateSection k_sections[] = {
    {NC_TAG('R', 'A', 'M', ' '), "RAM", NC_STATE_V_RAM, NC_STATE_MIN_RAM, nc_ram_state_save, nc_ram_state_load},
    {NC_TAG('V', 'M', ' ', ' '), "VM", NC_STATE_V_VM, NC_STATE_MIN_VM, nc_vm_state_save, nc_vm_state_load},
    {NC_TAG('M', 'M', ' ', ' '), "MM", NC_STATE_V_MM, NC_STATE_MIN_MM, nc_mm_state_save, nc_mm_state_load},
    {NC_TAG('P', 'A', 'G', 'E'), "PAGE", NC_STATE_V_PAGE, NC_STATE_MIN_PAGE, nc_page_state_save, nc_page_state_load},
    {NC_TAG('K', 'E', 'R', 'N'), "KERN", NC_STATE_V_KERN, NC_STATE_MIN_KERN, nc_kern_state_save, nc_kern_state_load},
    {NC_TAG('T', 'H', 'R', 'D'), "THRD", NC_STATE_V_THRD, NC_STATE_MIN_THRD, nc_thrd_state_save, nc_thrd_state_load},
    {NC_TAG('T', 'I', 'M', 'R'), "TIMR", NC_STATE_V_TIMR, NC_STATE_MIN_TIMR, nc_timr_state_save, nc_timr_state_load},
    /* the cache drives as they were, before the files open on them reopen */
    {NC_TAG('C', 'A', 'C', 'H'), "CACHE", NC_STATE_V_CACHE, NC_STATE_MIN_CACHE, nc_cache_state_save, nc_cache_state_load,
     true},
    {NC_TAG('F', 'I', 'L', 'E'), "FILE", NC_STATE_V_FILE, NC_STATE_MIN_FILE, nc_file_state_save, nc_file_state_load},
    {NC_TAG('N', 'V', '2', 'A'), "NV2A", NC_STATE_V_NV2A, NC_STATE_MIN_NV2A, nc_nv2a_state_save, nc_nv2a_state_load},
    {NC_TAG('M', 'I', 'S', 'C'), "MISC", NC_STATE_V_MISC, NC_STATE_MIN_MISC, nc_misc_state_save, nc_misc_state_load},
    {NC_TAG('P', 'B', 'D', ' '), "PBD", NC_STATE_V_PBD, NC_STATE_MIN_PBD, nc_pbd_state_save, nc_pbd_state_load},
    {NC_TAG('A', 'P', 'U', ' '), "APU", NC_STATE_V_APU, NC_STATE_MIN_APU, nc_apu_state_save, nc_apu_state_load},
    {NC_TAG('A', 'C', 'I', ' '), "ACI", NC_STATE_V_ACI, NC_STATE_MIN_ACI, nc_aci_state_save, nc_aci_state_load},
    {NC_TAG('U', 'S', 'B', ' '), "USB", NC_STATE_V_USB, NC_STATE_MIN_USB, nc_usb_state_save, nc_usb_state_load},
    /* the patches the RAM has: what a load with other mods takes back
     * (nc_mods_after_load) */
    {NC_TAG('M', 'O', 'D', 'S'), "MODS", NC_STATE_V_MODS, NC_STATE_MIN_MODS, nc_mods_state_save, nc_mods_state_load,
     true},
};
#define NSECTIONS (sizeof(k_sections) / sizeof(k_sections[0]))

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* What identifies the running title: its XBE (the first 4 KiB of its file
 * and its size) and its disc (the size, the XDVDFS header's sector). The XBE
 * as the disc has it: a mod that patches or replaces it (native/core/mods.c)
 * leaves the game the same one, its states loadable either way. */
static void identity(NcStateInfo *in)
{
    NcCore *c = g_core;
    NcKernel *k = &c->kernel;
    in->title_id = c->xbe.info.title_id;
    snprintf(in->xbe_path, sizeof(in->xbe_path), "%s", k->image_path[0] ? k->image_path : "\\Device\\CdRom0\\default.xbe");
    static const char cd[] = "\\Device\\CdRom0";
    const char *rel = !strncasecmp(in->xbe_path, cd, sizeof(cd) - 1) ? in->xbe_path + sizeof(cd) - 1 : in->xbe_path;
    NcDiscFile xf;
    if (!nc_disc_find_orig(&c->disc, rel, &xf)) {
        xf = c->xbe.file;
    }
    uint8_t buf[4096];
    const uint32_t n = xf.size < sizeof(buf) ? xf.size : (uint32_t)sizeof(buf);
    const uint32_t crc = nc_disc_read_orig(&c->disc, &xf, 0, buf, n) || nc_disc_read(&c->disc, &xf, 0, buf, n)
                             ? (uint32_t)crc32(0, buf, n)
                             : 0;
    memset(in->xbe_sha1, 0, sizeof(in->xbe_sha1));
    memcpy(in->xbe_sha1, &crc, 4);
    memcpy(in->xbe_sha1 + 4, &xf.size, 4);
    /* the image's, so a CHD and the ISO it was made from share savestates */
    in->disc_size = nc_disc_image_size(&c->disc);
    uint8_t sec[2048];
    in->disc_hash = nc_disc_image_read(&c->disc, c->disc.part_off + 32 * 2048, sec, sizeof(sec))
                        ? (uint32_t)crc32(0, sec, sizeof(sec))
                        : 0;
    in->ram_size = c->vm.ram_size;
}

/* ------------------------------------------------------------------ save */

/* A save request: pending until the guest CPU takes it (taking while it
 * writes the state), done with its result. */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;          /* CLOCK_MONOTONIC (init_req) */
    bool pending, taking, done;
    char path[512];
    const char *result;
    char why[256];
} g_req = {PTHREAD_MUTEX_INITIALIZER};
static pthread_once_t g_req_once = PTHREAD_ONCE_INIT;

static void init_req(void)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_req.cv, &attr);
    pthread_condattr_destroy(&attr);
}

static const char *capture(NcKernel *k, const NcFrame *f, bool idle)
{
    const uint64_t t0 = mono_ns();
    const char *who = NULL;
    if (!nc_pause_quiesce(500000000ull, &who)) {
        snprintf(g_req.why, sizeof(g_req.why), "the %s device did not stop", who ? who : "?");
        return g_req.why;
    }
    const char *err = nc_io_state_drain(k, 2000000000ull);
    if (err) {
        return err;
    }
    nc_kstate_capture_frame(k, f, idle);
    NcStateInfo info;
    memset(&info, 0, sizeof(info));
    identity(&info);
    uint64_t boot, off;
    nc_ke_state(&boot, &off, &info.guest_ns);
    info.wall_time = (int64_t)time(NULL);
    info.point = idle ? NC_STATE_AT_IDLE : NC_STATE_AT_ENTRY;
    snprintf(info.core_version, sizeof(info.core_version), "%s", g_core_version);
    snprintf(info.core_git, sizeof(info.core_git), "%s", NC_GIT);
    const uint64_t t1 = mono_ns();
    snprintf(g_path, sizeof(g_path), "%s", g_req.path);
    err = nc_state_write(g_req.path, &info, k_sections, NSECTIONS);
    if (!err) {
        nc_cache_gc(g_req.path);            /* the copies only replaced states used */
    }
    struct stat st;
    nc_log("hakuX-native-state", "save %s: %s (devices and I/O %.0f ms, written in %.0f ms, %.1f MiB, %s)", g_req.path,
           err ? err : "done", (t1 - t0) / 1e6, (mono_ns() - t1) / 1e6,
           stat(g_req.path, &st) == 0 && !err ? st.st_size / 1048576.0 : 0.0,
           idle ? "every thread waiting" : "at a kernel entry");
    return err;
}

void nc_state_cpu_work(NcKernel *k, const NcFrame *f, bool idle)
{
    pthread_mutex_lock(&g_req.mu);
    const bool take = g_req.pending;
    g_req.pending = false;
    g_req.taking = take;                /* the requester waits for it now, however long */
    pthread_mutex_unlock(&g_req.mu);
    if (!take) {
        return;
    }
    const char *r = capture(k, f, idle);
    pthread_mutex_lock(&g_req.mu);
    g_req.result = r;
    g_req.taking = false;
    g_req.done = true;
    pthread_cond_broadcast(&g_req.cv);
    pthread_mutex_unlock(&g_req.mu);
}

const char *nc_state_save(const char *path)
{
    if (!g_core) {
        return "no game is running";
    }
    if (!hle_pb_active()) {
        return "savestates need push-buffer mode";
    }
    pthread_once(&g_req_once, init_req);    /* the guest CPU waits on it only for a request */
    const uint64_t t0 = mono_ns();
    const bool was_paused = nc_paused();
    /* the frame before the next flip: the decoder stops at its stall, the
     * guest runs on to its next kernel entry (paused meanwhile: for that) */
    if (was_paused) {
        nc_pause_set(false);
    }
    bool parked = hle_pb_park(true, 500000000ull);
    if (!parked) {
        nc_log("hakuX-native-state", "no flip within 500 ms: the decoder stops where it is");
        parked = hle_pb_park(false, 1000000000ull);
    }
    const char *result = NULL;
    if (!parked) {
        result = "the GPU did not stop";
    } else {
        pthread_mutex_lock(&g_req.mu);
        snprintf(g_req.path, sizeof(g_req.path), "%s", path);
        g_req.pending = true;
        g_req.taking = false;
        g_req.done = false;
        g_req.result = NULL;
        pthread_mutex_unlock(&g_req.mu);
        nc_pause_set(true);
        nc_pause_kick();
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += 15;
        /* 15 s for the guest CPU to take the request; once it has, the
         * state is being written and the game resumes only after it (the
         * devices would change what it writes) */
        pthread_mutex_lock(&g_req.mu);
        int rc = 0;
        while (!g_req.done && (rc == 0 || g_req.taking)) {
            rc = g_req.taking ? pthread_cond_wait(&g_req.cv, &g_req.mu)
                              : pthread_cond_timedwait(&g_req.cv, &g_req.mu, &ts);
        }
        result = g_req.done ? g_req.result : "the game did not stop";
        g_req.pending = false;          /* not taken: withdrawn */
        pthread_mutex_unlock(&g_req.mu);
    }
    hle_pb_unpark();
    nc_pause_set(was_paused);
    nc_log("hakuX-native-state", "save: %s in %.0f ms", result ? result : "saved", (mono_ns() - t0) / 1e6);
    return result;
}

/* ------------------------------------------------------------------ load */

/* The header against the running game, then every section's version. */
static const char *identity_check(const NcStateInfo *info, const NcStateInfo *want)
{
    static char why[256];
    if (info->title_id != want->title_id || memcmp(info->xbe_sha1, want->xbe_sha1, sizeof(want->xbe_sha1)) != 0) {
        snprintf(why, sizeof(why), "the state is of another game (%08x, this is %08x)", info->title_id, want->title_id);
        return why;
    }
    if (info->disc_size != want->disc_size || info->disc_hash != want->disc_hash) {
        return "the state was made with another copy of this game's disc";
    }
    if (info->ram_size != want->ram_size) {
        return "the state's console has another memory size";
    }
    return NULL;
}

const char *nc_state_compat(const char *path)
{
    NcStateInfo want, info;
    if (!nc_state_read_info(path, &info)) {
        return "not a state this core can read";
    }
    if (g_core) {
        /* the disc decides: a title's states may be of its other XBEs (the
         * load boots the state's) */
        memset(&want, 0, sizeof(want));
        identity(&want);
        if (info.disc_size != want.disc_size || info.disc_hash != want.disc_hash) {
            return "the state was made with another game's disc (or another copy of it)";
        }
        if (info.ram_size != want.ram_size) {
            return "the state's console has another memory size";
        }
    }
    return nc_state_verify(path, &info, k_sections, NSECTIONS);
}

const char *nc_state_load(const char *path)
{
    NcKernel *k = &g_core->kernel;
    const uint64_t t0 = mono_ns();
    NcStateInfo want, info;
    memset(&want, 0, sizeof(want));
    identity(&want);
    if (!nc_state_read_info(path, &info)) {
        return "not a state this core can read";
    }
    const char *bad = identity_check(&info, &want);
    if (bad) {
        return bad;
    }
    /* the devices hold still while their state is replaced */
    snprintf(g_path, sizeof(g_path), "%s", path);
    nc_pause_set(true);
    const char *who = NULL;
    if (!nc_pause_quiesce(1000000000ull, &who)) {
        nc_log("hakuX-native-state", "load: the %s device did not stop", who ? who : "?");
    }
    const char *err = nc_state_read(path, &info, k_sections, NSECTIONS);
    if (!err) {
        hle_host_mem_set_dirty(0, k->vm->ram_size);     /* every page new to the GPU */
        nc_kernel_clock_arm(k);
        nc_kernel_set_hd_key(k);                        /* a state from before it was set */
        /* The decoder looks at its registers again: what woke it in the run
         * that saved (the flip its stall waited for, a display start) came
         * while it was parked, and nothing may come again (the guest waits
         * for the GPU). */
        hle_pb_kick();
    }
    nc_pause_set(false);
    nc_log("hakuX-native-state", "load %s: %s in %.0f ms (made by %s %s, guest time %.1f s)", path, err ? err : "done",
           (mono_ns() - t0) / 1e6, info.core_version, info.core_git, info.guest_ns / 1e9);
    return err;
}
