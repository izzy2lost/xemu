/*
 * hakuX native core — cheats: memory codes the app turns on and off
 * (mods/README.md has the format). The engine is pure C: the guest's memory
 * through callbacks (mods.c gives it the VM's, the host tests a fake one).
 *
 * The cheats and their lines come in at boot, before the guest runs;
 * nc_cheat_set may come from any thread; nc_cheats_run runs on the guest
 * CPU's thread with the guest stopped.
 */
#ifndef NATIVE_CORE_CHEATS_H
#define NATIVE_CORE_CHEATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_CHEATS_MAX 256

typedef struct NcCheatMem {
    void *opaque;
    /* false: the address isn't mapped */
    bool (*read)(void *opaque, uint32_t va, void *buf, unsigned n);
    bool (*write)(void *opaque, uint32_t va, const void *buf, unsigned n);
} NcCheatMem;

/* A new cheat, its code lines next (nc_cheat_line): its index, or -1 when
 * there is no room. once: written once each time it is turned on. */
int nc_cheat_add(const char *name, bool on, bool once);
/* One code line of the cheat added last. False, with why in err, when it
 * isn't one (the cheat keeps its other lines). */
bool nc_cheat_line(const char *line, char *err, size_t errlen);
/* A whole cheat at once (text: its code lines, one per line), from the app
 * while the game runs: built past the cheats there are and published last,
 * so the guest's thread never sees it half made. dry: only checked. Returns
 * its index (0 when dry), or -1 with why in err ("line 2: bad address"). */
int nc_cheat_compile(const char *name, bool on, bool once, const char *text, bool dry, char *err,
                     size_t errlen);
void nc_cheat_set(int index, bool on);
bool nc_cheat_on(int index);
unsigned nc_cheat_count(void);
const char *nc_cheat_name(int index);
/* Lines skipped so far for an unmapped address or pointer. */
uint64_t nc_cheat_misses(int index);
/* Runs every cheat that is on: returns the bytes written. */
unsigned nc_cheats_run(const NcCheatMem *m);
/* No cheats (the host tests). */
void nc_cheats_reset(void);

#ifdef __cplusplus
}
#endif

#endif
