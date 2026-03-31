/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "qemu/timer.h"
#include "renderer.h"
#include "ui/xemu-settings.h"
#include "hw/xbox/nv2a/pgraph/prim_rewrite.h"
#include <math.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif
#ifdef __ANDROID__
#include <SDL.h>
#include <android/log.h>
#endif

#if OPT_REORDER_SAFE_WINDOWS
static bool g_xemu_draw_reorder = true;
#endif
#if OPT_ASYNC_COMPILE
static bool g_xemu_async_compile = true;

void xemu_set_async_compile(bool enable)
{
    g_xemu_async_compile = enable;
}

bool xemu_get_async_compile(void)
{
    return g_xemu_async_compile;
}
#endif

void pgraph_vk_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    NV2A_VK_DPRINTF("NV097_SET_BEGIN_END: 0x%x", d->pgraph.primitive_mode);

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

#if OPT_REORDER_SAFE_WINDOWS
    if (g_xemu_draw_reorder) {
        PGRAPHVkState *r = pg->vk_renderer_state;
        bool surface_shape_changed =
            memcmp(&pg->surface_shape, &pg->last_surface_shape,
                   sizeof(SurfaceShape)) != 0;
        bool framebuffer_will_change =
            surface_shape_changed &&
            (pg->surface_shape.color_format || pg->surface_shape.zeta_format);
        if (r->reorder_window.count > 0 && framebuffer_will_change) {
            pgraph_vk_flush_reorder_window(d);
        }
    }
#endif

    pgraph_vk_surface_update(d, true, true, depth_test || stencil_test);

    if (is_nop_draw) {
        NV2A_VK_DPRINTF("nop!");
        return;
    }
}

static VkPrimitiveTopology get_primitive_topology(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    int primitive_mode = r->shader_binding->state.geom.primitive_mode;

    switch (primitive_mode) {
    case PRIM_TYPE_POINTS:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PRIM_TYPE_LINES:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PRIM_TYPE_TRIANGLES:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    default:
        assert(!"Invalid primitive_mode");
        return 0;
    }
}

static void pipeline_cache_entry_init(Lru *lru, LruNode *node,
                                      const void *state)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    snode->layout = VK_NULL_HANDLE;
    snode->pipeline = VK_NULL_HANDLE;
    snode->draw_time = 0;
#if OPT_ASYNC_COMPILE
    snode->pending = false;
#endif
}

#if OPT_ASYNC_COMPILE
static bool pipeline_cache_pre_evict(Lru *lru, LruNode *node)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    return !snode->pending;
}
#endif

static void pipeline_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, pipeline_cache);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    assert((!r->in_command_buffer ||
            snode->draw_time < r->command_buffer_start_time) &&
           "Pipeline evicted while in use!");

    vkDestroyPipeline(r->device, snode->pipeline, NULL);
    snode->pipeline = VK_NULL_HANDLE;

    vkDestroyPipelineLayout(r->device, snode->layout, NULL);
    snode->layout = VK_NULL_HANDLE;
}

static bool pipeline_cache_entry_compare(Lru *lru, LruNode *node,
                                         const void *key)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    return memcmp(&snode->key, key, sizeof(PipelineKey));
}

#ifdef __ANDROID__
#define PIPELINE_CACHE_SAVE_INTERVAL_US (30 * 1000000LL)

static char *get_pipeline_cache_path(PGRAPHVkState *r)
{
    char *pref_path = SDL_GetPrefPath("xemu", "xemu");
    if (!pref_path) {
        return NULL;
    }
    char *path = g_strdup_printf("%svk_pipeline_cache_%08x_%08x.bin",
                                 pref_path,
                                 r->device_props.deviceID,
                                 r->device_props.driverVersion);
    SDL_free(pref_path);
    return path;
}

static void save_pipeline_cache_to_disk(PGRAPHVkState *r)
{
    g_autofree char *cache_path = get_pipeline_cache_path(r);
    size_t data_size = 0;
    VkResult res;

    if (!cache_path) {
        return;
    }

    res = vkGetPipelineCacheData(r->device, r->vk_pipeline_cache,
                                 &data_size, NULL);
    if (res != VK_SUCCESS || data_size == 0) {
        return;
    }

    g_autofree void *data = g_malloc(data_size);
    res = vkGetPipelineCacheData(r->device, r->vk_pipeline_cache,
                                 &data_size, data);
    if (res != VK_SUCCESS) {
        return;
    }

    GError *err = NULL;
    if (g_file_set_contents(cache_path, (const gchar *)data,
                            (gssize)data_size, &err)) {
        __android_log_print(ANDROID_LOG_INFO, "xemu-vk",
                            "Saved pipeline cache: %zu bytes", data_size);
    } else {
        __android_log_print(ANDROID_LOG_WARN, "xemu-vk",
                            "Failed to save pipeline cache");
        if (err) {
            g_error_free(err);
        }
    }
}

static void maybe_save_pipeline_cache(PGRAPHVkState *r)
{
    int64_t now;

    if (!g_config.perf.cache_shaders) {
        return;
    }

    now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (r->pipeline_cache_last_save_us != 0 &&
        (now - r->pipeline_cache_last_save_us) <
            PIPELINE_CACHE_SAVE_INTERVAL_US) {
        return;
    }

    r->pipeline_cache_last_save_us = now;
    save_pipeline_cache_to_disk(r);
}
#endif

static void init_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkPipelineCacheCreateInfo cache_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .flags = 0,
        .initialDataSize = 0,
        .pInitialData = NULL,
        .pNext = NULL,
    };

#ifdef __ANDROID__
    g_autofree gchar *cache_data = NULL;
    gsize cache_data_size = 0;
    if (g_config.perf.cache_shaders) {
        g_autofree char *cache_path = get_pipeline_cache_path(r);
        if (cache_path) {
            GError *err = NULL;
            if (g_file_get_contents(cache_path, &cache_data, &cache_data_size,
                                    &err)) {
                cache_info.initialDataSize = (size_t)cache_data_size;
                cache_info.pInitialData = (const void *)cache_data;
                __android_log_print(ANDROID_LOG_INFO, "xemu-vk",
                                    "Loaded pipeline cache: %zu bytes",
                                    cache_data_size);
            } else {
                if (err) {
                    g_error_free(err);
                }
            }
        }
    }
#endif

    VkResult cache_result = vkCreatePipelineCache(r->device, &cache_info, NULL,
                                                  &r->vk_pipeline_cache);
    if (cache_result != VK_SUCCESS) {
        cache_info.initialDataSize = 0;
        cache_info.pInitialData = NULL;
        VK_CHECK(vkCreatePipelineCache(r->device, &cache_info, NULL,
                                       &r->vk_pipeline_cache));
    }

    const size_t pipeline_cache_size = 2048;
    lru_init(&r->pipeline_cache, 4096);
    r->pipeline_cache_entries =
        g_malloc_n(pipeline_cache_size, sizeof(PipelineBinding));
    assert(r->pipeline_cache_entries != NULL);
    for (int i = 0; i < pipeline_cache_size; i++) {
        lru_add_free(&r->pipeline_cache, &r->pipeline_cache_entries[i].node);
    }

    r->pipeline_cache.init_node = pipeline_cache_entry_init;
    r->pipeline_cache.compare_nodes = pipeline_cache_entry_compare;
    r->pipeline_cache.post_node_evict = pipeline_cache_entry_post_evict;
#if OPT_ASYNC_COMPILE
    r->pipeline_cache.pre_node_evict = pipeline_cache_pre_evict;
#endif
}

static void finalize_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

#ifdef __ANDROID__
    if (g_config.perf.cache_shaders) {
        save_pipeline_cache_to_disk(r);
    }
#endif

    lru_flush(&r->pipeline_cache);
    lru_destroy(&r->pipeline_cache);
    g_free(r->pipeline_cache_entries);
    r->pipeline_cache_entries = NULL;

    vkDestroyPipelineCache(r->device, r->vk_pipeline_cache, NULL);
}

static char const *const quad_glsl =
    "#version 450\n"
    "void main()\n"
    "{\n"
    "    float x = -1.0 + float((gl_VertexIndex & 1) << 2);\n"
    "    float y = -1.0 + float((gl_VertexIndex & 2) << 1);\n"
    "    gl_Position = vec4(x, y, 0, 1);\n"
    "}\n";

static char const *const solid_frag_glsl =
    "#version 450\n"
    "layout(location = 0) out vec4 fragColor;\n"
    "void main()\n"
    "{\n"
    "    fragColor = vec4(1.0);"
    "}\n";

static void init_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    r->quad_vert_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_VERTEX_BIT, quad_glsl);
    r->solid_frag_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_FRAGMENT_BIT, solid_frag_glsl);
}

static void finalize_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_destroy_shader_module(r, r->quad_vert_module);
    pgraph_vk_destroy_shader_module(r, r->solid_frag_module);
}

static void init_render_passes(PGRAPHVkState *r)
{
    r->render_passes = g_array_new(false, false, sizeof(RenderPass));
}

static void finalize_render_passes(PGRAPHVkState *r)
{
    for (int i = 0; i < r->render_passes->len; i++) {
        RenderPass *p = &g_array_index(r->render_passes, RenderPass, i);
        vkDestroyRenderPass(r->device, p->render_pass, NULL);
    }
    g_array_free(r->render_passes, true);
    r->render_passes = NULL;
}

void pgraph_vk_init_pipelines(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    init_pipeline_cache(pg);
    init_clear_shaders(pg);
    init_render_passes(r);

    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VK_CHECK(vkCreateSemaphore(r->device, &semaphore_info, NULL,
                               &r->command_buffer_semaphore));

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VK_CHECK(
        vkCreateFence(r->device, &fence_info, NULL, &r->command_buffer_fence));
}

void pgraph_vk_finalize_pipelines(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

#if OPT_ASYNC_COMPILE
    pgraph_vk_compile_worker_shutdown(r);
#endif

    finalize_clear_shaders(pg);
    finalize_pipeline_cache(pg);
    finalize_render_passes(r);

    vkDestroyFence(r->device, r->command_buffer_fence, NULL);
    vkDestroySemaphore(r->device, r->command_buffer_semaphore, NULL);
}

static void init_render_pass_state(PGRAPHState *pg, RenderPassState *state)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    state->color_format = r->color_binding ?
                              r->color_binding->host_fmt.vk_format :
                              VK_FORMAT_UNDEFINED;
    state->zeta_format = r->zeta_binding ? r->zeta_binding->host_fmt.vk_format :
                                           VK_FORMAT_UNDEFINED;
}

static VkRenderPass create_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    NV2A_VK_DPRINTF("Creating render pass");

    VkAttachmentDescription attachments[2];
    int num_attachments = 0;

    bool color = state->color_format != VK_FORMAT_UNDEFINED;
    bool zeta = state->zeta_format != VK_FORMAT_UNDEFINED;

    VkAttachmentReference color_reference;
    if (color) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->color_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
            .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        color_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_GENERAL
        };
        num_attachments++;
    }

    VkAttachmentReference depth_reference;
    if (zeta) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->zeta_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        depth_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        num_attachments++;
    }

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
    };

    if (color) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }

    if (zeta) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = color ? 1 : 0,
        .pColorAttachments = color ? &color_reference : NULL,
        .pDepthStencilAttachment = zeta ? &depth_reference : NULL,
    };

    VkRenderPassCreateInfo renderpass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = num_attachments,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    VkRenderPass render_pass;
    VK_CHECK(vkCreateRenderPass(r->device, &renderpass_create_info, NULL,
                                &render_pass));
    return render_pass;
}

static VkRenderPass add_new_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    RenderPass new_pass;
    memcpy(&new_pass.state, state, sizeof(*state));
    new_pass.render_pass = create_render_pass(r, state);
    g_array_append_vals(r->render_passes, &new_pass, 1);
    return new_pass.render_pass;
}

static VkRenderPass get_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    for (int i = 0; i < r->render_passes->len; i++) {
        RenderPass *p = &g_array_index(r->render_passes, RenderPass, i);
        if (!memcmp(&p->state, state, sizeof(*state))) {
            return p->render_pass;
        }
    }
    return add_new_render_pass(r, state);
}

static void create_frame_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DPRINTF("Creating framebuffer");

    assert(r->color_binding || r->zeta_binding);

    SurfaceBinding *binding = r->color_binding ? : r->zeta_binding;
    VkImageView color_view = r->color_binding ? r->color_binding->image_view
                                              : VK_NULL_HANDLE;
    VkImageView zeta_view = r->zeta_binding ? r->zeta_binding->image_view
                                            : VK_NULL_HANDLE;
    uint32_t width = binding->width;
    uint32_t height = binding->height;
    pgraph_apply_scaling_factor(pg, &width, &height);

    for (int i = 0; i < r->fb_cache_count; i++) {
        if (r->fb_cache[i].render_pass == r->render_pass &&
            r->fb_cache[i].color_view == color_view &&
            r->fb_cache[i].zeta_view == zeta_view &&
            r->fb_cache[i].width == width &&
            r->fb_cache[i].height == height) {
            r->current_framebuffer = r->fb_cache[i].framebuffer;
            return;
        }
    }

    if (r->framebuffer_index >= ARRAY_SIZE(r->framebuffers)) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    VkImageView attachments[2];
    int attachment_count = 0;

    if (r->color_binding) {
        attachments[attachment_count++] = color_view;
    }
    if (r->zeta_binding) {
        attachments[attachment_count++] = zeta_view;
    }

    VkFramebufferCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = r->render_pass,
        .attachmentCount = attachment_count,
        .pAttachments = attachments,
        .width = width,
        .height = height,
        .layers = 1,
    };
    VK_CHECK(vkCreateFramebuffer(r->device, &create_info, NULL,
                                 &r->framebuffers[r->framebuffer_index++]));
    r->current_framebuffer = r->framebuffers[r->framebuffer_index - 1];

    if (r->fb_cache_count < FB_CACHE_MAX) {
        r->fb_cache[r->fb_cache_count++] = (typeof(r->fb_cache[0])) {
            .render_pass = r->render_pass,
            .color_view = color_view,
            .zeta_view = zeta_view,
            .width = width,
            .height = height,
            .framebuffer = r->current_framebuffer,
        };
    }
}

static void destroy_framebuffers(PGRAPHState *pg)
{
    NV2A_VK_DPRINTF("Destroying framebuffer");
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < r->framebuffer_index; i++) {
        vkDestroyFramebuffer(r->device, r->framebuffers[i], NULL);
        r->framebuffers[i] = VK_NULL_HANDLE;
    }
    r->framebuffer_index = 0;
    r->fb_cache_count = 0;
    r->current_framebuffer = VK_NULL_HANDLE;
}

static void create_clear_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DGROUP_BEGIN("Creating clear pipeline");

    PipelineKey key;
    memset(&key, 0, sizeof(key));
    key.clear = true;
    init_render_pass_state(pg, &key.render_pass_state);

    key.regs[0] = r->clear_parameter;

    uint64_t hash = fast_hash((void *)&key, sizeof(key));
    LruNode *node = lru_lookup(&r->pipeline_cache, hash, &key);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    if (snode->pipeline != VK_NULL_HANDLE) {
        NV2A_VK_DPRINTF("Cache hit");
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);
    memcpy(&snode->key, &key, sizeof(key));

    bool clear_any_color_channels =
        r->clear_parameter & NV097_CLEAR_SURFACE_COLOR;
    bool clear_all_color_channels =
        (r->clear_parameter & NV097_CLEAR_SURFACE_COLOR) ==
        (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G | NV097_CLEAR_SURFACE_B |
         NV097_CLEAR_SURFACE_A);
    bool partial_color_clear =
        clear_any_color_channels && !clear_all_color_channels;

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[2];
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->quad_vert_module->module,
            .pName = "main",
        };
    if (partial_color_clear) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = r->solid_frag_module->module,
                .pName = "main",
            };
     }

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable =
            (r->clear_parameter & NV097_CLEAR_SURFACE_Z) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
    };

    if (r->clear_parameter & NV097_CLEAR_SURFACE_STENCIL) {
        depth_stencil.stencilTestEnable = VK_TRUE;
        depth_stencil.front.failOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.passOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.depthFailOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
        depth_stencil.front.compareMask = 0xff;
        depth_stencil.front.writeMask = 0xff;
        depth_stencil.front.reference = 0xff;
        depth_stencil.back = depth_stencil.front;
    }

    VkColorComponentFlags write_mask = 0;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_R)
        write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_G)
        write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_B)
        write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_A)
        write_mask |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = write_mask,
        .blendEnable = VK_TRUE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = r->color_binding ? 1 : 0,
        .pAttachments = r->color_binding ? &color_blend_attachment : NULL,
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR,
                                        VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = partial_color_clear ? 3 : 2,
        .pDynamicStates = dynamic_states,
    };

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };

    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = get_render_pass(r, &key.render_pass_state),
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_info, NULL, &pipeline));

    snode->pipeline = pipeline;
    snode->layout = layout;
    snode->render_pass = pipeline_info.renderPass;
    snode->draw_time = pg->draw_time;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

#ifdef __ANDROID__
    maybe_save_pipeline_cache(r);
#endif
    NV2A_VK_DGROUP_END();
}

static bool check_render_pass_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(r->pipeline_binding);

    RenderPassState state;
    init_render_pass_state(pg, &state);

    return memcmp(&state, &r->pipeline_binding->key.render_pass_state,
                  sizeof(state)) != 0;
}

// Quickly check for any state changes that would require more analysis
static bool check_pipeline_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->pipeline_binding ||
#if OPT_ASYNC_COMPILE
        r->pipeline_binding->pending ||
#endif
        r->pipeline_binding->pipeline == VK_NULL_HANDLE ||
        r->shader_bindings_changed ||
        r->texture_bindings_changed || check_render_pass_dirty(pg)) {
        return true;
    }
    if (pg->pipeline_state_gen != r->last_pipeline_state_gen) {
        return true;
    }

    // FIXME: Use dirty bits instead
    if (memcmp(r->vertex_attribute_descriptions,
               r->pipeline_binding->key.attribute_descriptions,
               r->num_active_vertex_attribute_descriptions *
                   sizeof(r->vertex_attribute_descriptions[0])) ||
        memcmp(r->vertex_binding_descriptions,
               r->pipeline_binding->key.binding_descriptions,
               r->num_active_vertex_binding_descriptions *
                   sizeof(r->vertex_binding_descriptions[0]))) {
        return true;
    }

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_NOTDIRTY);

    return false;
}

static void init_pipeline_key(PGRAPHState *pg, PipelineKey *key)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    memset(key, 0, sizeof(*key));
    init_render_pass_state(pg, &key->render_pass_state);
    memcpy(&key->shader_state, &r->shader_binding->state, sizeof(ShaderState));
    memcpy(key->binding_descriptions, r->vertex_binding_descriptions,
           sizeof(key->binding_descriptions[0]) *
               r->num_active_vertex_binding_descriptions);
    memcpy(key->attribute_descriptions, r->vertex_attribute_descriptions,
           sizeof(key->attribute_descriptions[0]) *
               r->num_active_vertex_attribute_descriptions);

    // FIXME: Register masking
    // FIXME: Use more dynamic state updates
    const int regs[] = {
        NV_PGRAPH_BLEND,       NV_PGRAPH_BLENDCOLOR,  NV_PGRAPH_CONTROL_0,
        NV_PGRAPH_CONTROL_1,   NV_PGRAPH_CONTROL_2,   NV_PGRAPH_CONTROL_3,
        NV_PGRAPH_SETUPRASTER, NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
    };
    assert(ARRAY_SIZE(regs) == ARRAY_SIZE(key->regs));
    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        key->regs[i] = pgraph_reg_r(pg, regs[i]);
    }
}

static void create_pipeline(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("Creating pipeline");

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    bool textures_clean = pgraph_vk_check_textures_fast_skip(pg);

    if (r->pipeline_binding &&
#if OPT_ASYNC_COMPILE
        !r->pipeline_binding->pending &&
#endif
        r->pipeline_binding->pipeline != VK_NULL_HANDLE &&
        pg->texture_state_gen == r->last_texture_state_gen &&
        textures_clean &&
        !check_render_pass_dirty(pg) &&
        pg->shader_state_gen == r->last_shader_state_gen &&
        pg->pipeline_state_gen == r->last_pipeline_state_gen &&
        pg->primitive_mode == r->shader_binding->state.geom.primitive_mode) {
        NV2A_VK_DGROUP_END();
        return;
    }

    if (pg->texture_state_gen != r->last_texture_state_gen || !textures_clean) {
        pgraph_vk_bind_textures(d);
        r->last_texture_state_gen = pg->texture_state_gen;
    }
    pgraph_vk_bind_shaders(pg);

    bool pipeline_dirty = check_pipeline_dirty(pg);

    pgraph_clear_dirty_reg_map(pg);
    r->last_pipeline_state_gen = pg->pipeline_state_gen;
    r->last_shader_state_gen = pg->shader_state_gen;
    r->last_any_reg_gen = pg->any_reg_gen;

    if (r->pipeline_binding && !pipeline_dirty) {
        NV2A_VK_DPRINTF("Cache hit");
        NV2A_VK_DGROUP_END();
        return;
    }

    PipelineKey key;
    init_pipeline_key(pg, &key);
    uint64_t hash = fast_hash((void *)&key, sizeof(key));

    LruNode *node = lru_lookup(&r->pipeline_cache, hash, &key);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    memcpy(&snode->key, &key, sizeof(key));

#if OPT_ASYNC_COMPILE
    if (snode->pending) {
        NV2A_VK_DPRINTF("Pending pipeline");
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return;
    }
#endif

    if (snode->pipeline != VK_NULL_HANDLE) {
        NV2A_VK_DPRINTF("Cache hit");
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);

#if OPT_ASYNC_COMPILE
    if (xemu_get_async_compile() &&
        (!r->shader_binding || !qatomic_read(&r->shader_binding->ready))) {
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return;
    }
#endif

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool depth_write = !!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE);
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;

    if (!r->shader_binding ||
        !r->shader_binding->vsh.module_info ||
        !r->shader_binding->psh.module_info) {
        NV2A_VK_DGROUP_END();
        return;
    }

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[3];

    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->shader_binding->vsh.module_info->module,
            .pName = "main",
        };
    if (r->shader_binding->geom.module_info) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_GEOMETRY_BIT,
                .module = r->shader_binding->geom.module_info->module,
                .pName = "main",
            };
    }
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = r->shader_binding->psh.module_info->module,
            .pName = "main",
        };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount =
            r->num_active_vertex_binding_descriptions,
        .pVertexBindingDescriptions = r->vertex_binding_descriptions,
        .vertexAttributeDescriptionCount =
            r->num_active_vertex_attribute_descriptions,
        .pVertexAttributeDescriptions = r->vertex_attribute_descriptions,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = get_primitive_topology(pg),
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    void *rasterizer_next_struct = NULL;

    VkPolygonMode polygon_mode =
        pgraph_polygon_mode_vk_map[r->shader_binding->state.geom.polygon_front_mode];
    if (polygon_mode != VK_POLYGON_MODE_FILL &&
        r->enabled_physical_device_features.fillModeNonSolid != VK_TRUE) {
        polygon_mode = VK_POLYGON_MODE_FILL;
    }

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable =
            r->enabled_physical_device_features.depthClamp == VK_TRUE ?
            VK_TRUE : VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = polygon_mode,
        .lineWidth = 1.0f,
        .frontFace = (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                      NV_PGRAPH_SETUPRASTER_FRONTFACE) ?
                          VK_FRONT_FACE_COUNTER_CLOCKWISE :
                          VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .pNext = rasterizer_next_struct,
    };

    if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) & NV_PGRAPH_SETUPRASTER_CULLENABLE) {
        uint32_t cull_face = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                      NV_PGRAPH_SETUPRASTER_CULLCTRL);
        assert(cull_face < ARRAY_SIZE(pgraph_cull_face_vk_map));
        rasterizer.cullMode = pgraph_cull_face_vk_map[cull_face];
    } else {
        rasterizer.cullMode = VK_CULL_MODE_NONE;
    }

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE,
    };

    if (depth_test) {
        depth_stencil.depthTestEnable = VK_TRUE;
        uint32_t depth_func =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0), NV_PGRAPH_CONTROL_0_ZFUNC);
        assert(depth_func < ARRAY_SIZE(pgraph_depth_func_vk_map));
        depth_stencil.depthCompareOp = pgraph_depth_func_vk_map[depth_func];
    }

    if (stencil_test) {
        depth_stencil.stencilTestEnable = VK_TRUE;
        uint32_t stencil_func = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                         NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
        uint32_t stencil_ref = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                        NV_PGRAPH_CONTROL_1_STENCIL_REF);
        uint32_t mask_read = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                      NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
        uint32_t mask_write = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                       NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
        uint32_t op_fail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                    NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
        uint32_t op_zfail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                     NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
        uint32_t op_zpass = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                     NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

        assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_vk_map));
        assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
        assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
        assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_vk_map));

        depth_stencil.front.failOp = pgraph_stencil_op_vk_map[op_fail];
        depth_stencil.front.passOp = pgraph_stencil_op_vk_map[op_zpass];
        depth_stencil.front.depthFailOp = pgraph_stencil_op_vk_map[op_zfail];
        depth_stencil.front.compareOp =
            pgraph_stencil_func_vk_map[stencil_func];
        depth_stencil.front.compareMask = mask_read;
        depth_stencil.front.writeMask = mask_write;
        depth_stencil.front.reference = stencil_ref;
        depth_stencil.back = depth_stencil.front;
    }

    VkColorComponentFlags write_mask = 0;
    if (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = write_mask,
    };

    float blend_constant[4] = { 0, 0, 0, 0 };

    if (pgraph_reg_r(pg, NV_PGRAPH_BLEND) & NV_PGRAPH_BLEND_EN) {
        color_blend_attachment.blendEnable = VK_TRUE;

        uint32_t sfactor =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_SFACTOR);
        uint32_t dfactor =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_DFACTOR);
        assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        color_blend_attachment.srcColorBlendFactor =
            pgraph_blend_factor_vk_map[sfactor];
        color_blend_attachment.dstColorBlendFactor =
            pgraph_blend_factor_vk_map[dfactor];
        color_blend_attachment.srcAlphaBlendFactor =
            pgraph_blend_factor_vk_map[sfactor];
        color_blend_attachment.dstAlphaBlendFactor =
            pgraph_blend_factor_vk_map[dfactor];

        uint32_t equation =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_EQN);
        assert(equation < ARRAY_SIZE(pgraph_blend_equation_vk_map));

        color_blend_attachment.colorBlendOp =
            pgraph_blend_equation_vk_map[equation];
        color_blend_attachment.alphaBlendOp =
            pgraph_blend_equation_vk_map[equation];

        uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
        pgraph_argb_pack32_to_rgba_float(blend_color, blend_constant);
    }

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = r->color_binding ? 1 : 0,
        .pAttachments = r->color_binding ? &color_blend_attachment : NULL,
        .blendConstants[0] = blend_constant[0],
        .blendConstants[1] = blend_constant[1],
        .blendConstants[2] = blend_constant[2],
        .blendConstants[3] = blend_constant[3],
    };

    VkDynamicState dynamic_states[3] = { VK_DYNAMIC_STATE_VIEWPORT,
                                         VK_DYNAMIC_STATE_SCISSOR };
    int num_dynamic_states = 2;

    snode->has_dynamic_line_width =
        (r->enabled_physical_device_features.wideLines == VK_TRUE) &&
        (r->shader_binding->state.geom.polygon_front_mode == POLY_MODE_LINE ||
         r->shader_binding->state.geom.primitive_mode == PRIM_TYPE_LINES ||
         r->shader_binding->state.geom.primitive_mode == PRIM_TYPE_LINE_LOOP ||
         r->shader_binding->state.geom.primitive_mode == PRIM_TYPE_LINE_STRIP);
    if (snode->has_dynamic_line_width) {
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_LINE_WIDTH;
    }

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = num_dynamic_states,
        .pDynamicStates = dynamic_states,
    };

    // FIXME: Dither
    // if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
    //         NV_PGRAPH_CONTROL_0_DITHERENABLE))
    // FIXME: point size
    // FIXME: Edge Antialiasing
    // bool anti_aliasing = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING),
    // NV_PGRAPH_ANTIALIASING_ENABLE);
    // if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
    //                           NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE) {
    // FIXME: VK_EXT_line_rasterization
    // }

    // if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
    //                           NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE) {
    // FIXME: No direct analog. Just do it with MSAA.
    // }


    VkPushConstantRange push_constant_ranges[2];
    int num_push_constant_ranges = 0;
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &r->descriptor_set_layout,
    };

#if OPT_BINDLESS_TEXTURES
    VkDescriptorSetLayout set_layouts[2];
    if (r->bindless_textures_supported) {
        set_layouts[0] = r->bindless_set_layout;
        set_layouts[1] = r->descriptor_set_layout;
        pipeline_layout_info.setLayoutCount = 2;
        pipeline_layout_info.pSetLayouts = set_layouts;
        push_constant_ranges[num_push_constant_ranges++] =
            (VkPushConstantRange){
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .offset = r->tex_push_offset,
                .size = NV2A_MAX_TEXTURES * sizeof(uint32_t),
            };
    }
#endif

    if (r->use_push_constants_for_uniform_attrs) {
        int num_uniform_attributes =
            __builtin_popcount(r->shader_binding->state.vsh.uniform_attrs);
#if OPT_BINDLESS_TEXTURES
        if (r->bindless_textures_supported &&
            num_uniform_attributes > r->max_vertex_push_attrs) {
            num_uniform_attributes = 0;
        }
#endif
        if (num_uniform_attributes) {
#if OPT_BINDLESS_TEXTURES
            uint32_t vertex_push_offset =
                r->bindless_textures_supported && r->tex_push_offset == 0
                    ? NV2A_MAX_TEXTURES * sizeof(uint32_t)
                    : 0;
#else
            uint32_t vertex_push_offset = 0;
#endif
            push_constant_ranges[num_push_constant_ranges++] =
                (VkPushConstantRange){
                    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                    .offset = vertex_push_offset,
                    .size = num_uniform_attributes * 4 * sizeof(float),
                };
        }
    }

    if (num_push_constant_ranges > 0) {
        pipeline_layout_info.pushConstantRangeCount = num_push_constant_ranges;
        pipeline_layout_info.pPushConstantRanges = push_constant_ranges;
    }

    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));

    VkGraphicsPipelineCreateInfo pipeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = get_render_pass(r, &key.render_pass_state),
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };

#if OPT_ASYNC_COMPILE
    if (xemu_get_async_compile()) {
        CompileJob *job = g_malloc0(sizeof(*job));
        PipelineCreateParams *p = &job->pipeline.params;

        job->type = COMPILE_JOB_PIPELINE;
        job->pipeline.target = snode;
        p->device = r->device;
        p->vk_pipeline_cache = r->vk_pipeline_cache;
        memcpy(p->shader_stages, shader_stages,
               num_active_shader_stages * sizeof(shader_stages[0]));
        p->num_shader_stages = num_active_shader_stages;
        p->num_module_infos = 0;
        p->module_infos[p->num_module_infos] = r->shader_binding->vsh.module_info;
        pgraph_vk_ref_shader_module(p->module_infos[p->num_module_infos++]);
        if (r->shader_binding->geom.module_info) {
            p->module_infos[p->num_module_infos] =
                r->shader_binding->geom.module_info;
            pgraph_vk_ref_shader_module(p->module_infos[p->num_module_infos++]);
        }
        p->module_infos[p->num_module_infos] = r->shader_binding->psh.module_info;
        pgraph_vk_ref_shader_module(p->module_infos[p->num_module_infos++]);
        memcpy(p->binding_descs, r->vertex_binding_descriptions,
               r->num_active_vertex_binding_descriptions *
                   sizeof(VkVertexInputBindingDescription));
        memcpy(p->attr_descs, r->vertex_attribute_descriptions,
               r->num_active_vertex_attribute_descriptions *
                   sizeof(VkVertexInputAttributeDescription));
        p->num_binding_descs = r->num_active_vertex_binding_descriptions;
        p->num_attr_descs = r->num_active_vertex_attribute_descriptions;
        p->topology = input_assembly.topology;
        p->rasterizer = rasterizer;
        p->depth_stencil = depth_stencil;
        p->has_zeta = r->zeta_binding != NULL;
        p->color_blend_attachment = color_blend_attachment;
        p->has_color = r->color_binding != NULL;
        memcpy(p->blend_constants, color_blending.blendConstants,
               sizeof(p->blend_constants));
        memcpy(p->dynamic_states, dynamic_states,
               num_dynamic_states * sizeof(dynamic_states[0]));
        p->num_dynamic_states = num_dynamic_states;
        p->has_dynamic_line_width = snode->has_dynamic_line_width;
        p->layout = layout;
        p->render_pass = pipeline_create_info.renderPass;

        snode->draw_time = pg->draw_time;
        snode->pending = true;
        r->pipeline_binding = snode;
        r->pipeline_binding_changed = true;
        pgraph_vk_compile_worker_enqueue(r, job);
        NV2A_VK_DGROUP_END();
        return;
    }
#endif

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_create_info, NULL, &pipeline));

    snode->pipeline = pipeline;
    snode->layout = layout;
    snode->render_pass = pipeline_create_info.renderPass;
    snode->draw_time = pg->draw_time;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

#ifdef __ANDROID__
    maybe_save_pipeline_cache(r);
#endif
    NV2A_VK_DGROUP_END();
}

static bool can_push_vertex_attr_values(PGRAPHVkState *r)
{
    if (!r->use_push_constants_for_uniform_attrs) {
        return false;
    }
#if OPT_BINDLESS_TEXTURES
    if (r->bindless_textures_supported &&
        __builtin_popcount(r->shader_binding->state.vsh.uniform_attrs) >
            r->max_vertex_push_attrs) {
        return false;
    }
#endif
    return true;
}

static void push_vertex_attr_values(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!can_push_vertex_attr_values(r)) {
        return;
    }

    // FIXME: Partial updates

    float values[NV2A_VERTEXSHADER_ATTRIBUTES][4];
    int num_uniform_attrs = 0;

    pgraph_get_inline_values(pg, r->shader_binding->state.vsh.uniform_attrs,
                             values, &num_uniform_attrs);

    if (num_uniform_attrs > 0) {
#if OPT_BINDLESS_TEXTURES
        uint32_t vertex_push_offset =
            r->bindless_textures_supported && r->tex_push_offset == 0
                ? NV2A_MAX_TEXTURES * sizeof(uint32_t)
                : 0;
#else
        uint32_t vertex_push_offset = 0;
#endif
        vkCmdPushConstants(r->command_buffer, r->pipeline_binding->layout,
                           VK_SHADER_STAGE_VERTEX_BIT, vertex_push_offset,
                           num_uniform_attrs * 4 * sizeof(float),
                           &values);
    }
}

static void bind_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(r->descriptor_set_index >= 1);

    vkCmdBindDescriptorSets(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            r->pipeline_binding->layout,
#if OPT_BINDLESS_TEXTURES
                            r->bindless_textures_supported ? 1 : 0,
#else
                            0,
#endif
                            1,
                            &r->descriptor_sets[r->descriptor_set_index - 1], 0,
                            NULL);
}

#if OPT_BINDLESS_TEXTURES
static void bind_bindless_set(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->bindless_textures_supported) {
        return;
    }

    vkCmdBindDescriptorSets(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            r->pipeline_binding->layout, 0, 1,
                            &r->bindless_descriptor_set, 0, NULL);
}

static void push_texture_indices(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->bindless_textures_supported) {
        return;
    }

    vkCmdPushConstants(r->command_buffer, r->pipeline_binding->layout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, r->tex_push_offset,
                       sizeof(r->tex_bindless_indices),
                       r->tex_bindless_indices);
}
#endif

static void begin_query(PGRAPHVkState *r)
{
    assert(r->in_command_buffer);
    assert(!r->in_render_pass);
    assert(!r->query_in_flight);

    // FIXME: We should handle this. Make the query buffer bigger, but at least
    // flush current queries.
    assert(r->num_queries_in_flight < r->max_queries_in_flight);

    nv2a_profile_inc_counter(NV2A_PROF_QUERY);
    vkCmdResetQueryPool(r->command_buffer, r->query_pool,
                        r->num_queries_in_flight, 1);
    VkQueryControlFlags query_flags =
        r->enabled_physical_device_features.occlusionQueryPrecise == VK_TRUE ?
        VK_QUERY_CONTROL_PRECISE_BIT : 0;
    vkCmdBeginQuery(r->command_buffer, r->query_pool, r->num_queries_in_flight,
                    query_flags);

    r->query_in_flight = true;
    r->new_query_needed = false;
    r->num_queries_in_flight++;
}

static void end_query(PGRAPHVkState *r)
{
    assert(r->in_command_buffer);
    assert(!r->in_render_pass);
    assert(r->query_in_flight);

    vkCmdEndQuery(r->command_buffer, r->query_pool,
                  r->num_queries_in_flight - 1);
    r->query_in_flight = false;
}

static void sync_staging_buffer(PGRAPHState *pg, VkCommandBuffer cmd,
                                int index_src, int index_dst)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b_src = &r->storage_buffers[index_src];
    StorageBuffer *b_dst = &r->storage_buffers[index_dst];

    if (!b_src->buffer_offset) {
        return;
    }

    VkBufferCopy copy_region = { .size = b_src->buffer_offset };
    vkCmdCopyBuffer(cmd, b_src->buffer, b_dst->buffer, 1, &copy_region);

    VkAccessFlags dst_access_mask;
    VkPipelineStageFlags dst_stage_mask;

    switch (index_dst) {
    case BUFFER_INDEX:
        dst_access_mask = VK_ACCESS_INDEX_READ_BIT |
                          VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        break;
    case BUFFER_VERTEX_INLINE:
        dst_access_mask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        break;
    case BUFFER_UNIFORM:
        dst_access_mask = VK_ACCESS_UNIFORM_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        break;
    default:
        assert(0);
        break;
    }

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = dst_access_mask,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = b_dst->buffer,
        .size = b_src->buffer_offset
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage_mask, 0,
                         0, NULL, 1, &barrier, 0, NULL);

    b_src->buffer_offset = 0;
}

static void flush_memory_buffer(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VK_CHECK(vmaFlushAllocation(
        r->allocator, r->storage_buffers[BUFFER_VERTEX_RAM].allocation, 0,
        VK_WHOLE_SIZE));

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_VERTEX_RAM].buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1,
                         &barrier, 0, NULL);
}

static void begin_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);
    assert(!r->in_render_pass);

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_RENDERPASSES);

    unsigned int vp_width = pg->surface_binding_dim.width,
                 vp_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);

    assert(r->current_framebuffer != VK_NULL_HANDLE);

    if (r->zeta_binding &&
        r->zeta_binding->image_layout !=
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = r->zeta_binding->image_layout,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = r->zeta_binding->image,
            .subresourceRange = {
                .aspectMask = r->zeta_binding->host_fmt.aspect,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        };
        vkCmdPipelineBarrier(
            r->command_buffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            0, 0, NULL, 0, NULL, 1, &barrier);
        r->zeta_binding->image_layout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = r->render_pass,
        .framebuffer = r->current_framebuffer,
        .renderArea.extent.width = vp_width,
        .renderArea.extent.height = vp_height,
        .clearValueCount = 0,
        .pClearValues = NULL,
    };
    vkCmdBeginRenderPass(r->command_buffer, &render_pass_begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    r->in_render_pass = true;

}

static void end_render_pass(PGRAPHVkState *r)
{
    if (r->in_render_pass) {
        vkCmdEndRenderPass(r->command_buffer);
        r->in_render_pass = false;
    }
}

#if OPT_REORDER_SAFE_WINDOWS
static void flush_reorder_window_internal(NV2AState *d);
static bool classify_draw_safe(PGRAPHState *pg);
static bool try_snapshot_draw_arrays(NV2AState *d, ReorderWindowEntry *entry);
static bool try_snapshot_inline_elements(NV2AState *d,
                                         ReorderWindowEntry *entry);
#endif

typedef struct VertexBufferRemap {
    uint16_t attributes;
    size_t buffer_space_required;
    struct {
        VkDeviceAddress offset;
        VkDeviceSize old_stride;
        VkDeviceSize new_stride;
    } map[NV2A_VERTEXSHADER_ATTRIBUTES];
} VertexBufferRemap;

static bool ensure_buffer_space(PGRAPHState *pg, int index, VkDeviceSize size);
static VertexBufferRemap remap_unaligned_attributes(PGRAPHState *pg,
                                                    uint32_t num_vertices);
static void copy_remapped_attributes_to_inline_buffer(PGRAPHState *pg,
                                                      VertexBufferRemap remap,
                                                      uint32_t start_vertex,
                                                      uint32_t num_vertices);

const enum NV2A_PROF_COUNTERS_ENUM finish_reason_to_counter_enum[] = {
    [VK_FINISH_REASON_VERTEX_BUFFER_DIRTY] = NV2A_PROF_FINISH_VERTEX_BUFFER_DIRTY,
    [VK_FINISH_REASON_SURFACE_CREATE] = NV2A_PROF_FINISH_SURFACE_CREATE,
    [VK_FINISH_REASON_SURFACE_DOWN] = NV2A_PROF_FINISH_SURFACE_DOWN,
    [VK_FINISH_REASON_NEED_BUFFER_SPACE] = NV2A_PROF_FINISH_NEED_BUFFER_SPACE,
    [VK_FINISH_REASON_FRAMEBUFFER_DIRTY] = NV2A_PROF_FINISH_FRAMEBUFFER_DIRTY,
    [VK_FINISH_REASON_PRESENTING] = NV2A_PROF_FINISH_PRESENTING,
    [VK_FINISH_REASON_FLIP_STALL] = NV2A_PROF_FINISH_FLIP_STALL,
    [VK_FINISH_REASON_FLUSH] = NV2A_PROF_FINISH_FLUSH,
    [VK_FINISH_REASON_STALLED] = NV2A_PROF_FINISH_STALLED,
};

void pgraph_vk_finish(PGRAPHState *pg, FinishReason finish_reason)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

#if OPT_REORDER_SAFE_WINDOWS
    if (r->reorder_window.count > 0) {
        NV2AState *d = container_of(pg, NV2AState, pgraph);
        flush_reorder_window_internal(d);
    }
    r->reorder_window.active = false;
#endif

    assert(!r->in_draw);
    assert(r->debug_depth == 0);

    if (r->in_command_buffer) {
        nv2a_profile_inc_counter(finish_reason_to_counter_enum[finish_reason]);

        if (r->in_render_pass) {
            end_render_pass(r);
        }
        if (r->query_in_flight) {
            end_query(r);
        }
        VK_CHECK(vkEndCommandBuffer(r->command_buffer));

        VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg); // FIXME: Cleanup
        sync_staging_buffer(pg, cmd, BUFFER_INDEX_STAGING, BUFFER_INDEX);
        sync_staging_buffer(pg, cmd, BUFFER_VERTEX_INLINE_STAGING,
                                BUFFER_VERTEX_INLINE);
        sync_staging_buffer(pg, cmd, BUFFER_UNIFORM_STAGING, BUFFER_UNIFORM);
        bitmap_clear(r->uploaded_bitmap, 0, r->bitmap_size);
        flush_memory_buffer(pg, cmd);
        VK_CHECK(vkEndCommandBuffer(r->aux_command_buffer));
        r->in_aux_command_buffer = false;

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit_infos[] = {
            {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &r->aux_command_buffer,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &r->command_buffer_semaphore,
            },
            {

                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &r->command_buffer,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &r->command_buffer_semaphore,
                .pWaitDstStageMask = &wait_stage,
            }
        };
        nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT);
        vkResetFences(r->device, 1, &r->command_buffer_fence);
        VK_CHECK(vkQueueSubmit(r->queue, ARRAY_SIZE(submit_infos), submit_infos,
                               r->command_buffer_fence));
        r->submit_count += 1;

        bool check_budget = false;

        // Periodically check memory budget
        const int max_num_submits_before_budget_update = 5;
        if (finish_reason == VK_FINISH_REASON_FLIP_STALL ||
            (r->submit_count - r->allocator_last_submit_index) >
                max_num_submits_before_budget_update) {

            // VMA queries budget via vmaSetCurrentFrameIndex
            vmaSetCurrentFrameIndex(r->allocator, r->submit_count);
            r->allocator_last_submit_index = r->submit_count;
            check_budget = true;
        }

        VK_CHECK(vkWaitForFences(r->device, 1, &r->command_buffer_fence,
                                 VK_TRUE, UINT64_MAX));

        r->descriptor_set_index = 0;
        r->in_command_buffer = false;
        destroy_framebuffers(pg);

        if (check_budget) {
            pgraph_vk_check_memory_budget(pg);
        }
    }

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    pgraph_vk_process_pending_reports_internal(d);

    pgraph_vk_compute_finish_complete(r);
}

void pgraph_vk_begin_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(!r->in_command_buffer);

    VkCommandBufferBeginInfo command_buffer_begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->command_buffer,
                                  &command_buffer_begin_info));
    r->command_buffer_start_time = pg->draw_time;
    r->in_command_buffer = true;
#if OPT_BINDLESS_TEXTURES
    r->bindless_set_bound = false;
#endif
}

// FIXME: Refactor below

void pgraph_vk_ensure_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->in_command_buffer) {
        pgraph_vk_begin_command_buffer(pg);
    }
}

void pgraph_vk_ensure_not_in_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    end_render_pass(r);
    if (r->query_in_flight) {
        end_query(r);
    }
}

VkCommandBuffer pgraph_vk_begin_nondraw_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    pgraph_vk_ensure_command_buffer(pg);
    pgraph_vk_ensure_not_in_render_pass(pg);
    return r->command_buffer;
}

void pgraph_vk_end_nondraw_commands(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(cmd == r->command_buffer);
}

// FIXME: Add more metrics for determining command buffer 'fullness' and
// conservatively flush. Unfortunately there doesn't appear to be a good
// way to determine what the actual maximum capacity of a command buffer
// is, but we are obviously not supposed to endlessly append to one command
// buffer. For other reasons though (like descriptor set amount, surface
// changes, etc) we do flush often.

static void begin_pre_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    bool textures_clean = pgraph_vk_check_textures_fast_skip(pg);

#if OPT_ASYNC_COMPILE
    r->async_draw_skip = false;
#endif

    assert(r->color_binding || r->zeta_binding);
    assert(!r->color_binding || r->color_binding->initialized);
    assert(!r->zeta_binding || r->zeta_binding->initialized);

    if (!pg->clearing &&
        r->pipeline_binding &&
        r->shader_binding &&
        r->pipeline_binding->pipeline != VK_NULL_HANDLE &&
        !r->pipeline_binding_changed &&
        r->in_command_buffer &&
        r->in_render_pass &&
        r->framebuffer_index > 0 &&
        !r->framebuffer_dirty &&
        !r->uniforms_changed &&
        r->descriptor_set_index > 0 &&
        pg->texture_state_gen == r->last_texture_state_gen &&
        textures_clean &&
        pg->shader_state_gen == r->last_shader_state_gen &&
        pg->pipeline_state_gen == r->last_pipeline_state_gen &&
        pg->any_reg_gen == r->last_any_reg_gen &&
        pg->primitive_mode == r->shader_binding->state.geom.primitive_mode &&
        !pg->program_data_dirty &&
        pg->vertex_attr_gen == r->pipeline_vertex_attr_gen) {
        r->pre_draw_skipped = true;
        return;
    }

    r->pre_draw_skipped = false;

    if (pg->clearing) {
        create_clear_pipeline(pg);
    } else {
        create_pipeline(pg);
        r->pipeline_vertex_attr_gen = pg->vertex_attr_gen;
    }

#if OPT_ASYNC_COMPILE
    if (!pg->clearing && xemu_get_async_compile() && r->pipeline_binding) {
        bool skip = false;
        if (!r->shader_binding || !qatomic_read(&r->shader_binding->ready)) {
            skip = true;
        } else if (r->pipeline_binding->pending) {
            skip = true;
        } else if (r->pipeline_binding->pipeline == VK_NULL_HANDLE) {
            skip = true;
        }
        if (skip) {
            r->async_draw_skip = true;
            r->pre_draw_skipped = true;
            pgraph_vk_ensure_command_buffer(pg);
            return;
        }
    }
#endif

    bool render_pass_dirty = r->pipeline_binding->render_pass != r->render_pass;

    if (r->framebuffer_dirty || render_pass_dirty) {
        pgraph_vk_ensure_not_in_render_pass(pg);
    }
    if (render_pass_dirty) {
        r->render_pass = r->pipeline_binding->render_pass;
    }
    if (r->framebuffer_dirty) {
        create_frame_buffer(pg);
        r->framebuffer_dirty = false;
    }
    if (!pg->clearing) {
        pgraph_vk_update_descriptor_sets(pg);
        r->shader_bindings_changed = false;
        r->texture_bindings_changed = false;
    }
    if (r->framebuffer_index == 0) {
        create_frame_buffer(pg);
    }

    pgraph_vk_ensure_command_buffer(pg);
    pgraph_clear_dirty_reg_map(pg);
    r->last_any_reg_gen = pg->any_reg_gen;
    r->last_shader_state_gen = pg->shader_state_gen;
    r->last_pipeline_state_gen = pg->pipeline_state_gen;
}

static float clamp_line_width_to_device_limits(PGRAPHState *pg, float width)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    float min_width = r->device_props.limits.lineWidthRange[0];
    float max_width = r->device_props.limits.lineWidthRange[1];
    float granularity = r->device_props.limits.lineWidthGranularity;

    if (granularity != 0.0f) {
        float steps = roundf((width - min_width) / granularity);
        width = min_width + steps * granularity;
    }
    return fminf(fmaxf(min_width, width), max_width);
}

static void begin_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);

    // Visibility testing
    if (!pg->clearing && pg->zpass_pixel_count_enable) {
        if (r->new_query_needed && r->query_in_flight) {
            end_render_pass(r);
            end_query(r);
        }
        if (!r->query_in_flight) {
            end_render_pass(r);
            begin_query(r);
        }
    } else if (r->query_in_flight) {
        end_render_pass(r);
        end_query(r);
    }

    if (pg->clearing) {
        end_render_pass(r);
    }

    bool must_bind_pipeline = r->pipeline_binding_changed;

    if (!r->in_render_pass) {
        begin_render_pass(pg);
        must_bind_pipeline = true;
    }

    if (must_bind_pipeline) {
        nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_BIND);
        vkCmdBindPipeline(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          r->pipeline_binding->pipeline);
        r->pipeline_binding_changed = false;
        r->pipeline_binding->draw_time = pg->draw_time;

        unsigned int vp_width = pg->surface_binding_dim.width,
                     vp_height = pg->surface_binding_dim.height;
        pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);

        VkViewport viewport = {
            .width = vp_width,
            .height = vp_height,
            .minDepth = 0.0,
            .maxDepth = 1.0,
        };
        vkCmdSetViewport(r->command_buffer, 0, 1, &viewport);

        /* Surface clip */
        /* FIXME: Consider moving to PSH w/ window clip */
        unsigned int xmin = pg->surface_shape.clip_x,
                     ymin = pg->surface_shape.clip_y;

        unsigned int scissor_width = pg->surface_shape.clip_width,
                     scissor_height = pg->surface_shape.clip_height;

        pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
        pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

        pgraph_apply_scaling_factor(pg, &xmin, &ymin);
        pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

        VkRect2D scissor = {
            .offset.x = xmin,
            .offset.y = ymin,
            .extent.width = scissor_width,
            .extent.height = scissor_height,
        };
        vkCmdSetScissor(r->command_buffer, 0, 1, &scissor);

        if (r->pipeline_binding->has_dynamic_line_width) {
            float line_width =
                clamp_line_width_to_device_limits(pg, pg->surface_scale_factor);
            vkCmdSetLineWidth(r->command_buffer, line_width);
        }
    }

    if (!pg->clearing && !r->pre_draw_skipped) {
#if OPT_BINDLESS_TEXTURES
        if (r->bindless_textures_supported && !r->bindless_set_bound) {
            bind_bindless_set(pg);
            r->bindless_set_bound = true;
        }
#endif
        bind_descriptor_sets(pg);
        push_vertex_attr_values(pg);
#if OPT_BINDLESS_TEXTURES
        push_texture_indices(pg);
#endif
    }

    r->in_draw = true;
}

static void end_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);
    assert(r->in_render_pass);

    if (pg->clearing) {
        end_render_pass(r);
    }

    r->in_draw = false;
}

void pgraph_vk_draw_end(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    if (is_nop_draw) {
        // FIXME: Check PGRAPH register 0x880.
        // HW uses bit 11 in 0x880 to enable or disable a color/zeta limit
        // check that will raise an exception in the case that a draw should
        // modify the color and/or zeta buffer but the target(s) are masked
        // off. This check only seems to trigger during the fragment
        // processing, it is legal to attempt a draw that is entirely
        // clipped regardless of 0x880. See xemu#635 for context.
        NV2A_VK_DPRINTF("nop draw!\n");
        return;
    }

#if OPT_REORDER_SAFE_WINDOWS
    if (g_xemu_draw_reorder &&
        (pg->draw_arrays_length || pg->inline_elements_length) &&
        !pg->clearing) {
        ReorderWindow *window = &r->reorder_window;
        bool is_safe = classify_draw_safe(pg);

        if (is_safe && r->framebuffer_dirty) {
            is_safe = false;
        }
        if (is_safe && pg->zpass_pixel_count_enable) {
            is_safe = false;
        }

        if (is_safe && window->count < REORDER_WINDOW_MAX) {
            ReorderWindowEntry *entry = &window->entries[window->count];
            bool ok = pg->draw_arrays_length ?
                try_snapshot_draw_arrays(d, entry) :
                try_snapshot_inline_elements(d, entry);
            if (ok) {
                entry->sequence_number = window->count;

                int group = -1;
                for (int i = 0; i < window->num_seen_pipelines; i++) {
                    if (window->seen_pipelines[i] == entry->pipeline_binding) {
                        group = window->seen_pipeline_group[i];
                        break;
                    }
                }
                if (group < 0) {
                    if (window->num_seen_pipelines < REORDER_MAX_PIPELINES) {
                        window->seen_pipelines[window->num_seen_pipelines] =
                            entry->pipeline_binding;
                        window->seen_pipeline_group[window->num_seen_pipelines] =
                            window->next_group;
                        window->num_seen_pipelines++;
                    }
                    group = window->next_group++;
                }
                entry->group_order = group;

                window->count++;
                window->active = true;
                return;
            }
        }

        if (window->count > 0) {
            flush_reorder_window_internal(d);
        }
        window->active = false;
    } else if (r->reorder_window.count > 0) {
        flush_reorder_window_internal(d);
        r->reorder_window.active = false;
    }
#endif

    pgraph_vk_flush_draw(d);

    pg->draw_time++;
    if (r->color_binding && pgraph_color_write_enabled(pg)) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && pgraph_zeta_write_enabled(pg)) {
        r->zeta_binding->draw_time = pg->draw_time;
    }

    pgraph_vk_set_surface_dirty(pg, color_write, depth_test || stencil_test);
}

static int compare_memory_sync_requirement_by_addr(const void *p1,
                                                   const void *p2)
{
    const MemorySyncRequirement *l = p1, *r = p2;
    if (l->addr < r->addr)
        return -1;
    if (l->addr > r->addr)
        return 1;
    return 0;
}

static void sync_vertex_ram_buffer(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_vertex_ram_buffer_syncs == 0) {
        return;
    }

    // Align sync requirements to page boundaries
    NV2A_VK_DGROUP_BEGIN("Sync vertex RAM buffer");

    for (int i = 0; i < r->num_vertex_ram_buffer_syncs; i++) {
        NV2A_VK_DPRINTF("Need to sync vertex memory @%" HWADDR_PRIx
                        ", %" HWADDR_PRIx " bytes",
                        r->vertex_ram_buffer_syncs[i].addr,
                        r->vertex_ram_buffer_syncs[i].size);

        hwaddr start_addr =
            r->vertex_ram_buffer_syncs[i].addr & TARGET_PAGE_MASK;
        hwaddr end_addr = r->vertex_ram_buffer_syncs[i].addr +
                          r->vertex_ram_buffer_syncs[i].size;
        end_addr = ROUND_UP(end_addr, TARGET_PAGE_SIZE);

        NV2A_VK_DPRINTF("- %d: %08" HWADDR_PRIx " %zd bytes"
                          " -> %08" HWADDR_PRIx " %zd bytes", i,
                        r->vertex_ram_buffer_syncs[i].addr,
                        r->vertex_ram_buffer_syncs[i].size, start_addr,
                        end_addr - start_addr);

        r->vertex_ram_buffer_syncs[i].addr = start_addr;
        r->vertex_ram_buffer_syncs[i].size = end_addr - start_addr;
    }

    // Sort the requirements in increasing order of addresses
    qsort(r->vertex_ram_buffer_syncs, r->num_vertex_ram_buffer_syncs,
          sizeof(MemorySyncRequirement),
          compare_memory_sync_requirement_by_addr);

    // Merge overlapping/adjacent requests to minimize number of tests
    MemorySyncRequirement merged[16];
    int num_syncs = 1;

    merged[0] = r->vertex_ram_buffer_syncs[0];

    for (int i = 1; i < r->num_vertex_ram_buffer_syncs; i++) {
        MemorySyncRequirement *p = &merged[num_syncs - 1];
        MemorySyncRequirement *t = &r->vertex_ram_buffer_syncs[i];

        if (t->addr <= (p->addr + p->size)) {
            // Merge with previous
            hwaddr p_end_addr = p->addr + p->size;
            hwaddr t_end_addr = t->addr + t->size;
            hwaddr new_end_addr = MAX(p_end_addr, t_end_addr);
            p->size = new_end_addr - p->addr;
        } else {
            merged[num_syncs++] = *t;
        }
    }

    if (num_syncs < r->num_vertex_ram_buffer_syncs) {
        NV2A_VK_DPRINTF("Reduced to %d sync checks", num_syncs);
    }

    for (int i = 0; i < num_syncs; i++) {
        hwaddr addr = merged[i].addr;
        VkDeviceSize size = merged[i].size;

        NV2A_VK_DPRINTF("- %d: %08"HWADDR_PRIx" %zd bytes", i, addr, size);

        if (memory_region_test_and_clear_dirty(d->vram, addr, size,
                                               DIRTY_MEMORY_NV2A)) {
            NV2A_VK_DPRINTF("Memory dirty. Synchronizing...");
            pgraph_vk_update_vertex_ram_buffer(pg, addr, d->vram_ptr + addr,
                                               size);
        }
    }

    r->num_vertex_ram_buffer_syncs = 0;

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

#if OPT_REORDER_SAFE_WINDOWS
    pgraph_vk_flush_reorder_window(d);
#endif

    nv2a_profile_inc_counter(NV2A_PROF_CLEAR);

    bool write_color = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_zeta =
        (parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

    pg->clearing = true;

    // FIXME: If doing a full surface clear, mark the surface for full clear
    // and we can just do the clear as part of the surface load.
    pgraph_vk_surface_update(d, true, write_color, write_zeta);

    SurfaceBinding *binding = r->color_binding ?: r->zeta_binding;
    if (!binding) {
        /* Nothing bound to clear */
        pg->clearing = false;
        return;
    }

    r->clear_parameter = parameter;

    uint32_t clearrectx = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    uint32_t clearrecty = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);

    unsigned int xmin = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMAX);

    NV2A_VK_DGROUP_BEGIN("CLEAR min=(%d,%d) max=(%d,%d)%s%s", xmin, ymin, xmax,
                         ymax, write_color ? " color" : "",
                         write_zeta ? " zeta" : "");

    begin_pre_draw(pg);
    pgraph_vk_begin_debug_marker(r, r->command_buffer,
        RGBA_BLUE, "Clear %08" HWADDR_PRIx,
        binding->vram_addr);
    begin_draw(pg);

    // FIXME: What does hardware do when min >= max?
    // FIXME: What does hardware do when min >= surface size?
    xmin = MIN(xmin, binding->width - 1);
    ymin = MIN(ymin, binding->height - 1);
    xmax = MAX(xmin, MIN(xmax, binding->width - 1));
    ymax = MAX(ymin, MIN(ymax, binding->height - 1));

    unsigned int scissor_width = MAX(0, xmax - xmin + 1);
    unsigned int scissor_height = MAX(0, ymax - ymin + 1);

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    VkClearRect clear_rect = {
        .rect = {
            .offset = { .x = xmin, .y = ymin },
            .extent = { .width = scissor_width, .height = scissor_height },
        },
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    int num_attachments = 0;
    VkClearAttachment attachments[2];

    if (write_color && r->color_binding) {
        const bool clear_all_color_channels =
            (parameter & NV097_CLEAR_SURFACE_COLOR) ==
            (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G |
             NV097_CLEAR_SURFACE_B | NV097_CLEAR_SURFACE_A);

        if (clear_all_color_channels) {
            attachments[num_attachments] = (VkClearAttachment){
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .colorAttachment = 0,
            };
            pgraph_get_clear_color(
                pg, attachments[num_attachments].clearValue.color.float32);
            num_attachments++;
        } else {
            float blend_constants[4];
            pgraph_get_clear_color(pg, blend_constants);
            vkCmdSetScissor(r->command_buffer, 0, 1, &clear_rect.rect);
            vkCmdSetBlendConstants(r->command_buffer, blend_constants);
            vkCmdDraw(r->command_buffer, 3, 1, 0, 0);
        }
    }

    if (write_zeta && r->zeta_binding) {
        int stencil_value = 0;
        float depth_value = 1.0;
        pgraph_get_clear_depth_stencil_value(pg, &depth_value, &stencil_value);

        VkImageAspectFlags aspect = 0;
        if (parameter & NV097_CLEAR_SURFACE_Z) {
            aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if ((parameter & NV097_CLEAR_SURFACE_STENCIL) &&
            (r->zeta_binding->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        attachments[num_attachments++] = (VkClearAttachment){
            .aspectMask = aspect,
            .clearValue.depthStencil.depth = depth_value,
            .clearValue.depthStencil.stencil = stencil_value,
        };
    }

    if (num_attachments) {
        vkCmdClearAttachments(r->command_buffer, num_attachments, attachments,
                              1, &clear_rect);
    }
    end_draw(pg);
    pgraph_vk_end_debug_marker(r, r->command_buffer);

    pg->clearing = false;

    pgraph_vk_set_surface_dirty(pg, write_color, write_zeta);

    NV2A_VK_DGROUP_END();
}

#if 0
static void pgraph_vk_debug_attrs(NV2AState *d)
{
    for (int vertex_idx = 0; vertex_idx < pg->draw_arrays_count[i]; vertex_idx++) {
        NV2A_VK_DGROUP_BEGIN("Vertex %d+%d", pg->draw_arrays_start[i], vertex_idx);
        for (int attr_idx = 0; attr_idx < NV2A_VERTEXSHADER_ATTRIBUTES; attr_idx++) {
            VertexAttribute *attr = &pg->vertex_attributes[attr_idx];
            if (attr->count) {
                char *p = (char *)d->vram_ptr + r->attribute_offsets[attr_idx] + (pg->draw_arrays_start[i] + vertex_idx) * attr->stride;
                NV2A_VK_DGROUP_BEGIN("Attribute %d data at %tx", attr_idx, (ptrdiff_t)(p - (char*)d->vram_ptr));
                for (int count_idx = 0; count_idx < attr->count; count_idx++) {
                    switch (attr->format) {
                    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
                        NV2A_VK_DPRINTF("[%d] %f", count_idx, *(float*)p);
                        p += sizeof(float);
                        break;
                    default:
                        assert(0);
                        break;
                    }
                }
                NV2A_VK_DGROUP_END();
            }
        }
        NV2A_VK_DGROUP_END();
    }
}
#endif

static void bind_vertex_buffer(PGRAPHState *pg, uint16_t inline_map,
                               VkDeviceSize offset)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_active_vertex_binding_descriptions == 0) {
        return;
    }

    VkBuffer buffers[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkDeviceSize offsets[NV2A_VERTEXSHADER_ATTRIBUTES];

    for (int i = 0; i < r->num_active_vertex_binding_descriptions; i++) {
        int attr_idx = r->vertex_attribute_descriptions[i].location;
        int buffer_idx = (inline_map & (1 << attr_idx)) ? BUFFER_VERTEX_INLINE :
                                                          BUFFER_VERTEX_RAM;
        buffers[i] = r->storage_buffers[buffer_idx].buffer;
        offsets[i] = offset + r->vertex_attribute_offsets[attr_idx];
    }

    vkCmdBindVertexBuffers(r->command_buffer, 0,
                           r->num_active_vertex_binding_descriptions, buffers,
                           offsets);
}

static void bind_inline_vertex_buffer(PGRAPHState *pg, VkDeviceSize offset)
{
    bind_vertex_buffer(pg, 0xffff, offset);
}

#if OPT_REORDER_SAFE_WINDOWS
static bool check_rt_as_texture_hazard(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        TextureBinding *binding = r->texture_bindings[i];
        if (!binding || binding == &r->dummy_texture) {
            continue;
        }

        hwaddr tex_start = binding->key.texture_vram_offset;
        hwaddr tex_end = tex_start + binding->key.texture_length;

        if (r->color_binding) {
            hwaddr rt_start = r->color_binding->vram_addr;
            hwaddr rt_end = rt_start + r->color_binding->size;
            if (tex_start < rt_end && rt_start < tex_end) {
                return true;
            }
        }

        if (r->zeta_binding) {
            hwaddr zt_start = r->zeta_binding->vram_addr;
            hwaddr zt_end = zt_start + r->zeta_binding->size;
            if (tex_start < zt_end && zt_start < tex_end) {
                return true;
            }
        }
    }

    return false;
}

static bool classify_draw_safe(PGRAPHState *pg)
{
    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint32_t blend = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
    uint32_t control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);

    if (blend & NV_PGRAPH_BLEND_EN) {
        uint32_t src = GET_MASK(blend, NV_PGRAPH_BLEND_SFACTOR);
        uint32_t dst = GET_MASK(blend, NV_PGRAPH_BLEND_DFACTOR);
        if (src != NV_PGRAPH_BLEND_SFACTOR_ONE ||
            dst != NV_PGRAPH_BLEND_DFACTOR_ZERO) {
            return false;
        }
    }

    if ((control_0 & (NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE |
                      NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE |
                      NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE |
                      NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE)) !=
        (NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE |
         NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE |
         NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE |
         NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE)) {
        return false;
    }

    if (!(control_0 & NV_PGRAPH_CONTROL_0_ZENABLE) ||
        !(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE)) {
        return false;
    }

    uint32_t zfunc = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ZFUNC);
    if (zfunc != NV_PGRAPH_CONTROL_0_ZFUNC_LESS &&
        zfunc != NV_PGRAPH_CONTROL_0_ZFUNC_LEQUAL) {
        return false;
    }

    if (control_1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE) {
        return false;
    }

    if (control_0 & NV_PGRAPH_CONTROL_0_ALPHATESTENABLE) {
        uint32_t afunc = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ALPHAFUNC);
        uint32_t aref = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ALPHAREF);
        bool safe_alpha = (afunc == ALPHA_FUNC_ALWAYS) ||
                          (afunc == ALPHA_FUNC_GREATER && aref == 0);
        if (!safe_alpha) {
            return false;
        }
    }

    if (check_rt_as_texture_hazard(pg)) {
        return false;
    }

    return true;
}

static bool reorder_uses_bindless_direct_surface_textures(PGRAPHVkState *r)
{
#if OPT_BINDLESS_TEXTURES
    if (r->bindless_textures_supported) {
        for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            if (r->tex_surface_direct[i]) {
                return true;
            }
        }
    }
#endif
    return false;
}

static void snapshot_vertex_buffers(PGRAPHState *pg, ReorderWindowEntry *entry,
                                    uint16_t inline_map, VkDeviceSize offset)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    entry->num_vertex_bindings = r->num_active_vertex_binding_descriptions;
    for (int i = 0; i < entry->num_vertex_bindings; i++) {
        int attr_idx = r->vertex_attribute_descriptions[i].location;
        int buffer_idx = (inline_map & (1 << attr_idx)) ? BUFFER_VERTEX_INLINE :
                                                          BUFFER_VERTEX_RAM;
        entry->vertex_buffers[i] = r->storage_buffers[buffer_idx].buffer;
        entry->vertex_offsets[i] =
            offset + r->vertex_attribute_offsets[attr_idx];
    }
}

static void snapshot_draw_state(PGRAPHState *pg, ReorderWindowEntry *entry)
{
    unsigned int vp_width = pg->surface_binding_dim.width;
    unsigned int vp_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);
    entry->viewport = (VkViewport){
        .width = vp_width,
        .height = vp_height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    unsigned int xmin = pg->surface_shape.clip_x;
    unsigned int ymin = pg->surface_shape.clip_y;
    unsigned int scissor_width = pg->surface_shape.clip_width;
    unsigned int scissor_height = pg->surface_shape.clip_height;

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);
    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    entry->scissor = (VkRect2D){
        .offset = { .x = xmin, .y = ymin },
        .extent = { .width = scissor_width, .height = scissor_height },
    };
}

static void snapshot_push_constants(PGRAPHState *pg, ReorderWindowEntry *entry)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    entry->use_push_constants = can_push_vertex_attr_values(r);
    entry->num_push_values = 0;

    if (!entry->use_push_constants || !r->shader_binding) {
        return;
    }

    float values[NV2A_VERTEXSHADER_ATTRIBUTES][4];
    int num_uniform_attrs = 0;
    pgraph_get_inline_values(pg, r->shader_binding->state.vsh.uniform_attrs,
                             values, &num_uniform_attrs);
    entry->num_push_values = num_uniform_attrs;
    if (num_uniform_attrs > 0) {
        memcpy(entry->push_values, values,
               num_uniform_attrs * 4 * sizeof(float));
    }
}

static void snapshot_texture_indices(PGRAPHState *pg, ReorderWindowEntry *entry)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    entry->use_bindless_textures = false;
#if OPT_BINDLESS_TEXTURES
    entry->use_bindless_textures = r->bindless_textures_supported;
    if (entry->use_bindless_textures) {
        memcpy(entry->tex_bindless_indices, r->tex_bindless_indices,
               sizeof(entry->tex_bindless_indices));
    }
#endif
}

static bool try_snapshot_draw_arrays(NV2AState *d, ReorderWindowEntry *entry)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        return false;
    }

    nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);
    r->num_vertex_ram_buffer_syncs = 0;

    PrimAssemblyState assembly = {
        .primitive_mode = pg->primitive_mode,
        .polygon_mode = (enum ShaderPolygonMode)GET_MASK(
            pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
            NV_PGRAPH_SETUPRASTER_FRONTFACEMODE),
        .last_provoking = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                                   NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
                          NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_LAST,
        .flat_shading = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                                 NV_PGRAPH_CONTROL_3_SHADEMODE) ==
                        NV_PGRAPH_CONTROL_3_SHADEMODE_FLAT,
    };

    pgraph_vk_bind_vertex_attributes(d, pg->draw_arrays_min_start,
                                     pg->draw_arrays_max_count - 1, false, 0,
                                     pg->draw_arrays_max_count - 1);

    uint32_t max_element = 0;
    for (int i = 0; i < pg->draw_arrays_length; i++) {
        max_element = MAX(max_element, pg->draw_arrays_start[i] +
                                           pg->draw_arrays_count[i]);
    }

    sync_vertex_ram_buffer(pg);
    VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element);

    PrimRewrite prim_rw = pgraph_prim_rewrite_ranges(
        &r->prim_rewrite_buf, assembly, pg->draw_arrays_start,
        pg->draw_arrays_count, pg->draw_arrays_length);

    if (prim_rw.num_indices > 0) {
        ensure_buffer_space(pg, BUFFER_INDEX_STAGING,
                            prim_rw.num_indices * sizeof(uint32_t));
    } else if (pg->draw_arrays_length > 1) {
        ensure_buffer_space(pg, BUFFER_INDEX_STAGING,
                            pg->draw_arrays_length *
                                sizeof(VkDrawIndirectCommand));
    }

    begin_pre_draw(pg);
    if (r->async_draw_skip ||
        reorder_uses_bindless_direct_surface_textures(r) ||
        !r->pipeline_binding || r->descriptor_set_index == 0) {
        return false;
    }

    copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element);

    entry->pipeline_binding = r->pipeline_binding;
    entry->layout = r->pipeline_binding->layout;
    entry->descriptor_set = r->descriptor_sets[r->descriptor_set_index - 1];
    entry->has_dynamic_line_width = r->pipeline_binding->has_dynamic_line_width;
    if (entry->has_dynamic_line_width) {
        entry->line_width =
            clamp_line_width_to_device_limits(pg, pg->surface_scale_factor);
    }

    snapshot_vertex_buffers(pg, entry, remap.attributes, 0);
    snapshot_draw_state(pg, entry);
    snapshot_push_constants(pg, entry);
    snapshot_texture_indices(pg, entry);

    if (prim_rw.num_indices > 0) {
        entry->draw_mode = RW_DRAW_INDEXED;
        entry->draw_count = prim_rw.num_indices;
        entry->index_indirect_offset = pgraph_vk_update_index_buffer(
            pg, prim_rw.indices, prim_rw.num_indices * sizeof(uint32_t));
    } else if (pg->draw_arrays_length > 1) {
        VkDrawIndirectCommand cmds[pg->draw_arrays_length];
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            cmds[i] = (VkDrawIndirectCommand){
                .vertexCount = pg->draw_arrays_count[i],
                .instanceCount = 1,
                .firstVertex = pg->draw_arrays_start[i],
                .firstInstance = 0,
            };
        }
        entry->draw_mode = RW_DRAW_INDIRECT;
        entry->draw_count = pg->draw_arrays_length;
        entry->index_indirect_offset = pgraph_vk_update_index_buffer(
            pg, cmds, sizeof(cmds));
    } else {
        entry->draw_mode = RW_DRAW_DIRECT;
        entry->draw_count = 1;
        entry->vertex_count = pg->draw_arrays_count[0];
        entry->first_vertex = pg->draw_arrays_start[0];
    }

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    entry->color_write =
        (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE) ||
        (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE) ||
        (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE) ||
        (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE);
    entry->depth_test = !!(control_0 & NV_PGRAPH_CONTROL_0_ZENABLE);
    entry->stencil_test =
        !!(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) &
           NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE);

    return true;
}

static bool try_snapshot_inline_elements(NV2AState *d,
                                         ReorderWindowEntry *entry)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        return false;
    }

    nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);

    PrimAssemblyState assembly = {
        .primitive_mode = pg->primitive_mode,
        .polygon_mode = (enum ShaderPolygonMode)GET_MASK(
            pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
            NV_PGRAPH_SETUPRASTER_FRONTFACEMODE),
        .last_provoking = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                                   NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
                          NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_LAST,
        .flat_shading = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                                 NV_PGRAPH_CONTROL_3_SHADEMODE) ==
                        NV_PGRAPH_CONTROL_3_SHADEMODE_FLAT,
    };

    uint32_t *draw_indices = pg->inline_elements;
    unsigned int draw_index_count = pg->inline_elements_length;
    PrimRewrite prim_rw = pgraph_prim_rewrite_indexed(
        &r->prim_rewrite_buf, assembly, pg->inline_elements,
        pg->inline_elements_length);
    if (prim_rw.num_indices > 0) {
        draw_indices = prim_rw.indices;
        draw_index_count = prim_rw.num_indices;
    }

    size_t index_data_size = draw_index_count * sizeof(uint32_t);
    ensure_buffer_space(pg, BUFFER_INDEX_STAGING, index_data_size);

    uint32_t min_element = UINT32_MAX;
    uint32_t max_element = 0;
    for (unsigned int i = 0; i < draw_index_count; i++) {
        max_element = MAX(draw_indices[i], max_element);
        min_element = MIN(draw_indices[i], min_element);
    }

    pgraph_vk_bind_vertex_attributes(
        d, min_element, max_element, false, 0,
        draw_indices[draw_index_count - 1]);
    sync_vertex_ram_buffer(pg);
    VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element + 1);

    begin_pre_draw(pg);
    if (r->async_draw_skip ||
        reorder_uses_bindless_direct_surface_textures(r) ||
        !r->pipeline_binding || r->descriptor_set_index == 0) {
        return false;
    }

    copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element + 1);

    entry->pipeline_binding = r->pipeline_binding;
    entry->layout = r->pipeline_binding->layout;
    entry->descriptor_set = r->descriptor_sets[r->descriptor_set_index - 1];
    entry->has_dynamic_line_width = r->pipeline_binding->has_dynamic_line_width;
    if (entry->has_dynamic_line_width) {
        entry->line_width =
            clamp_line_width_to_device_limits(pg, pg->surface_scale_factor);
    }

    snapshot_vertex_buffers(pg, entry, remap.attributes, 0);
    snapshot_draw_state(pg, entry);
    snapshot_push_constants(pg, entry);
    snapshot_texture_indices(pg, entry);

    entry->draw_mode = RW_DRAW_INDEXED;
    entry->draw_count = draw_index_count;
    entry->index_indirect_offset = pgraph_vk_update_index_buffer(
        pg, draw_indices, index_data_size);

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    entry->color_write =
        (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE) ||
        (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE) ||
        (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE) ||
        (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE);
    entry->depth_test = !!(control_0 & NV_PGRAPH_CONTROL_0_ZENABLE);
    entry->stencil_test =
        !!(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) &
           NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE);

    return true;
}

static int compare_reorder_entries(const void *a, const void *b)
{
    const ReorderWindowEntry *left = a;
    const ReorderWindowEntry *right = b;

    if (left->group_order < right->group_order) {
        return -1;
    }
    if (left->group_order > right->group_order) {
        return 1;
    }
    return left->sequence_number - right->sequence_number;
}

static void emit_reorder_entry(PGRAPHState *pg, ReorderWindowEntry *entry,
                               PipelineBinding *prev_pipeline)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    bool pipeline_changed = (entry->pipeline_binding != prev_pipeline);

    if (!r->in_render_pass) {
        begin_render_pass(pg);
        pipeline_changed = true;
    }

    if (pipeline_changed) {
        nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_BIND);
        vkCmdBindPipeline(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          entry->pipeline_binding->pipeline);
        entry->pipeline_binding->draw_time = pg->draw_time;
        vkCmdSetViewport(r->command_buffer, 0, 1, &entry->viewport);
        vkCmdSetScissor(r->command_buffer, 0, 1, &entry->scissor);
        if (entry->has_dynamic_line_width) {
            vkCmdSetLineWidth(r->command_buffer, entry->line_width);
        }
    }

#if OPT_BINDLESS_TEXTURES
    if (entry->use_bindless_textures) {
        vkCmdBindDescriptorSets(r->command_buffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                entry->layout, 0, 1,
                                &r->bindless_descriptor_set, 0, NULL);
        r->bindless_set_bound = true;
        vkCmdPushConstants(r->command_buffer, entry->layout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, r->tex_push_offset,
                           sizeof(entry->tex_bindless_indices),
                           entry->tex_bindless_indices);
    }
#endif

    vkCmdBindDescriptorSets(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            entry->layout,
                            entry->use_bindless_textures ? 1 : 0, 1,
                            &entry->descriptor_set, 0, NULL);

    if (entry->use_push_constants && entry->num_push_values > 0) {
#if OPT_BINDLESS_TEXTURES
        uint32_t vertex_push_offset =
            entry->use_bindless_textures && r->tex_push_offset == 0
                ? NV2A_MAX_TEXTURES * sizeof(uint32_t)
                : 0;
#else
        uint32_t vertex_push_offset = 0;
#endif
        vkCmdPushConstants(r->command_buffer, entry->layout,
                           VK_SHADER_STAGE_VERTEX_BIT, vertex_push_offset,
                           entry->num_push_values * 4 * sizeof(float),
                           entry->push_values);
    }

    if (entry->num_vertex_bindings > 0) {
        vkCmdBindVertexBuffers(r->command_buffer, 0,
                               entry->num_vertex_bindings,
                               entry->vertex_buffers, entry->vertex_offsets);
    }

    switch (entry->draw_mode) {
    case RW_DRAW_INDEXED:
        vkCmdBindIndexBuffer(r->command_buffer,
                             r->storage_buffers[BUFFER_INDEX].buffer,
                             entry->index_indirect_offset,
                             VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(r->command_buffer, entry->draw_count, 1, 0, 0, 0);
        break;
    case RW_DRAW_INDIRECT:
        vkCmdDrawIndirect(r->command_buffer,
                          r->storage_buffers[BUFFER_INDEX].buffer,
                          entry->index_indirect_offset, entry->draw_count,
                          sizeof(VkDrawIndirectCommand));
        break;
    case RW_DRAW_DIRECT:
        vkCmdDraw(r->command_buffer, entry->vertex_count, 1,
                  entry->first_vertex, 0);
        break;
    }
}

static void flush_reorder_window_internal(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    ReorderWindow *window = &r->reorder_window;

    if (window->count == 0) {
        return;
    }

    qsort(window->entries, window->count, sizeof(window->entries[0]),
          compare_reorder_entries);

    pgraph_vk_ensure_command_buffer(pg);

    PipelineBinding *prev_pipeline = NULL;
    for (int i = 0; i < window->count; i++) {
        ReorderWindowEntry *entry = &window->entries[i];
        emit_reorder_entry(pg, entry, prev_pipeline);
        prev_pipeline = entry->pipeline_binding;

        pg->draw_time++;
        if (r->color_binding && entry->color_write) {
            r->color_binding->draw_time = pg->draw_time;
        }
        if (r->zeta_binding && (entry->depth_test || entry->stencil_test)) {
            r->zeta_binding->draw_time = pg->draw_time;
        }

        pgraph_vk_set_surface_dirty(pg, entry->color_write,
                                    entry->depth_test || entry->stencil_test);
    }

    r->pipeline_binding = prev_pipeline;
    r->pipeline_binding_changed = false;
    window->count = 0;
    window->active = false;
    window->num_seen_pipelines = 0;
    window->next_group = 0;
}
#endif

void pgraph_vk_flush_reorder_window(NV2AState *d)
{
#if OPT_REORDER_SAFE_WINDOWS
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    if (r->reorder_window.count > 0) {
        flush_reorder_window_internal(d);
    }
    r->reorder_window.active = false;
#endif
}

void pgraph_vk_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta)
{
    NV2A_DPRINTF("pgraph_set_surface_dirty(%d, %d) -- %d %d\n", color, zeta,
                 pgraph_color_write_enabled(pg), pgraph_zeta_write_enabled(pg));

    PGRAPHVkState *r = pg->vk_renderer_state;

    /* FIXME: Does this apply to CLEARs too? */
    color = color && pgraph_color_write_enabled(pg);
    zeta = zeta && pgraph_zeta_write_enabled(pg);
    pg->surface_color.draw_dirty |= color;
    pg->surface_zeta.draw_dirty |= zeta;

    if (r->color_binding) {
        r->color_binding->draw_dirty |= color;
        if (color) {
            r->color_binding->draw_generation++;
        }
        r->color_binding->frame_time = pg->frame_time;
        r->color_binding->cleared = false;
    }

    if (r->zeta_binding) {
        r->zeta_binding->draw_dirty |= zeta;
        if (zeta) {
            r->zeta_binding->draw_generation++;
        }
        r->zeta_binding->frame_time = pg->frame_time;
        r->zeta_binding->cleared = false;
    }
}

static bool ensure_buffer_space(PGRAPHState *pg, int index, VkDeviceSize size)
{
    if (!pgraph_vk_buffer_has_space_for(pg, index, size, 1)) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        return true;
    }

    return false;
}

static void get_size_and_count_for_format(VkFormat fmt, size_t *size, size_t *count)
{
    static const struct {
        size_t size;
        size_t count;
    } table[] = {
        [VK_FORMAT_R8_UNORM] =              { 1, 1 },
        [VK_FORMAT_R8G8_UNORM] =            { 1, 2 },
        [VK_FORMAT_R8G8B8_UNORM] =          { 1, 3 },
        [VK_FORMAT_R8G8B8A8_UNORM] =        { 1, 4 },
        [VK_FORMAT_R16_SNORM] =             { 2, 1 },
        [VK_FORMAT_R16G16_SNORM] =          { 2, 2 },
        [VK_FORMAT_R16G16B16_SNORM] =       { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SNORM] =    { 2, 4 },
        [VK_FORMAT_R16_SSCALED] =           { 2, 1 },
        [VK_FORMAT_R16G16_SSCALED] =        { 2, 2 },
        [VK_FORMAT_R16G16B16_SSCALED] =     { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SSCALED] =  { 2, 4 },
        [VK_FORMAT_R32_SFLOAT] =            { 4, 1 },
        [VK_FORMAT_R32G32_SFLOAT] =         { 4, 2 },
        [VK_FORMAT_R32G32B32_SFLOAT] =      { 4, 3 },
        [VK_FORMAT_R32G32B32A32_SFLOAT] =   { 4, 4 },
        [VK_FORMAT_R32_SINT] =              { 4, 1 },
    };

    assert(fmt < ARRAY_SIZE(table));
    assert(table[fmt].size);

    *size = table[fmt].size;
    *count = table[fmt].count;
}

static VertexBufferRemap remap_unaligned_attributes(PGRAPHState *pg,
                                                    uint32_t num_vertices)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VertexBufferRemap remap = {0};

    VkDeviceAddress output_offset = 0;

    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        int desc_loc = r->vertex_attribute_to_description_location[attr_id];
        if (desc_loc < 0) {
            continue;
        }

        VkVertexInputBindingDescription *desc =
            &r->vertex_binding_descriptions[desc_loc];
        VkVertexInputAttributeDescription *attr =
            &r->vertex_attribute_descriptions[desc_loc];

        size_t element_size, element_count;
        get_size_and_count_for_format(attr->format, &element_size, &element_count);

        bool offset_valid =
            (r->vertex_attribute_offsets[attr_id] % element_size == 0);
        bool stride_valid = (desc->stride % element_size == 0);

        if (offset_valid && stride_valid) {
            continue;
        }

        remap.attributes |= 1 << attr_id;
        remap.map[attr_id].offset = ROUND_UP(output_offset, element_size);
        remap.map[attr_id].old_stride = desc->stride;
        remap.map[attr_id].new_stride = element_size * element_count;

        // fprintf(stderr,
        //         "attr %02d remapped: "
        //         "%08" HWADDR_PRIx "->%08" HWADDR_PRIx " "
        //         "stride=%d->%zd\n",
        //         attr_id, r->vertex_attribute_offsets[attr_id],
        //         remap.map[attr_id].offset,
        //         remap.map[attr_id].old_stride,
        //         remap.map[attr_id].new_stride);

        output_offset =
            remap.map[attr_id].offset + remap.map[attr_id].new_stride * num_vertices;
        desc->stride = remap.map[attr_id].new_stride;
    }

    remap.buffer_space_required = output_offset;

    // reserve space
    if (remap.attributes) {
        StorageBuffer *buffer = &r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING];
        VkDeviceSize starting_offset = ROUND_UP(buffer->buffer_offset, 16);
        size_t total_space_required =
            (starting_offset - buffer->buffer_offset) + remap.buffer_space_required;
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING, total_space_required);
        buffer->buffer_offset = ROUND_UP(buffer->buffer_offset, 16);
    }

    return remap;
}

#ifdef __aarch64__
static inline bool copy_remapped_attribute_arm64(uint8_t *dst,
                                                 const uint8_t *src,
                                                 VkDeviceSize old_stride,
                                                 VkDeviceSize new_stride,
                                                 uint32_t num_vertices)
{
    switch (new_stride) {
    case 4:
        for (uint32_t vertex_id = 0; vertex_id < num_vertices; vertex_id++) {
            uint32_t word;

            memcpy(&word, src, sizeof(word));
            memcpy(dst, &word, sizeof(word));

            dst += new_stride;
            src += old_stride;
        }
        return true;

    case 8:
        for (uint32_t vertex_id = 0; vertex_id < num_vertices; vertex_id++) {
            vst1_u8(dst, vld1_u8(src));

            dst += new_stride;
            src += old_stride;
        }
        return true;

    case 12:
        for (uint32_t vertex_id = 0; vertex_id < num_vertices; vertex_id++) {
            uint32_t tail_word;

            vst1_u8(dst, vld1_u8(src));
            memcpy(&tail_word, src + 8, sizeof(tail_word));
            memcpy(dst + 8, &tail_word, sizeof(tail_word));

            dst += new_stride;
            src += old_stride;
        }
        return true;

    case 16:
        for (uint32_t vertex_id = 0; vertex_id < num_vertices; vertex_id++) {
            vst1q_u8(dst, vld1q_u8(src));

            dst += new_stride;
            src += old_stride;
        }
        return true;

    default:
        return false;
    }
}
#endif

static void copy_remapped_attributes_to_inline_buffer(PGRAPHState *pg,
                                                      VertexBufferRemap remap,
                                                      uint32_t start_vertex,
                                                      uint32_t num_vertices)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *buffer = &r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING];

    if (!remap.attributes) {
        return;
    }

    assert(pgraph_vk_buffer_has_space_for(pg, BUFFER_VERTEX_INLINE_STAGING,
                                          remap.buffer_space_required, 256));

    // FIXME: SIMD memcpy
    // FIXME: Caching
    // FIXME: Account for only what is drawn
    assert(start_vertex == 0);
    assert(buffer->mapped);

    // Copy vertex data
    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        if (!(remap.attributes & (1 << attr_id))) {
            continue;
        }

        VkDeviceSize attr_buffer_offset =
            buffer->buffer_offset + remap.map[attr_id].offset;

        uint8_t *out_ptr = buffer->mapped + attr_buffer_offset;
        uint8_t *in_ptr = d->vram_ptr + r->vertex_attribute_offsets[attr_id];

#ifdef __aarch64__
        if (copy_remapped_attribute_arm64(out_ptr, in_ptr,
                                          remap.map[attr_id].old_stride,
                                          remap.map[attr_id].new_stride,
                                          num_vertices)) {
            r->vertex_attribute_offsets[attr_id] = attr_buffer_offset;
            continue;
        }
#endif

        for (int vertex_id = 0; vertex_id < num_vertices; vertex_id++) {
            memcpy(out_ptr, in_ptr, remap.map[attr_id].new_stride);
            out_ptr += remap.map[attr_id].new_stride;
            in_ptr += remap.map[attr_id].old_stride;
        }

        r->vertex_attribute_offsets[attr_id] = attr_buffer_offset;
    }


    buffer->buffer_offset += remap.buffer_space_required;
}

void pgraph_vk_flush_draw(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        NV2A_VK_DPRINTF("No binding present!!!\n");
        return;
    }

    r->num_vertex_ram_buffer_syncs = 0;

    PrimAssemblyState assembly = {
        .primitive_mode = pg->primitive_mode,
        .polygon_mode = (enum ShaderPolygonMode)GET_MASK(
            pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
            NV_PGRAPH_SETUPRASTER_FRONTFACEMODE),
        .last_provoking = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                                   NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
                          NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_LAST,
        .flat_shading = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                                 NV_PGRAPH_CONTROL_3_SHADEMODE) ==
                        NV_PGRAPH_CONTROL_3_SHADEMODE_FLAT,
    };

    if (pg->draw_arrays_length) {
        NV2A_VK_DGROUP_BEGIN("Draw Arrays");
        nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);

        assert(pg->inline_elements_length == 0);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        pgraph_vk_bind_vertex_attributes(d, pg->draw_arrays_min_start,
                                         pg->draw_arrays_max_count - 1, false,
                                         0, pg->draw_arrays_max_count - 1);
        uint32_t min_element = INT_MAX;
        uint32_t max_element = 0;
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            min_element = MIN(pg->draw_arrays_start[i], min_element);
            max_element = MAX(max_element, pg->draw_arrays_start[i] + pg->draw_arrays_count[i]);
        }
        sync_vertex_ram_buffer(pg);
        VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element);

        PrimRewrite prim_rw = pgraph_prim_rewrite_ranges(
            &r->prim_rewrite_buf, assembly,
            pg->draw_arrays_start, pg->draw_arrays_count,
            pg->draw_arrays_length);

        if (prim_rw.num_indices > 0) {
            size_t rewrite_size =
                prim_rw.num_indices * sizeof(uint32_t);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING, rewrite_size);
        }

        begin_pre_draw(pg);
        if (r->async_draw_skip) {
            NV2A_VK_DGROUP_END();
            return;
        }
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Draw Arrays");
        begin_draw(pg);
        bind_vertex_buffer(pg, remap.attributes, 0);

        if (prim_rw.num_indices > 0) {
            size_t rewrite_size = prim_rw.num_indices * sizeof(uint32_t);
            VkDeviceSize buffer_offset = pgraph_vk_update_index_buffer(
                pg, prim_rw.indices, rewrite_size);
            vkCmdBindIndexBuffer(r->command_buffer,
                                 r->storage_buffers[BUFFER_INDEX].buffer,
                                 buffer_offset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(r->command_buffer, prim_rw.num_indices, 1, 0, 0,
                             0);
        } else {
            for (int i = 0; i < pg->draw_arrays_length; i++) {
                uint32_t start = pg->draw_arrays_start[i],
                         count = pg->draw_arrays_count[i];
                NV2A_VK_DPRINTF("- [%d] Start:%d Count:%d", i, start, count);
                vkCmdDraw(r->command_buffer, count, 1, start, 0);
            }
        }

        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_elements_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Elements");
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);

        uint32_t *draw_indices = pg->inline_elements;
        unsigned int draw_index_count = pg->inline_elements_length;
        PrimRewrite prim_rw = pgraph_prim_rewrite_indexed(
            &r->prim_rewrite_buf, assembly, pg->inline_elements,
            pg->inline_elements_length);
        if (prim_rw.num_indices > 0) {
            draw_indices = prim_rw.indices;
            draw_index_count = prim_rw.num_indices;
        }

        size_t index_data_size = draw_index_count * sizeof(uint32_t);
        ensure_buffer_space(pg, BUFFER_INDEX_STAGING, index_data_size);

        uint32_t min_element = (uint32_t)-1;
        uint32_t max_element = 0;
        for (unsigned int i = 0; i < draw_index_count; i++) {
            max_element = MAX(draw_indices[i], max_element);
            min_element = MIN(draw_indices[i], min_element);
        }
        pgraph_vk_bind_vertex_attributes(
            d, min_element, max_element, false, 0,
            draw_indices[draw_index_count - 1]);
        sync_vertex_ram_buffer(pg);
        VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element + 1);

        begin_pre_draw(pg);
        if (r->async_draw_skip) {
            NV2A_VK_DGROUP_END();
            return;
        }
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element + 1);
        VkDeviceSize buffer_offset = pgraph_vk_update_index_buffer(
            pg, draw_indices, index_data_size);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Elements");
        begin_draw(pg);
        bind_vertex_buffer(pg, remap.attributes, 0);
        vkCmdBindIndexBuffer(r->command_buffer,
                             r->storage_buffers[BUFFER_INDEX].buffer,
                             buffer_offset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(r->command_buffer, draw_index_count, 1, 0, 0, 0);
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_buffer_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Buffer");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_BUFFERS);
        assert(pg->inline_array_length == 0);

        size_t vertex_data_size = pg->inline_buffer_length * sizeof(float) * 4;
        void *data[NV2A_VERTEXSHADER_ATTRIBUTES];
        size_t sizes[NV2A_VERTEXSHADER_ATTRIBUTES];
        size_t offset = 0;

        pgraph_vk_bind_vertex_attributes_inline(d);
        for (int i = 0; i < r->num_active_vertex_attribute_descriptions; i++) {
            int attr_index = r->vertex_attribute_descriptions[i].location;

            VertexAttribute *attr = &pg->vertex_attributes[attr_index];
            r->vertex_attribute_offsets[attr_index] = offset;

            data[i] = attr->inline_buffer;
            sizes[i] = vertex_data_size;

            attr->inline_buffer_populated = false;
            offset += vertex_data_size;
        }
        PrimRewrite prim_rw = pgraph_prim_rewrite_sequential(
            &r->prim_rewrite_buf, assembly, 0, pg->inline_buffer_length);

        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING, offset);
        if (prim_rw.num_indices > 0) {
            size_t rewrite_size = prim_rw.num_indices * sizeof(uint32_t);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING, rewrite_size);
        }

        begin_pre_draw(pg);
        if (r->async_draw_skip) {
            NV2A_VK_DGROUP_END();
            return;
        }
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, data, sizes, r->num_active_vertex_attribute_descriptions);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Buffer");
        begin_draw(pg);
        bind_inline_vertex_buffer(pg, buffer_offset);

        if (prim_rw.num_indices > 0) {
            size_t rewrite_size = prim_rw.num_indices * sizeof(uint32_t);
            VkDeviceSize idx_offset = pgraph_vk_update_index_buffer(
                pg, prim_rw.indices, rewrite_size);
            vkCmdBindIndexBuffer(r->command_buffer,
                                 r->storage_buffers[BUFFER_INDEX].buffer,
                                 idx_offset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(r->command_buffer, prim_rw.num_indices, 1, 0, 0,
                             0);
        } else {
            vkCmdDraw(r->command_buffer, pg->inline_buffer_length, 1, 0, 0);
        }

        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_array_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Array");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ARRAYS);

        VkDeviceSize inline_array_data_size = pg->inline_array_length * 4;
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING,
                               inline_array_data_size);

        unsigned int offset = 0;
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->count == 0) {
                continue;
            }

            /* FIXME: Double check */
            offset = ROUND_UP(offset, attr->size);
            attr->inline_array_offset = offset;
            NV2A_DPRINTF("bind inline attribute %d size=%d, count=%d\n", i,
                         attr->size, attr->count);
            offset += attr->size * attr->count;
            offset = ROUND_UP(offset, attr->size);
        }

        unsigned int vertex_size = offset;
        unsigned int index_count = pg->inline_array_length * 4 / vertex_size;

        NV2A_DPRINTF("draw inline array %d, %d\n", vertex_size, index_count);
        pgraph_vk_bind_vertex_attributes(d, 0, index_count - 1, true,
                                         vertex_size, index_count - 1);

        PrimRewrite prim_rw = pgraph_prim_rewrite_sequential(
            &r->prim_rewrite_buf, assembly, 0, index_count);

        if (prim_rw.num_indices > 0) {
            size_t rewrite_size = prim_rw.num_indices * sizeof(uint32_t);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING, rewrite_size);
        }

        begin_pre_draw(pg);
        if (r->async_draw_skip) {
            NV2A_VK_DGROUP_END();
            return;
        }
        void *inline_array_data = pg->inline_array;
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, &inline_array_data, &inline_array_data_size, 1);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Array");
        begin_draw(pg);
        bind_inline_vertex_buffer(pg, buffer_offset);

        if (prim_rw.num_indices > 0) {
            size_t rewrite_size = prim_rw.num_indices * sizeof(uint32_t);
            VkDeviceSize idx_offset = pgraph_vk_update_index_buffer(
                pg, prim_rw.indices, rewrite_size);
            vkCmdBindIndexBuffer(r->command_buffer,
                                 r->storage_buffers[BUFFER_INDEX].buffer,
                                 idx_offset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(r->command_buffer, prim_rw.num_indices, 1, 0, 0,
                             0);
        } else {
            vkCmdDraw(r->command_buffer, index_count, 1, 0, 0);
        }

        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);
        NV2A_VK_DGROUP_END();
    } else {
        NV2A_VK_DPRINTF("EMPTY NV097_SET_BEGIN_END");
        NV2A_UNCONFIRMED("EMPTY NV097_SET_BEGIN_END");
    }
}
