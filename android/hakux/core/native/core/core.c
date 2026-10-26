/*
 * hakuX native core — boot and run a title.
 */
#include "native/core/core.h"
#include "native/dev/dev.h"
#include "native/dev/usb.h"
#include "native/hle/hle_native.h"
#include "native/core/mmio.h"
#include "native/core/mods.h"
#include "native/state/save.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/prctl.h>
#include <unistd.h>

#define MAIN_STACK_SIZE (256u * 1024u)

static NcThread *g_boot;
static bool g_relaunch;
static char g_loadstate[512];   /* boot into this savestate (nc_core_set_loadstate) */
static bool g_loaded;

void nc_core_set_relaunch(bool on)
{
    g_relaunch = on;
}

void nc_core_set_loadstate(const char *path)
{
    snprintf(g_loadstate, sizeof(g_loadstate), "%s", path ? path : "");
}
static bool core_hook_query(void *opaque, uint32_t eip);

/* The title can't start: why, for the activity's dialog (NC_FATAL_BOOT). */
static bool boot_failed(const char *why)
{
    nc_hle_set_fatal(NC_FATAL_BOOT, why);
    return false;
}

/* The game's file is the problem, not the core (NC_FATAL_DISC). */
static bool disc_failed(const char *why)
{
    nc_hle_set_fatal(NC_FATAL_DISC, why);
    return false;
}

bool nc_core_boot(NcCore *c, int disc_fd)
{
    memset(c, 0, sizeof(*c));
    c->disc.fd = -1;
    /* NC_RAM_MB=128 (debug kit size): a dev switch for memory investigations. */
    const char *mb = getenv("NC_RAM_MB");
    const uint32_t ram = mb && atoi(mb) == 128 ? 128u << 20 : NC_RAM_RETAIL;
    if (!nc_vm_init(&c->vm, ram) || !nc_kernel_init(&c->kernel, &c->vm)) {
        return boot_failed("The emulated console couldn't be set up.");
    }
    if (g_relaunch) {
        /* the last title's kept memory (D3D's saved display), where it was */
        nc_kernel_av_saved_set(nc_mm_keep_restore(&c->kernel));
    }
    nc_dev_nv2a_init();
    nc_dev_apu_init(&c->vm); /* before the catch-all, which would claim its window */
    nc_dev_ohci_init(&c->vm);
    nc_dev_aci_init(&c->vm);
    nc_dev_misc_init();
    if (disc_fd < 0) {
        return disc_failed("The game's file can't be opened.");
    }
    if (!nc_disc_open(&c->disc, disc_fd)) {
        close(disc_fd);
        return disc_failed("The game's file isn't an Xbox disc image.");
    }
    nc_mods_install(&c->disc);          /* the mods' files and XBE patches, the cheats */
    /* A relaunch (XLaunchNewImage, NativeCoreActivity starts the core again):
     * the XBE the saved launch data page names, and the page for the title. */
    static uint8_t page[NC_LAUNCH_PAGE];
    bool have_page = false;
    char xbe_path[NC_LAUNCH_PAGE] = "\\default.xbe";
    if (g_loadstate[0]) {
        /* a savestate: the XBE it was running (a relaunched title's own) */
        NcStateInfo info;
        static const char cd[] = "\\Device\\CdRom0";
        if (!nc_state_read_info(g_loadstate, &info)) {
            nc_hle_set_fatal(NC_FATAL_STATE, "the savestate can't be read");
            return false;
        }
        if (!strncasecmp(info.xbe_path, cd, sizeof(cd) - 1) && info.xbe_path[sizeof(cd) - 1] == '\\') {
            snprintf(xbe_path, sizeof(xbe_path), "%s", info.xbe_path + sizeof(cd) - 1);
            snprintf(c->kernel.image_path, sizeof(c->kernel.image_path), "%s", info.xbe_path);
        }
        nc_log("hakuX-native-frontier", "savestate: booting %s for %s", xbe_path, g_loadstate);
    } else if (g_relaunch) {
        char file[512];
        nc_kernel_relaunch_file(file, sizeof(file));
        FILE *fp = fopen(file, "rb");
        if (fp) {
            have_page = fread(page, 1, NC_LAUNCH_PAGE, fp) == NC_LAUNCH_PAGE;
            fclose(fp);
            unlink(file);
        }
        if (have_page && nc_kernel_launch_xbe(page, xbe_path, sizeof(xbe_path))) {
            snprintf(c->kernel.image_path, sizeof(c->kernel.image_path), "\\Device\\CdRom0%s", xbe_path);
            nc_log("hakuX-native-frontier", "relaunch: %s (launch type %u)", c->kernel.image_path,
                   *(const uint32_t *)page);
        } else {
            nc_log("hakuX-native-frontier", "relaunch: no launch data page, booting the disc's default.xbe");
            have_page = false;
            snprintf(xbe_path, sizeof(xbe_path), "%s", "\\default.xbe");
        }
    }
    if (!nc_xbe_load(&c->vm, &c->disc, xbe_path, &c->kernel, &c->xbe)) {
        char why[256];
        snprintf(why, sizeof(why), "The game's program (%s) can't be loaded: it's damaged, or hakuX doesn't support "
                 "it yet.", xbe_path[0] == '\\' ? xbe_path + 1 : xbe_path);
        return boot_failed(why);
    }
    nc_kernel_title_loaded(&c->kernel, &c->disc, &c->xbe.file, c->xbe.info.base, c->xbe.info.sizeof_image);
    if (have_page) {
        nc_kernel_launch_page_restore(&c->kernel, page);
    }
    nc_kernel_title_fixups(&c->kernel, &c->xbe.info);

    NcCpuConfig cfg = {
        .membase = c->vm.base,
        .sysop = nc_kernel_sysop,
        .opaque = &c->kernel,
        .hook_query = core_hook_query,
        .idt_base = c->kernel.idt_va,
        .idt_limit = 0x7FF,
    };
    c->cpu = nc_cpu_create(&cfg, c->kernel.kpcr_va);
    if (!c->cpu) {
        return boot_failed("The emulated CPU couldn't start.");
    }
    nc_mods_bind(&c->vm, c->cpu, &c->xbe.info);

    uint32_t stack = MAIN_STACK_SIZE;
    if (c->xbe.stack_commit > stack) {
        stack = nc_align_up(c->xbe.stack_commit, NC_PAGE);
    }
    g_boot = nc_kernel_boot_thread(&c->kernel, c->cpu, c->xbe.info.entry, stack);
    if (!g_boot) {
        return boot_failed("The emulated CPU couldn't start.");
    }
    nc_log("hakuX-native-frontier", "stage S1 entry %08x, stack %08x-%08x, fs %08x",
           c->xbe.info.entry, g_boot->stack_limit, g_boot->stack_base, c->kernel.kpcr_va);

    /* The HLE GPU layer: its hooks come from the title's signature scan. */
    c->kernel.hle_hook = nc_hle_hook;
    c->kernel.hle_after_sysop = nc_hle_after_sysop;
    nc_hle_boot(&c->kernel, &c->vm, c->cpu, &g_boot->ctx);
    nc_state_attach(c);
    if (g_loadstate[0]) {
        const char *err = nc_state_load(g_loadstate);
        if (err) {
            char why[384];
            snprintf(why, sizeof(why), "The savestate couldn't be loaded: %s.", err);
            nc_hle_set_fatal(NC_FATAL_STATE, why);
            return false;
        }
        nc_mods_after_load(&c->kernel, c->kernel.image_path);
        g_loaded = true;
    }
    return true;
}

void nc_core_run(NcCore *c)
{
    /* The guest thread's short host waits (a Sleep(0) poll that went on, the
     * idle CPU) end when they should: Linux lets a normal thread's timed
     * wait run 50 us late by default. */
    prctl(PR_SET_TIMERSLACK, 1000UL, 0, 0, 0);
    /* a savestate: the thread it was running, or the scheduler first */
    const NcCpuContext *start = g_loaded ? nc_kstate_start_context(&c->kernel) : &g_boot->ctx;
    nc_cpu_run(c->cpu, start ? start : &g_boot->ctx);
    const NcKernel *k = &c->kernel;
    if (k->stop) {
        nc_log("hakuX-native-frontier", "stop: %s", k->stop_reason);
    } else {
        nc_log("hakuX-native-frontier", "stop: guest executed HLT");
    }
    unsigned threads = 0;
    for (unsigned i = 0; i < NC_MAX_THREADS; i++) {
        threads += k->threads[i].state != NC_T_FREE;
    }
    const NcKExport *first = nc_kernel_export(k->first_ordinal);
    nc_log("hakuX-native-frontier", "summary stage=%s kernel_calls=%llu first_call=%s threads=%u",
           k->first_ordinal ? "S2" : "S1", (unsigned long long)k->calls,
           first ? first->name : "-", threads);
}

/* Hooked guest addresses: the HLE GPU's functions and device-access sites. */
static bool core_hook_query(void *opaque, uint32_t eip)
{
    return nc_x86mmio_is_site(eip) || nc_hle_hook_query(opaque, eip);
}

/* A fault in translated code the core can't take as the guest's: where the
 * guest was, and what it was doing, before the process goes down with it.
 * Faults elsewhere go on to ART's handlers unreported (its null checks). */
static bool fault_report(NcCore *c, uintptr_t pc, uint64_t va, const char *what)
{
    /* FEX maps a pc outside its code buffer to the vCPU's current EIP, not
     * 0: every fault in the process took this for the guest's, ART's null
     * checks on the UI thread too (a Galaxy Tab S9, Orta: the dumps, the UI
     * thread with them, then an ANR). Only translated code's. */
    uint32_t eip;
    if (!nc_cpu_sample_pc(c->cpu, pc, &eip) || !eip) {
        return false;
    }
    nc_log("hakuX-native-frontier", "host fault: %s at guest %08llx, eip %08x (host pc %p)", what,
           (unsigned long long)va, eip, (void *)pc);
    nc_kernel_dump_recent(-1);
    nc_kernel_dump_threads(&c->kernel, -1);
    return false;
}

/* bumped from the fault handlers of any thread (atomically), read by the stats */
uint64_t g_nc_faults_callret, g_nc_faults_dirty, g_nc_faults_mmio;

static struct {
    uint32_t eip, n;
} g_mfs[16];

static void mmio_fault_sample(uint32_t eip)
{
    if (__atomic_load_n(&g_nc_faults_mmio, __ATOMIC_RELAXED) & 63) {
        return;
    }
    unsigned k = 0;
    while (k < 16 && g_mfs[k].n && g_mfs[k].eip != eip) {
        k++;
    }
    if (k < 16) {
        g_mfs[k].eip = eip;
        g_mfs[k].n++;
    }
}

unsigned nc_mmio_fault_top(uint32_t *eips, uint32_t *counts, unsigned max)
{
    unsigned n = 0;
    for (; n < max; n++) {
        int best = -1;
        for (unsigned k = 0; k < 16; k++) {
            if (g_mfs[k].n && (best < 0 || g_mfs[k].n > g_mfs[best].n)) {
                best = (int)k;
            }
        }
        if (best < 0) {
            break;
        }
        eips[n] = g_mfs[best].eip;
        counts[n] = g_mfs[best].n * 64;
        g_mfs[best].n = 0;
    }
    memset(g_mfs, 0, sizeof(g_mfs));
    return n;
}

void nc_mmio_fault_sites(char *buf, size_t len)
{
    size_t o = 0;
    buf[0] = 0;
    for (unsigned r = 0; r < 6 && o + 24 < len; r++) {
        int best = -1;
        for (unsigned k = 0; k < 16; k++) {
            if (g_mfs[k].n && (best < 0 || g_mfs[k].n > g_mfs[best].n)) {
                best = (int)k;
            }
        }
        if (best < 0) {
            break;
        }
        o += (size_t)snprintf(buf + o, len - o, " %08x:%u", g_mfs[best].eip, g_mfs[best].n * 64);
        g_mfs[best].n = 0;
    }
    memset(g_mfs, 0, sizeof(g_mfs));
}

bool nc_core_fault(NcCore *c, int sig, siginfo_t *info, void *ucontext)
{
    if (sig == SIGILL) {
        return false;
    }
    const uintptr_t pc = ((ucontext_t *)ucontext)->uc_mcontext.pc;
    if (nc_cpu_callret_fault(c->cpu, (uintptr_t)info->si_addr, ucontext)) {
        __atomic_fetch_add(&g_nc_faults_callret, 1, __ATOMIC_RELAXED);
        return true;
    }
    {   /* NC_WATCHPA: a guest store to the watched RAM, with its EIP */
        const uint64_t gva = nc_vm_guest_of(&c->vm, info->si_addr);
        const uint32_t gpa = gva != UINT64_MAX ? nc_vm_phys(&c->vm, (uint32_t)gva) : UINT32_MAX;
        if (gpa != UINT32_MAX && nc_hle_watch_pa(gpa)) {
            nc_hle_watch_store((uint32_t)gva, gpa, nc_cpu_guest_eip_at(c->cpu, pc), 0);
            static unsigned framed;
            NcFrame *wf = framed < 20 ? nc_cpu_fault_frame(c->cpu, ucontext) : NULL;
            if (wf) {
                /* the stack's words that look like the title's code: return addresses */
                framed++;
                char fr[400];
                int n = 0;
                const uint32_t esp = nc_frame_gpr(wf, NC_ESP);
                for (uint32_t i = 0; i < 160 && n < (int)sizeof(fr) - 12; i++) {
                    const uint32_t a = esp + 4 * i;
                    if (nc_vm_phys(&c->vm, a) == UINT32_MAX) {
                        break;
                    }
                    const uint32_t w = nc_vm_rd32(&c->vm, a);
                    if (w >= 0x11000 && w < 0x160000) {
                        n += snprintf(fr + n, sizeof(fr) - (size_t)n, " %x:%08x", i * 4, w);
                    }
                }
                nc_log("hakuX-native-watch", "  esp %08x stack%s", esp, fr);
            }
        }
    }
    if (nc_hle_dirty_fault(info)) {
        __atomic_fetch_add(&g_nc_faults_dirty, 1, __ATOMIC_RELAXED);
        return true;
    }
    if (nc_mmio_fault(&c->vm, info, ucontext)) {
        __atomic_fetch_add(&g_nc_faults_mmio, 1, __ATOMIC_RELAXED);
        mmio_fault_sample(nc_cpu_guest_eip_at(c->cpu, pc));
        /* From translated code: that instruction is hooked from now on. */
        nc_x86mmio_note(nc_cpu_guest_eip_at(c->cpu, pc));
        return true;
    }
    /* A guest page the guest doesn't have: the title's page tables may map
     * it since the last flush (the access runs again), or else from
     * translated code the title's page-fault handler, if it has one, maps it.
     * An access past the window's ends is the guest's at the wrapped address
     * (Forza after a race: [0xFFFFFFC4 + 0x60], a NULL list entry's field):
     * the title's fault when that page is missing, as on the Xbox. */
    uint64_t va = nc_vm_guest_of(&c->vm, info->si_addr);
    const uint64_t wrapped = va == UINT64_MAX ? nc_vm_guest_of_wrapped(&c->vm, info->si_addr) : UINT64_MAX;
    if (wrapped != UINT64_MAX) {
        va = wrapped;
    }
    if (sig != SIGSEGV || va == UINT64_MAX) {
        return fault_report(c, pc, (uint64_t)(uintptr_t)info->si_addr, "outside the guest");
    }
    if (nc_vm_phys(&c->vm, (uint32_t)va) != UINT32_MAX) {
        return fault_report(c, pc, va, wrapped != UINT64_MAX ? "wrapped, to a mapped page" : "mapped page");
    }
    if (nc_paging_fault(&c->kernel, (uint32_t)va)) {
        /* mapped now; the wrapped access would only fault again */
        return wrapped == UINT64_MAX || fault_report(c, pc, va, "wrapped, to a page the tables map");
    }
    NcFrame *f = nc_cpu_fault_frame(c->cpu, ucontext);
    if (!f) {
        nc_kernel_call_fault(&c->kernel, (uint32_t)va, nc_cpu_fault_is_write(ucontext));
        return false;
    }
    if (wrapped != UINT64_MAX) {
        nc_log("hakuX-native-frontier", "guest fault at %08x (wrapped address), eip %08x", (uint32_t)va,
               nc_frame_eip(f));
    }
    if (!nc_kernel_page_fault(&c->kernel, f, (uint32_t)va, nc_cpu_fault_is_write(ucontext))) {
        return fault_report(c, pc, va, "the title doesn't handle it");
    }
    nc_cpu_fault_resume(c->cpu, ucontext);
    return true;
}
