/*
 * hakuX native core — the Sega Chihiro baseboard's two USB devices, the
 * Cypress AN2131s: the QC (JVS I/O board, EEPROMs, RTC, the game's backup
 * SRAM) and the SC (two UARTs: card readers, drive boards).
 *
 * Each runs its 8051 firmware, from the board's EEPROM dumps (ic10 for the
 * QC, pc20 for the SC), on xemu's low-level model (hw/xbox/chihiro: the
 * 8051, the AN2131's registers, the JVS board, the drive boards), built here
 * unchanged against a few lines of shim (native/chihiro/shim). Every control
 * transfer but SET_ADDRESS goes to the firmware, as on the chip; the bulk
 * endpoints are its buffers. The 8051 runs 6000 cycles per USB frame (1 ms),
 * as xemu's tick does.
 *
 * NC_CHIHIRO=1 turns it on (ohci.c puts the QC on root port 1 and the SC on
 * port 2, no gamepads). NC_CHIHIRO_ROMS: a folder with ic10_g24lc64.bin,
 * ic11_24lc024.bin and pc20_g24lc64.bin, else House of the Dead 3's built
 * in. NC_CHIHIRO_SAVE: the backup file (xemu's format, "CHSV"), read at boot
 * and written as the game changes it. NC_CHIHIRO_REGION: 1 Japan, 2 USA,
 * 3 Export (default). NC_CHIHIRO_FREEPLAY=1: free play.
 *
 * Input: player 1's gamepad (nc_pad_set) drives the JVS board the way xemu's
 * does on a phone, by the cabinet the game's XBE names (the table below):
 * driving cabinets get the wheel, gun cabinets the gun (nc_chihiro_gun, or
 * the left stick without one).
 */
#include "native/dev/usb.h"
#include "native/core/irq.h"
#include "native/kernel/kernel.h"
#include "native/core/vm.h"

#include "qemu/osdep.h"
#include "chihiro.h"
#include "chihiro-log.h"
#include "chihiro-an2131.h"
#include "chihiro-jvs.h"
#include "chihiro-cabinet.h"
#include "chihiro-cardreader-crp1231.h"
#include "chihiro-cardreader-hw210.h"
#include "chihiro-driveboard-sega838.h"
#include "chihiro-driveboard-v257.h"
#include "chihiro-firmware.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#define TAG "hakuX-native-chihiro"

/* ------------------------------------------------- what the sources need */

unsigned chihiro_log_mask;
char chihiro_game_dir[1024];
char chihiro_game_filename[64];
bool chihiro_freeplay_setting;
bool chihiro_hw210_enabled;      /* no card readers yet: the SC's UARTs idle */
bool chihiro_crp1231_enabled;
CardReaderState *chihiro_card_reader_global;
CRP1231State *chihiro_crp1231_global;

static uint64_t g_t0_ms;

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int64_t qemu_clock_get_ms(int clock)
{
    return (int64_t)(mono_ms() - g_t0_ms);
}

/* The card readers aren't fitted yet (chihiro_*_enabled stay off, so the
 * firmware's UARTs never reach these). */
void card_reader_write_byte(CardReaderState *s, uint8_t byte) {}
int card_reader_read(CardReaderState *s, uint8_t *buf, int max_len) { return 0; }
bool card_reader_has_response(CardReaderState *s) { return false; }
void crp1231_receive_byte(CRP1231State *r, uint8_t byte) {}
bool crp1231_has_response(CRP1231State *r) { return false; }
bool crp1231_read(CRP1231State *r, uint8_t *out) { return false; }
bool crp1231_mid_response(CRP1231State *r) { return false; }

/* xemu's CONFIG_CHIHIRO_JVS_PROFILE_*: the input layout. */
enum { P_NONE = -1, P_HOTD3, P_VC3, P_GS, P_CTX, P_OR2, P_OK, P_WMMT2, P_GUNDAM };

/* What each game's cabinet has fitted (xemu's chihiro_cabinets, chihiro.c),
 * by the basename of its XBE. */
typedef struct Cabinet {
    const char *xbe;             /* case-insensitive prefix */
    int profile;
    ChihiroCardReaderKind card;
    ChihiroDriveBoardKind drive;
    ChihiroMonitorKind monitor;
    uint8_t locks[2];
} Cabinet;

static const Cabinet k_cabinets[] = {
    {"hod3xb", P_HOTD3, CHIHIRO_CARD_NONE, CHIHIRO_DRIVE_NONE, CHIHIRO_MONITOR_15KHZ, {0, 0}},
    {"ctx_ac", P_CTX, CHIHIRO_CARD_NONE, CHIHIRO_DRIVE_NONE, CHIHIRO_MONITOR_15KHZ, {0, 0}},
    {"vc3", P_VC3, CHIHIRO_CARD_NONE, CHIHIRO_DRIVE_NONE, CHIHIRO_MONITOR_15KHZ, {0, 0}},
    {"vsg", P_GS, CHIHIRO_CARD_HW210, CHIHIRO_DRIVE_NONE, CHIHIRO_MONITOR_15KHZ, {0x20, 0x01}},
    {"outrun2", P_OR2, CHIHIRO_CARD_NONE, CHIHIRO_DRIVE_SEGA838, CHIHIRO_MONITOR_15KHZ, {0, 0}},
    {"OllieKing", P_OK, CHIHIRO_CARD_NONE, CHIHIRO_DRIVE_NONE, CHIHIRO_MONITOR_15KHZ, {0, 0}},
    {"V307", P_WMMT2, CHIHIRO_CARD_CRP1231, CHIHIRO_DRIVE_V257, CHIHIRO_MONITOR_15KHZ, {0, 0}},
    {"V322", P_WMMT2, CHIHIRO_CARD_CRP1231, CHIHIRO_DRIVE_V257, CHIHIRO_MONITOR_15KHZ, {0, 0}},
    {"gs", P_GUNDAM, CHIHIRO_CARD_HW210, CHIHIRO_DRIVE_NONE, CHIHIRO_MONITOR_15KHZ, {0x40, 0x00}},
    {"mj3", P_NONE, CHIHIRO_CARD_NONE, CHIHIRO_DRIVE_NONE, CHIHIRO_MONITOR_31KHZ, {0, 0}},
    {"mj2", P_NONE, CHIHIRO_CARD_NONE, CHIHIRO_DRIVE_NONE, CHIHIRO_MONITOR_31KHZ, {0, 0}},
    {"golf", P_NONE, CHIHIRO_CARD_NONE, CHIHIRO_DRIVE_NONE, CHIHIRO_MONITOR_31KHZ, {0, 0}},
};

static const Cabinet *g_cabinet;

static const Cabinet *cabinet(void)
{
    return g_cabinet;
}

ChihiroCardReaderKind chihiro_cabinet_card_reader(void)
{
    return cabinet() ? cabinet()->card : CHIHIRO_CARD_NONE;
}

ChihiroDriveBoardKind chihiro_cabinet_drive_board(void)
{
    return cabinet() ? cabinet()->drive : CHIHIRO_DRIVE_NONE;
}

uint8_t chihiro_cabinet_card_lock(int player)
{
    return cabinet() && player >= 0 && player < 2 ? cabinet()->locks[player] : 0;
}

ChihiroMonitorKind chihiro_cabinet_monitor(void)
{
    return cabinet() ? cabinet()->monitor : CHIHIRO_MONITOR_15KHZ;
}

int chihiro_detected_game_profile(void)
{
    return cabinet() ? cabinet()->profile : P_NONE;
}

void nc_chihiro_set_game(const char *xbe)
{
    const char *n = xbe ? xbe : "";
    for (const char *p = n; *p; p++) {
        if (*p == '\\' || *p == '/') {
            n = p + 1;
        }
    }
    snprintf(chihiro_game_filename, sizeof(chihiro_game_filename), "%s", n);
    g_cabinet = NULL;
    for (size_t i = 0; i < sizeof(k_cabinets) / sizeof(k_cabinets[0]); i++) {
        if (!strncasecmp(n, k_cabinets[i].xbe, strlen(k_cabinets[i].xbe))) {
            g_cabinet = &k_cabinets[i];
            break;
        }
    }
    nc_log(TAG, "game %s: %s", n, g_cabinet ? g_cabinet->xbe : "not in the cabinet table, nothing fitted");
}

/* ------------------------------------------------------------- the board */

typedef struct Board {
    NcUsbDev dev;
    bool qc;
    uint8_t eeprom[8192];        /* ic10 (QC) or pc20 (SC) */
    uint8_t ic11[512];           /* the 128-byte dump, through a 256-byte window */
    uint8_t extmem[2][65536];    /* [0] the firmware's work RAM, [1] the game's backup */
    ChihiroJVSState jvs;
    AN2131State an;
    bool fw_held;
    unsigned frames;
} Board;

static Board *g_qc, *g_sc;
static bool g_enabled;
static char g_save[512];

static DriveBoardState g_sega838;
static V257DriveBoard g_v257;

/* Their descriptors are the firmware's (it answers GET_DESCRIPTOR); these
 * only give the hub and the logs a shape. */
static const uint8_t k_qc_dev[18] = {18, 1, 0x00, 0x01, 0x60, 0, 0, 0x40, 0xa3, 0x0c, 0x02, 0x00,
                                     0x08, 0x01, 2, 3, 1, 1};
static const uint8_t k_sc_dev[18] = {18, 1, 0x00, 0x01, 0x60, 1, 0, 0x40, 0xa3, 0x0c, 0x03, 0x00,
                                     0x10, 0x01, 2, 3, 1, 1};

static uint8_t region_byte(void)
{
    const char *r = getenv("NC_CHIHIRO_REGION");
    const int v = r ? atoi(r) : 3;
    return (uint8_t)(v >= 1 && v <= 3 ? v : 3);
}

/* Free play: coin mode 10 in the ACBU coin config (xemu's
 * chihiro_apply_freeplay, chihiro-usb.c, which has the layout). */
static void apply_freeplay(Board *b)
{
    if (b->ic11[0] != 'A' || b->ic11[1] != 'C') {
        return;
    }
    static const uint8_t freeplay_defaults[20] = {
        0x63, 0x09, 0x04, 0x00, 0x01, 0x01, 0x0A, 0x01, 0x01, 0x01,
        0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    };
    for (int slot = 0; slot < 2; slot++) {
        const int base = slot * 0x40, cfg = base + 0x1C;
        if (chihiro_freeplay_setting) {
            if (b->ic11[cfg] <= 8 || b->ic11[cfg] >= 100) {
                memcpy(&b->ic11[cfg], freeplay_defaults, 20);
            } else {
                b->ic11[base + 0x22] = 0x0A;
                b->ic11[base + 0x23] = 0x01;
            }
        } else if (b->ic11[base + 0x22] == 0x0A) {
            b->ic11[base + 0x22] = 0x01;
            b->ic11[base + 0x23] = 0x00;
        }
        uint16_t sum = 0;
        for (int i = 0; i < 52; i++) {
            sum += b->ic11[base + 0x0C + i];
        }
        b->ic11[base + 0x0A] = (uint8_t)sum;
        b->ic11[base + 0x0B] = (uint8_t)(sum >> 8);
    }
}

/* The save file, as xemu writes it: "CHSV", version 2, the ic11 EEPROM
 * (512), the backup half from 0x8000 (0x8000 bytes) and from 0x2000
 * (0x5B40). Version 1 stops before the last. */
#define SAVE_MAGIC 0x56534843u
#define SAVE_HI_OFF 0x8000
#define SAVE_HI_SIZE 0x8000
#define SAVE_LO_OFF 0x2000
#define SAVE_LO_SIZE 0x5B40

static bool save_read(Board *b, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint32_t magic = 0, version = 0;
    uint8_t ic11[512];
    static uint8_t backup[65536];
    memcpy(backup, b->extmem[1], sizeof(backup));
    bool ok = fread(&magic, 4, 1, f) == 1 && magic == SAVE_MAGIC && fread(&version, 4, 1, f) == 1 &&
              (version == 1 || version == 2) && fread(ic11, 1, sizeof(ic11), f) == sizeof(ic11) &&
              fread(backup + SAVE_HI_OFF, 1, SAVE_HI_SIZE, f) == SAVE_HI_SIZE;
    if (ok) {
        memset(backup + SAVE_LO_OFF, 0, SAVE_LO_SIZE);
        ok = version == 1 || fread(backup + SAVE_LO_OFF, 1, SAVE_LO_SIZE, f) == SAVE_LO_SIZE;
    }
    fclose(f);
    if (ok) {
        memcpy(b->ic11, ic11, sizeof(ic11));
        memcpy(b->extmem[1], backup, sizeof(backup));
        nc_log(TAG, "save (version %u) loaded from %s", version, path);
    } else {
        nc_log(TAG, "%s isn't a Chihiro save; the board starts fresh", path);
    }
    return ok;
}

/* Written beside, then renamed over: a crash never leaves half a file. */
static bool save_write(Board *b, const char *path)
{
    char tmp[sizeof(g_save) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return false;
    }
    const uint32_t magic = SAVE_MAGIC, version = 2;
    bool ok = fwrite(&magic, 4, 1, f) == 1 && fwrite(&version, 4, 1, f) == 1 &&
              fwrite(b->ic11, 1, 512, f) == 512 &&
              fwrite(b->extmem[1] + SAVE_HI_OFF, 1, SAVE_HI_SIZE, f) == SAVE_HI_SIZE &&
              fwrite(b->extmem[1] + SAVE_LO_OFF, 1, SAVE_LO_SIZE, f) == SAVE_LO_SIZE;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static int board_control(NcUsbDev *d, const uint8_t *s, uint8_t *data, int len)
{
    Board *b = (Board *)d;
    const uint8_t req = s[1];
    const uint16_t value = nc_rd16(s + 2);
    const int wlen = nc_rd16(s + 6);

    /* ANCHOR_LOAD: the silicon's own firmware download, into the 8051's RAM;
     * releasing CPUCS starts what was loaded. */
    if (req == 0xA0) {
        if (!(s[0] & 0x80)) {
            an2131_anchor_load(&b->an, value, data, len);
            if (value == 0x7F92 && len > 0) {
                const bool hold = data[0] & 1;
                if (b->fw_held && !hold) {
                    nc_log(TAG, "%s: firmware downloaded, running", d->name);
                }
                b->fw_held = hold;
            }
            return 0;
        }
        return NC_USB_STALL;
    }
    if (!b->an.cpu_running) {
        return NC_USB_STALL;
    }
    const bool in = s[0] & 0x80;
    uint8_t resp[256];
    const int r = an2131_setup_packet(&b->an, s, in ? NULL : data, in ? 0 : len, resp, sizeof(resp));
    if (!in) {
        return 0;
    }
    if (r > 0) {
        const int n = MIN(r, len);
        memcpy(data, resp, n);
        return n;
    }
    /* No answer armed: the request's whole length goes back, as xemu does. */
    return MIN(wlen, len);
}

static int board_data(NcUsbDev *d, bool in, int ep, uint8_t *buf, int len)
{
    Board *b = (Board *)d;
    if (ep >= AN2131_EP_COUNT || !b->an.cpu_running) {
        return NC_USB_STALL;
    }
    if (in) {
        if (an2131_ep_in_poll(&b->an, ep) < 0) {
            return NC_USB_NAK;
        }
        uint8_t tmp[64];
        const int got = an2131_ep_in_read(&b->an, ep, tmp, MIN(len, (int)sizeof(tmp)));
        if (got > 0) {
            memcpy(buf, tmp, got);
        }
        return got > 0 ? got : 0;
    }
    /* The OUT buffer NAKs until the firmware took the last packet. */
    if (b->an.ep[ep].cs_out & EPCS_BSY) {
        return NC_USB_NAK;
    }
    const int n = MIN(len, 64);
    an2131_ep_out_write(&b->an, ep, buf, n);
    an2131_run(&b->an, 2000);
    return n;
}

static void board_reset(NcUsbDev *d)
{
    Board *b = (Board *)d;
    if (b->qc) {
        b->eeprom[0x1F00] = region_byte();
        apply_freeplay(b);
    }
    if (b->an.cpu_running) {
        b->an.usbirq |= USBIRQ_URES;
        b->an.cpu.irq_recheck = true;
        an2131_run(&b->an, 10000);
    }
}

static void input_frame(void);

/* debug: NC_CHIHIRO_PCS=1, where the guest's CPU is, sampled each frame and
 * its top PCs logged every 2 s (a title stuck waiting on the baseboard). */
extern bool (*g_hle_pc_sampler)(uint32_t *pc);

static NcKernel *g_kernel;

void nc_chihiro_set_kernel(struct NcKernel *k)
{
    g_kernel = k;
}

static void pc_sample(unsigned frame)
{
    static int on = -1;
    static uint32_t pcs[64], counts[64];
    if (on < 0) {
        on = getenv("NC_CHIHIRO_PCS") != NULL;
    }
    if (!on || !g_hle_pc_sampler) {
        return;
    }
    uint32_t pc = 0;
    if (g_hle_pc_sampler(&pc)) {
        for (unsigned i = 0; i < 64; i++) {
            if (!counts[i] || pcs[i] == pc) {
                pcs[i] = pc;
                counts[i]++;
                break;
            }
        }
    }
    if (frame % 2000 == 0) {
        char line[512];
        int n = 0;
        for (unsigned t = 0; t < 10; t++) {
            unsigned best = 64;
            for (unsigned i = 0; i < 64; i++) {
                if (counts[i] && (best == 64 || counts[i] > counts[best])) {
                    best = i;
                }
            }
            if (best == 64) {
                break;
            }
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x:%u", pcs[best], counts[best]);
            counts[best] = 0;
        }
        memset(counts, 0, sizeof(counts));
        nc_log(TAG, "guest pcs:%s", line);
        if (frame % 10000 == 0 && g_kernel) {   /* the threads every 10 s */
            nc_log(TAG, "kernel: irql %u, current t%u, in dpc %d, in isr %d (saved irql %u), dpc saved irql %u",
                   *(uint8_t *)nc_vm_host(g_kernel->vm, g_kernel->kpcr_va + 0x24),
                   g_kernel->current ? g_kernel->current->id : 0, g_kernel->in_dpc,
                   g_kernel->in_isr, g_kernel->isr_saved_irql, g_kernel->dpc_saved_irql);
            nc_kernel_dump_recent(-1);
            nc_kernel_dump_threads(g_kernel, -1);
        }
    }
}

/* Each USB frame (1 ms, the OHCI thread). */
static void board_frame(NcUsbDev *d)
{
    Board *b = (Board *)d;
    b->frames++;
    if (b->qc) {
        pc_sample(b->frames);
        if (b->frames % 4 == 0) {
            input_frame();
        }
        if (b->frames % 2000 == 0 && g_save[0] && b->an.backup_dirty) {
            b->an.backup_dirty = false;
            if (!save_write(b, g_save)) {
                nc_log(TAG, "the save couldn't be written to %s", g_save);
            }
        }
    } else if (b->frames % 16 == 0 && chihiro_cabinet_drive_board() == CHIHIRO_DRIVE_V257) {
        v257_tick(&g_v257);     /* the V257 keeps its own cadence */
    }
    if (b->an.cpu_running) {
        an2131_run(&b->an, 6000);
    }
}

static bool load_rom(const char *dir, const char *name, uint8_t *out, size_t size, size_t *got)
{
    if (!dir || !*dir) {
        return false;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    const size_t n = fread(out, 1, size, f);
    fclose(f);
    if (got) {
        *got = n;
    }
    return got ? n > 0 : n == size;
}

static Board *board_new(bool qc)
{
    Board *b = calloc(1, sizeof(Board));
    b->qc = qc;
    b->dev.name = qc ? "chihiro-qc" : "chihiro-sc";
    b->dev.dev_desc = qc ? k_qc_dev : k_sc_dev;
    b->dev.dev_desc_len = 18;
    b->dev.raw_ep0 = true;
    b->dev.control = board_control;
    b->dev.data = board_data;
    b->dev.reset = board_reset;
    b->dev.frame = board_frame;
    b->dev.pending_addr = -1;

    const char *roms = getenv("NC_CHIHIRO_ROMS");
    size_t ic11_len = 0;
    const bool own_fw = load_rom(roms, qc ? "ic10_g24lc64.bin" : "pc20_g24lc64.bin", b->eeprom, 8192, NULL);
    if (!own_fw) {
        memcpy(b->eeprom, qc ? hotd3_ic10_g24lc64 : hotd3_pc20_g24lc64, 8192);
    }
    if (!load_rom(roms, "ic11_24lc024.bin", b->ic11, 128, &ic11_len)) {
        memcpy(b->ic11, hotd3_ic11_24lc024, sizeof(hotd3_ic11_24lc024));
    }
    if (qc) {
        b->eeprom[0x1F00] = region_byte();
        if (g_save[0]) {
            save_read(b, g_save);
        }
        apply_freeplay(b);
        chihiro_jvs_init(&b->jvs);
        chihiro_jvs_global = &b->jvs;
    }

    an2131_init(&b->an);
    b->an.is_qc = qc;
    b->an.ic10_eeprom = b->eeprom;
    b->an.ic10_size = sizeof(b->eeprom);
    b->an.ic11_eeprom = b->ic11;
    b->an.ic11_size = 256;
    b->an.extmem = b->extmem[0];
    an2131_b2_boot(&b->an, b->eeprom, sizeof(b->eeprom));
    if (!qc && b->an.cpu_running) {
        an2131_run(&b->an, 8000000);    /* the SC's init finishes within its boot */
    }
    nc_log(TAG, "%s: firmware %s, %s", b->dev.name, own_fw ? "from NC_CHIHIRO_ROMS" : "built in (HOTD3)",
           b->an.cpu_running ? "running" : "NOT running");
    return b;
}

bool nc_chihiro_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NC_CHIHIRO");
        on = e && atoi(e) != 0;
    }
    return on;
}

FILE *nc_chihiro_stderr;

#ifdef __ANDROID__
/* xemu's Chihiro models report on stderr, which Android drops: their
 * nc_chihiro_stderr is a pipe whose lines go to the log. */
static void *stderr_pump(void *arg)
{
    FILE *in = fdopen((int)(intptr_t)arg, "r");
    char line[512];
    while (in && fgets(line, sizeof(line), in)) {
        line[strcspn(line, "\n")] = 0;
        __android_log_write(ANDROID_LOG_INFO, TAG, line);
    }
    return NULL;
}
#endif

static void stderr_open(void)
{
#ifdef __ANDROID__
    int fd[2];
    if (pipe(fd) == 0 && (nc_chihiro_stderr = fdopen(fd[1], "w"))) {
        setvbuf(nc_chihiro_stderr, NULL, _IOLBF, 0);
        pthread_t t;
        pthread_create(&t, NULL, stderr_pump, (void *)(intptr_t)fd[0]);
        pthread_detach(t);
        return;
    }
#endif
    nc_chihiro_stderr = fdopen(dup(STDERR_FILENO), "w");
}

void nc_chihiro_init(NcUsbDev **qc, NcUsbDev **sc)
{
    g_t0_ms = mono_ms();
    stderr_open();
    chihiro_log_init();
    const char *save = getenv("NC_CHIHIRO_SAVE");
    snprintf(g_save, sizeof(g_save), "%s", save ? save : "");
    const char *fp = getenv("NC_CHIHIRO_FREEPLAY");
    chihiro_freeplay_setting = fp && atoi(fp) != 0;

    driveboard_init(&g_sega838);
    chihiro_driveboard_global = &g_sega838;
    v257_init(&g_v257);
    chihiro_v257_global = &g_v257;

    g_qc = board_new(true);
    g_sc = board_new(false);
    g_enabled = true;
    *qc = &g_qc->dev;
    *sc = &g_sc->dev;
}

void nc_chihiro_flush(void)
{
    if (g_qc && g_save[0] && g_qc->an.backup_dirty) {
        g_qc->an.backup_dirty = false;
        save_write(g_qc, g_save);
    }
}

/* ----------------------------------------------------------------- input */

static pthread_mutex_t g_gun_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_gun_x, g_gun_y;
static bool g_gun_on, g_gun_down, g_gun_live;

void nc_chihiro_gun(float x, float y, bool on_screen, bool down)
{
    pthread_mutex_lock(&g_gun_lock);
    g_gun_x = x;
    g_gun_y = y;
    g_gun_on = on_screen;
    g_gun_down = down;
    g_gun_live = true;
    pthread_mutex_unlock(&g_gun_lock);
}

#define PRESSED(p, i) ((p).analog[i] > 0x20)
enum { A_A, A_B, A_X, A_Y, A_BLACK, A_WHITE, A_LT, A_RT };

/* xemu_input_update_jvs_driving (ui/xemu-input.c), from the gamepad. */
static void input_driving(ChihiroJVSState *jvs, const NcPadState *p, int profile)
{
    int steer = p->lx;
    if (p->buttons & NC_PAD_DPAD_LEFT) {
        steer = -32768;
    }
    if (p->buttons & NC_PAD_DPAD_RIGHT) {
        steer = 32767;
    }
    const uint16_t gas = (uint16_t)(p->analog[A_RT] * 257);
    const uint16_t pedal = (uint16_t)(p->analog[A_LT] * 257);
    const uint16_t wheel = (uint16_t)(steer + 32768);

    static NcPadState prev;
    const bool black_down = PRESSED(*p, A_BLACK) && !PRESSED(prev, A_BLACK);
    const bool white_down = PRESSED(*p, A_WHITE) && !PRESSED(prev, A_WHITE);
    const bool a_down = PRESSED(*p, A_A) && !PRESSED(prev, A_A);
    const bool b_down = PRESSED(*p, A_B) && !PRESSED(prev, A_B);
    prev = *p;

    uint8_t sw0 = 0, sw1 = 0, p2sw0 = 0;
    if (p->buttons & NC_PAD_START) {
        sw0 |= 0x80;
    }
    if (p->buttons & NC_PAD_RTHUMB) {
        sw0 |= 0x40;                    /* SERVICE */
    }
    if (profile == P_OK) {
        jvs->analog[0] = 0;
        jvs->analog[1] = (uint16_t)MIN(65535u, 65536u - wheel);
        jvs->analog[2] = 0;
        if (pedal > 8191 || (p->buttons & NC_PAD_DPAD_UP)) {
            sw0 |= 0x20;
        }
        if (gas > 8191 || (p->buttons & NC_PAD_DPAD_DOWN)) {
            sw0 |= 0x10;
        }
        if (PRESSED(*p, A_A) || PRESSED(*p, A_WHITE)) {
            sw0 |= 0x02;
        }
        if (PRESSED(*p, A_B) || PRESSED(*p, A_BLACK)) {
            sw0 |= 0x01;
        }
    } else {
        jvs->analog[0] = wheel;
        jvs->analog[1] = gas;
        jvs->analog[2] = pedal;
    }
    jvs->analog[3] = 0x8000;

    if (profile == P_WMMT2) {
        v257_set_steer(&g_v257, (int16_t)steer);
        const uint16_t w = v257_wheel(&g_v257);
        jvs->analog[0] = (uint16_t)(w << 6 | w >> 4);
        static const uint8_t gate[7] = {0, 0xA0, 0x60, 0x80, 0x40, 0x90, 0x50};
        static unsigned gear = 1;
        if (black_down && gear < 6) {
            gear++;
        }
        if (white_down && gear > 0) {
            gear--;
        }
        sw1 |= gate[gear];
        if (PRESSED(*p, A_X)) {
            sw1 |= 0x02;
        }
        if (PRESSED(*p, A_Y)) {
            sw1 |= 0x01;
        }
    } else if (profile == P_CTX) {
        static bool forward = true;
        if (a_down) {
            forward = true;
        }
        if (b_down) {
            forward = false;
        }
        sw0 |= forward ? 0x20 : 0x10;
        if (PRESSED(*p, A_X)) {
            sw0 |= 0x02;
        }
    } else if (profile == P_OR2) {
        if (PRESSED(*p, A_X)) {
            sw0 |= 0x10;
        }
        if (PRESSED(*p, A_BLACK)) {
            p2sw0 |= 0x20;
        }
        if (PRESSED(*p, A_WHITE)) {
            p2sw0 |= 0x10;
        }
    }
    jvs->player_switches[0][0] = sw0;
    jvs->player_switches[0][1] = sw1;
    jvs->player_switches[1][0] = p2sw0;
    jvs->player_switches[1][1] = 0;
    jvs->system_switches = (p->buttons & NC_PAD_LTHUMB) ? 0x80 : 0x00;
}

/* xemu_input_update_jvs_lightgun: the gun from nc_chihiro_gun (the touch
 * screen), else aimed with the left stick and fired with A. */
static void input_gun(ChihiroJVSState *jvs, const NcPadState *p)
{
    float x, y;
    bool on, down;
    pthread_mutex_lock(&g_gun_lock);
    const bool live = g_gun_live;
    x = g_gun_x;
    y = g_gun_y;
    on = g_gun_on;
    down = g_gun_down;
    pthread_mutex_unlock(&g_gun_lock);
    if (!live) {
        x = (p->lx + 32768) / 65535.0f;
        y = (32767 - p->ly) / 65535.0f;
        on = true;
        down = PRESSED(*p, A_A) || PRESSED(*p, A_RT);
    }
    const bool reload = PRESSED(*p, A_B);
    const bool pedal = PRESSED(*p, A_LT);

    uint8_t sw0 = 0, sw1 = 0;
    if (!on) {
        jvs->analog[0] = jvs->analog[1] = 0;
    } else {
        jvs->analog[0] = (uint16_t)(MIN(MAX(x, 0.0f), 1.0f) * 0xFFFF);
        jvs->analog[1] = (uint16_t)(MIN(MAX(y, 0.0f), 1.0f) * 0xFFFF);
    }
    if (down || reload) {
        sw0 |= 0x02;
    }
    if (reload) {
        sw0 |= 0x01;
    }
    if (p->buttons & NC_PAD_START) {
        sw0 |= 0x80;
    }
    if (PRESSED(*p, A_WHITE)) {
        sw0 |= 0x40;                    /* SERVICE */
    }
    jvs->player_switches[0][0] = sw0;
    if (on && !reload) {
        sw1 |= 0x80;                    /* SCREEN-IN */
    }
    if (pedal) {
        sw1 |= 0x40;
    }
    jvs->player_switches[0][1] = sw1;
    jvs->system_switches = PRESSED(*p, A_BLACK) ? 0x80 : 0x00;  /* TEST */
}

static void input_frame(void)
{
    ChihiroJVSState *jvs = chihiro_jvs_global;
    if (!jvs) {
        return;
    }
    NcPadState p;
    nc_pad_get(0, &p);
    const int profile = chihiro_detected_game_profile();
    if (profile == P_CTX || profile == P_OR2 || profile == P_WMMT2 || profile == P_OK) {
        input_driving(jvs, &p, profile);
    } else {
        input_gun(jvs, &p);
    }
    static bool coin_prev;
    const bool coin = p.buttons & NC_PAD_BACK;
    if (coin && !coin_prev) {
        jvs->coin_count[0]++;
    }
    coin_prev = coin;
}
