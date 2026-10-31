/*
 * hakuX HLE GPU — Vulkan driver quirks: what a driver gets wrong that the
 * back end works around. Each quirk is a bit with a name and the drivers it
 * is on for by default, picked from the driver's identity at device
 * creation (quirks.c). A new one is a row in that table and a test of
 * hle_quirk() where the back end works around it.
 *
 * A/B on any driver: HAKUX_HLE_QUIRKS at start, or the live property
 * debug.hakux.quirks: names separated by commas or spaces, "+name" (or the
 * bare name) on and "-name" off on top of the driver's defaults, "+all" /
 * "-all" every quirk. Empty: the driver's defaults.
 */
#ifndef HW_XBOX_HLE_VK_QUIRKS_H
#define HW_XBOX_HLE_VK_QUIRKS_H

#include "hw/xbox/hle/vk/vkdev.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    /* Binding a pipeline drops the dynamic vertex input: a draw whose input
     * equals the last one's (so the back end does not record it again)
     * fetches another layout's attributes. */
    HLE_QUIRK_VI_AFTER_PIPELINE = 1u << 0,
    /* Every dynamic state recorded again after a pipeline bind: the broad
     * form of the one above, for telling which state a driver loses. */
    HLE_QUIRK_STATE_AFTER_PIPELINE = 1u << 1,
    /* Two programs with the same invariant position math come out a depth
     * step apart when they run in different command buffers: an EQUAL pass
     * that does not write misses whole triangles. Such passes test a step
     * toward the eye and discard in the shader what is further in front
     * (vk/draw.c eq_tol, hle_draw_zeq). */
    HLE_QUIRK_EQ_DEPTH_STEP = 1u << 2,
    /* A depth clear by the pass's loadOp (an LRZ fast clear the driver keeps
     * on chip) is lost when the pass's first draws run without LRZ (the
     * depth test off: a backdrop): the depth-tested draws after them read
     * the LRZ state stored at the last pass's end and are rejected in blocks
     * of 128 x 32 pixels against the previous frame's depth. Such a pass is
     * preceded by an empty one of one pixel that clears depth, whose end
     * stores the cleared state (back.c open_pass). */
    HLE_QUIRK_LRZ_CLEAR = 1u << 3,
    /* A pipeline without a colour attachment sets the blend constants to
     * zero but keeps the ones set last as current: set again unchanged they
     * record nothing, and draws blending by them use zero. The first colour
     * draw after such a pipeline sets other constants before its own
     * (vk/draw.c). */
    HLE_QUIRK_BLEND_CONST_RESET = 1u << 4,
    /* A present with a semaphore to wait on waits for it on the CPU, the
     * queue held (Qualcomm's: Android's present asks the driver for a fence
     * over it): the presenter waits for its pass itself, unlocked, and
     * presents with nothing to wait for (vk/swapchain.c). On for every
     * driver but Turnip, whose present hands the compositor a fence. */
    HLE_QUIRK_PRESENT_CPU_WAIT = 1u << 11,
    /* A vertex attribute whose address its component size doesn't divide
     * (Vulkan's rule: a float at a 2-byte boundary, as an Xbox vertex of 18
     * bytes puts every other one) is read wrong, and the meshes explode
     * (Mali-G57): such a binding's vertices are copied to the arena with each
     * attribute moved to an offset its components divide (pb/pbdraw.c). */
    HLE_QUIRK_VB_ALIGN = 1u << 12,
};

extern uint32_t g_hle_quirks;

static inline bool hle_quirk(uint32_t q)
{
    return (g_hle_quirks & q) != 0;
}

/* The driver's quirks (the device is created), and HAKUX_HLE_QUIRKS. */
void hle_quirks_init(VkPhysicalDevice phys);

/* Once a frame: debug.hakux.quirks changed. */
void hle_quirks_poll(void);

#endif
