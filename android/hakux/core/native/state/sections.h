/*
 * hakuX native core — the savestate's sections, each saved and restored by
 * the module that owns its state (save.c lists them in load order).
 */
#ifndef NATIVE_STATE_SECTIONS_H
#define NATIVE_STATE_SECTIONS_H

#include "native/state/state.h"

#ifdef __cplusplus
extern "C" {
#endif

struct NcKernel;
/* The kernel a save or load works on (save.c). */
struct NcKernel *nc_state_kernel(void);

#define NC_SECTION_FNS(x)                                                                          \
    bool nc_##x##_state_save(NcBuf *b);                                                            \
    bool nc_##x##_state_load(NcRd *r, unsigned v);

NC_SECTION_FNS(ram)   /* native/state/vm_state.c: guest RAM */
NC_SECTION_FNS(vm)    /* ... its page tables */
NC_SECTION_FNS(kern)  /* native/kernel/kstate.c */
NC_SECTION_FNS(thrd)  /* ... */
NC_SECTION_FNS(timr)  /* native/kernel/timer.c */
NC_SECTION_FNS(mm)    /* native/kernel/mm.c */
NC_SECTION_FNS(page)  /* native/kernel/paging.c */
NC_SECTION_FNS(cache) /* native/state/cache.c: the title's cache drives */
NC_SECTION_FNS(file)  /* native/kernel/io.c */
NC_SECTION_FNS(nv2a)  /* native/dev/nv2a.c */
NC_SECTION_FNS(pbd)   /* hw/xbox/hle/pb/pbstate.c */
NC_SECTION_FNS(apu)   /* native/dev/apu/state.c */
NC_SECTION_FNS(aci)   /* native/dev/aci.c */
NC_SECTION_FNS(usb)   /* native/dev/ohci.c (with the hub and the controllers) */
NC_SECTION_FNS(chih)  /* native/dev/chihiro.c: a Chihiro's baseboard and media board */
NC_SECTION_FNS(misc)  /* native/dev/ports.c */
NC_SECTION_FNS(mods)  /* native/core/mods.c: the mods' XBE patches in the RAM */

#undef NC_SECTION_FNS

#ifdef __cplusplus
}
#endif

#endif
