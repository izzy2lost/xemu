/*
 * hakuX native core, app front end (libhakuxcore.so): SDL3 window and input,
 * the core on its own thread, and the HLE GPU's frames presented (through
 * its Vulkan swapchain).
 * Independent of xemu: it talks to native/core and the HLE GPU's display only.
 */
#ifndef NATIVE_APP_APP_H
#define NATIVE_APP_APP_H

#include "native/core/nc.h"

struct ANativeWindow;

/* present.c — on the window's thread. */
bool nc_present_init(void);
/* Once per display refresh: the newest frame (or the last one) letterboxed
 * into win, waiting for the panel; false when nothing could be presented
 * (the HLE GPU not up yet, no window). */
bool nc_present_frame(struct ANativeWindow *win);
/* The picture's shape on screen: the frame's own (auto: 4:3 for 640x480),
 * 4:3, 16:9 (a widescreen title's anamorphic 640x480), or the whole window. */
enum { NC_ASPECT_AUTO, NC_ASPECT_4_3, NC_ASPECT_16_9, NC_ASPECT_STRETCH };
void nc_present_set_aspect(int mode);
/* The console is set to widescreen: auto shows a 4:3-sized frame as 16:9
 * (a widescreen title renders anamorphic into it). */
void nc_present_set_widescreen(bool on);
/* A point of the window (0..1 each way) as one of the last frame shown
 * (0..1 inside it); false when it's on the letterbox's bars. */
bool nc_present_to_frame(float *x, float *y);
/* FXAA over each frame before it's scaled to the window. */
void nc_present_set_fxaa(bool on);
/* A frame smaller than its place in the window: scaled up bilinearly, or by
 * Snapdragon Game Super Resolution (a larger one is always averaged down). */
enum { NC_UPSCALE_BILINEAR, NC_UPSCALE_SGSR };   /* swapchain.h HLE_UPSCALE_*'s */
void nc_present_set_upscale(int mode);
/* Frames the title presented per second over the last half second, their
 * average interval, and the last one's (ms). */
void nc_present_stats(float *fps, float *avg_ms, float *last_ms);
/* A title frame has been on screen (any thread). */
bool nc_present_shown(void);

/* vklayer.c — debug.hakux.vkvalidation=1: the Khronos validation layer in
 * front of the custom driver, its vkGetInstanceProcAddr returned in place of
 * the driver's (drv_gipa as it is otherwise, or without the layer). */
void *nc_vk_layers(void *drv_gipa);

/* vsync.c — the panel's vsyncs to the emulated display's vblank clock. */
void nc_vsync_start(void);

#endif
