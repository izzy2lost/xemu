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

#include "ui/xemu-settings.h"
#include "renderer.h"
#include "qemu/error-report.h"
#include <EGL/egl.h>
#include <math.h>
#ifdef __ANDROID__
#include <android/log.h>
#define DBG_LOG(...) __android_log_print(ANDROID_LOG_INFO, "hakuX-vk-dbg", __VA_ARGS__)
#else
#define DBG_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

extern bool xemu_get_frame_skip(void);

#ifdef __ANDROID__
#include "hw/xbox/nv2a/nv2a.h"

enum DisplayCaptureState {
    DISPLAY_CAPTURE_IDLE,
    DISPLAY_CAPTURE_REQUESTED,
    DISPLAY_CAPTURE_READY,
};

static void destroy_display_capture_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (d->capture.mapped) {
        vmaUnmapMemory(r->allocator, d->capture.allocation);
        d->capture.mapped = NULL;
    }
    if (d->capture.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(r->allocator, d->capture.buffer,
                         d->capture.allocation);
        d->capture.buffer = VK_NULL_HANDLE;
        d->capture.allocation = VK_NULL_HANDLE;
    }
    d->capture.size = 0;
    d->capture.width = 0;
    d->capture.height = 0;
    qatomic_set(&d->capture.state, DISPLAY_CAPTURE_IDLE);
}

static bool ensure_display_capture_buffer(PGRAPHState *pg, size_t size)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (d->capture.buffer != VK_NULL_HANDLE && d->capture.size >= size) {
        return true;
    }

    destroy_display_capture_buffer(pg);

    VkBufferCreateInfo buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
    };
    if (vmaCreateBuffer(r->allocator, &buffer_create_info, &alloc_create_info,
                        &d->capture.buffer, &d->capture.allocation,
                        NULL) != VK_SUCCESS) {
        d->capture.buffer = VK_NULL_HANDLE;
        d->capture.allocation = VK_NULL_HANDLE;
        return false;
    }
    if (vmaMapMemory(r->allocator, d->capture.allocation,
                     &d->capture.mapped) != VK_SUCCESS) {
        d->capture.mapped = NULL;
        destroy_display_capture_buffer(pg);
        return false;
    }

    d->capture.size = size;
    return true;
}

bool nv2a_android_display_capture_supported(void)
{
    NV2AState *d = g_nv2a;
    if (!d || !d->pgraph.renderer ||
        d->pgraph.renderer->type != CONFIG_DISPLAY_RENDERER_VULKAN) {
        return false;
    }
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    if (!r) {
        return false;
    }
    return r->display.direct_present && r->display.capture.supported;
}

void nv2a_android_request_display_capture(void)
{
    if (!nv2a_android_display_capture_supported()) {
        return;
    }
    PGRAPHVkDisplayState *disp = &g_nv2a->pgraph.vk_renderer_state->display;
    qatomic_cmpxchg(&disp->capture.state, DISPLAY_CAPTURE_IDLE,
                    DISPLAY_CAPTURE_REQUESTED);
}

bool nv2a_android_display_capture_ready(void)
{
    if (!nv2a_android_display_capture_supported()) {
        return false;
    }
    PGRAPHVkDisplayState *disp = &g_nv2a->pgraph.vk_renderer_state->display;
    return qatomic_read(&disp->capture.state) == DISPLAY_CAPTURE_READY;
}

bool nv2a_android_take_display_capture(uint8_t **rgba, int *width, int *height)
{
    if (!nv2a_android_display_capture_ready()) {
        return false;
    }

    PGRAPHVkDisplayState *disp = &g_nv2a->pgraph.vk_renderer_state->display;
    size_t size = (size_t)disp->capture.width * disp->capture.height * 4;

    if (!disp->capture.mapped || size == 0 || size > disp->capture.size) {
        qatomic_set(&disp->capture.state, DISPLAY_CAPTURE_IDLE);
        return false;
    }

    *rgba = g_malloc(size);
    memcpy(*rgba, disp->capture.mapped, size);
    *width = disp->capture.width;
    *height = disp->capture.height;

    /* Consume the frame so the next request captures a fresh one. */
    qatomic_set(&disp->capture.state, DISPLAY_CAPTURE_IDLE);
    return true;
}
#endif

#if HAVE_EXTERNAL_MEMORY
#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <vulkan/vulkan_android.h>

static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC p_eglGetNativeClientBufferANDROID;
static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;
static PFN_vkGetAndroidHardwareBufferPropertiesANDROID p_vkGetAndroidHardwareBufferPropertiesANDROID;

static bool ahb_interop_loaded;
static bool ahb_interop_available;

static bool load_ahb_interop_symbols(VkDevice device)
{
    if (ahb_interop_loaded) {
        return ahb_interop_available;
    }
    ahb_interop_loaded = true;

    p_eglGetNativeClientBufferANDROID =
        (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress(
            "eglGetNativeClientBufferANDROID");
    p_eglCreateImageKHR =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    p_eglDestroyImageKHR =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    p_glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
            "glEGLImageTargetTexture2DOES");
    p_vkGetAndroidHardwareBufferPropertiesANDROID =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(
            device, "vkGetAndroidHardwareBufferPropertiesANDROID");

    ahb_interop_available = p_eglGetNativeClientBufferANDROID &&
                            p_eglCreateImageKHR &&
                            p_eglDestroyImageKHR &&
                            p_glEGLImageTargetTexture2DOES &&
                            p_vkGetAndroidHardwareBufferPropertiesANDROID;

    __android_log_print(ANDROID_LOG_INFO, "hakuX",
                        "AHB interop: %s", ahb_interop_available ? "available" : "NOT available");
    return ahb_interop_available;
}

bool pgraph_vk_gl_external_memory_available(void)
{
    return true;
}

#else /* !__ANDROID__ */

static PFNGLDELETEMEMORYOBJECTSEXTPROC p_glDeleteMemoryObjectsEXT;
static PFNGLISMEMORYOBJECTEXTPROC p_glIsMemoryObjectEXT;
static PFNGLCREATEMEMORYOBJECTSEXTPROC p_glCreateMemoryObjectsEXT;
static PFNGLIMPORTMEMORYFDEXTPROC p_glImportMemoryFdEXT;
static PFNGLTEXSTORAGEMEM2DEXTPROC p_glTexStorageMem2DEXT;

static bool gl_external_memory_loaded;
static bool gl_external_memory_available;

static bool load_gl_external_memory_symbols(void)
{
    if (gl_external_memory_loaded) {
        return gl_external_memory_available;
    }
    gl_external_memory_loaded = true;

    p_glDeleteMemoryObjectsEXT =
        (PFNGLDELETEMEMORYOBJECTSEXTPROC)eglGetProcAddress(
            "glDeleteMemoryObjectsEXT");
    p_glIsMemoryObjectEXT =
        (PFNGLISMEMORYOBJECTEXTPROC)eglGetProcAddress(
            "glIsMemoryObjectEXT");
    p_glCreateMemoryObjectsEXT =
        (PFNGLCREATEMEMORYOBJECTSEXTPROC)eglGetProcAddress(
            "glCreateMemoryObjectsEXT");
    p_glImportMemoryFdEXT =
        (PFNGLIMPORTMEMORYFDEXTPROC)eglGetProcAddress(
            "glImportMemoryFdEXT");
    p_glTexStorageMem2DEXT =
        (PFNGLTEXSTORAGEMEM2DEXTPROC)eglGetProcAddress(
            "glTexStorageMem2DEXT");

    gl_external_memory_available = p_glDeleteMemoryObjectsEXT &&
                                   p_glIsMemoryObjectEXT &&
                                   p_glCreateMemoryObjectsEXT &&
                                   p_glImportMemoryFdEXT &&
                                   p_glTexStorageMem2DEXT;
    return gl_external_memory_available;
}

bool pgraph_vk_gl_external_memory_available(void)
{
    return load_gl_external_memory_symbols();
}
#endif /* __ANDROID__ */
#endif /* HAVE_EXTERNAL_MEMORY */

static uint8_t *convert_texture_data__CR8YB8CB8YA8(uint8_t *data_out,
                                                   const uint8_t *data_in,
                                                   unsigned int width,
                                                   unsigned int height,
                                                   unsigned int input_pitch,
                                                   size_t output_pitch)
{
    int x, y;
    for (y = 0; y < height; y++) {
        const uint8_t *line = &data_in[y * input_pitch];
        uint8_t *output_line = &data_out[y * output_pitch];
        for (x = 0; x < width; x++) {
            uint8_t *pixel = &output_line[x * 4];
            convert_yuy2_to_rgb(line, x, &pixel[0], &pixel[1], &pixel[2]);
            pixel[3] = 255;
        }
    }
    return data_out;
}

static float pvideo_calculate_scale(unsigned int din_dout,
                                    unsigned int output_size)
{
    float calculated_in = din_dout * (output_size - 1);
    calculated_in = floorf(calculated_in / (1 << 20) + 0.5f);
    return (calculated_in + 1.0f) / output_size;
}

static void destroy_pvideo_image(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (d->pvideo.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(r->device, d->pvideo.sampler, NULL);
        d->pvideo.sampler = VK_NULL_HANDLE;
    }

    for (int i = 0; i < NUM_DISPLAY_IMAGES; i++) {
        if (d->pvideo.staging_mapped[i] != NULL) {
            vmaUnmapMemory(r->allocator,
                           d->pvideo.staging_allocations[i]);
            d->pvideo.staging_mapped[i] = NULL;
        }
        if (d->pvideo.staging_buffers[i] != VK_NULL_HANDLE) {
            vmaDestroyBuffer(r->allocator, d->pvideo.staging_buffers[i],
                             d->pvideo.staging_allocations[i]);
            d->pvideo.staging_buffers[i] = VK_NULL_HANDLE;
            d->pvideo.staging_allocations[i] = VK_NULL_HANDLE;
        }
        if (d->pvideo.image_views[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(r->device, d->pvideo.image_views[i], NULL);
            d->pvideo.image_views[i] = VK_NULL_HANDLE;
        }
        if (d->pvideo.images[i] != VK_NULL_HANDLE) {
            vmaDestroyImage(r->allocator, d->pvideo.images[i],
                            d->pvideo.allocations[i]);
            d->pvideo.images[i] = VK_NULL_HANDLE;
            d->pvideo.allocations[i] = VK_NULL_HANDLE;
        }
        d->pvideo.image_valid[i] = false;
        d->pvideo.upload_pending[i] = false;
    }

    d->pvideo.staging_size = 0;
    d->pvideo.width = 0;
    d->pvideo.height = 0;
}

static void create_pvideo_image(PGRAPHState *pg, int width, int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (d->pvideo.images[0] != VK_NULL_HANDLE && d->pvideo.width == width &&
        d->pvideo.height == height) {
        return;
    }

    if (d->pvideo.images[0] != VK_NULL_HANDLE) {
        /*
         * Display command buffers can still sample the old PVIDEO image.
         * Resolution changes are rare, so wait before replacing its resources.
         */
        VK_CHECK(vkQueueWaitIdle(r->queue));
        destroy_pvideo_image(pg);
    }

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = 0,
    };
    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
    };
    for (int i = 0; i < NUM_DISPLAY_IMAGES; i++) {
        VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                                &alloc_create_info, &d->pvideo.images[i],
                                &d->pvideo.allocations[i], NULL));
        image_view_create_info.image = d->pvideo.images[i];
        VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                                   &d->pvideo.image_views[i]));
        d->pvideo.image_valid[i] = false;
    }

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    };
    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &d->pvideo.sampler));

    /* PVIDEO is refreshed independently of guest draws. Keep one upload
     * buffer per display-image slot so the copy can be recorded in the same
     * command buffer that samples it without racing another frame. */
    d->pvideo.staging_size = (size_t)width * height * 4;
    VkBufferCreateInfo buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = d->pvideo.staging_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo staging_alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
    };
    for (int i = 0; i < NUM_DISPLAY_IMAGES; i++) {
        VK_CHECK(vmaCreateBuffer(r->allocator, &buffer_create_info,
                                 &staging_alloc_create_info,
                                 &d->pvideo.staging_buffers[i],
                                 &d->pvideo.staging_allocations[i], NULL));
        VK_CHECK(vmaMapMemory(r->allocator,
                              d->pvideo.staging_allocations[i],
                              &d->pvideo.staging_mapped[i]));
    }

    d->pvideo.width = width;
    d->pvideo.height = height;
}

static void upload_pvideo_image(PGRAPHState *pg, PvideoState state)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;

    create_pvideo_image(pg, state.in_width, state.in_height);
    assert(disp->render_idx >= 0 && disp->render_idx < NUM_DISPLAY_IMAGES);
    int image_index = disp->render_idx;

    // FIXME: Dirty tracking. We don't necessarily need to upload so much.

    size_t display_data_size = state.in_width * state.in_height * 4;
    assert(display_data_size <= disp->pvideo.staging_size);
    uint8_t *mapped_memory_ptr = disp->pvideo.staging_mapped[image_index];

    convert_texture_data__CR8YB8CB8YA8(
        mapped_memory_ptr, d->vram_ptr + state.base + state.offset,
        state.in_width, state.in_height, state.pitch,
        (size_t)state.in_width * 4);

    vmaFlushAllocation(r->allocator,
                       disp->pvideo.staging_allocations[image_index],
                       0, display_data_size);
    disp->pvideo.upload_pending[image_index] = true;
}

static const char *display_frag_glsl =
    "#version 450\n"
    "layout(binding = 0) uniform sampler2D tex;\n"
    "layout(binding = 1) uniform sampler2D pvideo_tex;\n"
    "layout(binding = 2) uniform sampler2D prev_tex;\n"
    "layout(push_constant, std430) uniform PushConstants {\n"
    "    float line_offset;\n"
    "    vec2 display_size;\n"
    "    vec2 output_offset;\n"
    "    vec2 output_size;\n"
    "    bool flip_y;\n"
    "    bool pvideo_enable;\n"
    "    vec2 pvideo_in_pos;\n"
    "    vec4 pvideo_pos;\n"
    "    vec4 pvideo_scale;\n"
    "    bool pvideo_color_key_enable;\n"
    "    vec3 pvideo_color_key;\n"
    "    float blend_factor;\n"
    "};\n"
    "layout(location = 0) out vec4 out_Color;\n"
    "void main()\n"
    "{\n"
    "    vec2 display_coord = (gl_FragCoord.xy - output_offset) / output_size * display_size;\n"
    "    vec2 tex_coord = display_coord/display_size;\n"
    "    float rel = display_size.y/textureSize(tex, 0).y/line_offset;\n"
    "    tex_coord.y = 1 + rel*(tex_coord.y - 1);\n"
    "    if (flip_y) {\n"
    "        tex_coord.y = 1 - tex_coord.y;\n"
    "    }\n"
    "    out_Color.rgba = texture(tex, tex_coord);\n"
    "    if (pvideo_enable) {\n"
    "        float screen_y = flip_y ? display_size.y - display_coord.y : display_coord.y;\n"
    "        vec2 screen_coord = vec2(display_coord.x, screen_y) * pvideo_scale.z;\n"
    "        vec4 output_region = vec4(pvideo_pos.xy, pvideo_pos.xy + pvideo_pos.zw);\n"
    "        bvec4 clip = bvec4(lessThan(screen_coord, output_region.xy),\n"
    "                           greaterThan(screen_coord, output_region.zw));\n"
    "        if (!any(clip) && (!pvideo_color_key_enable || out_Color.rgb == pvideo_color_key)) {\n"
    "            vec2 out_xy = screen_coord - pvideo_pos.xy;\n"
    "            vec2 in_st = (pvideo_in_pos + out_xy * pvideo_scale.xy) / textureSize(pvideo_tex, 0);\n"
    "            out_Color.rgba = texture(pvideo_tex, in_st);\n"
    "        }\n"
    "    }\n"
    "    if (blend_factor > 0.0) {\n"
    "        vec2 prev_coord = display_coord / display_size;\n"
    "        vec4 prev = texture(prev_tex, prev_coord);\n"
    "        out_Color.rgba = mix(out_Color.rgba, prev, blend_factor);\n"
    "    }\n"
    "}\n";

static void create_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorPoolSize pool_sizes = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 3 * NUM_DISPLAY_IMAGES,
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_sizes,
        .maxSets = NUM_DISPLAY_IMAGES,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                    &r->display.descriptor_pool));
}

static void destroy_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorPool(r->device, r->display.descriptor_pool, NULL);
    r->display.descriptor_pool = VK_NULL_HANDLE;
}

static void create_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayoutBinding bindings[3];

    for (int i = 0; i < ARRAY_SIZE(bindings); i++) {
        bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_SIZE(bindings),
        .pBindings = bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &layout_info, NULL,
                                         &r->display.descriptor_set_layout));
}

static void destroy_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorSetLayout(r->device, r->display.descriptor_set_layout,
                                 NULL);
    r->display.descriptor_set_layout = VK_NULL_HANDLE;
}

static void create_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayout layouts[NUM_DISPLAY_IMAGES];
    for (int i = 0; i < NUM_DISPLAY_IMAGES; i++) {
        layouts[i] = r->display.descriptor_set_layout;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->display.descriptor_pool,
        .descriptorSetCount = NUM_DISPLAY_IMAGES,
        .pSetLayouts = layouts,
    };
    VK_CHECK(vkAllocateDescriptorSets(r->device, &alloc_info,
                                      r->display.descriptor_sets));
}

static void create_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkAttachmentDescription attachment;

    VkAttachmentReference color_reference;
    attachment = (VkAttachmentDescription){
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    color_reference = (VkAttachmentReference){
        0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
    };

    dependency.srcStageMask |=
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask |=
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_reference,
    };

    VkRenderPassCreateInfo renderpass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    VK_CHECK(vkCreateRenderPass(r->device, &renderpass_create_info, NULL,
                                &r->display.render_pass));
}

static void destroy_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    vkDestroyRenderPass(r->device, r->display.render_pass, NULL);
    r->display.render_pass = VK_NULL_HANDLE;
}

static void create_display_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->display.display_frag =
        pgraph_vk_create_shader_module_from_glsl(
            r, VK_SHADER_STAGE_FRAGMENT_BIT, display_frag_glsl);

    VkPipelineShaderStageCreateInfo shader_stages[] = {
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->quad_vert_module->module,
            .pName = "main",
        },
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = r->display.display_frag->module,
            .pName = "main",
        },
     };

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
        .depthTestEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
    };

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_FALSE,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };

    VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = r->display.display_frag->push_constants.total_size,
    };

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &r->display.descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant_range,
    };
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &r->display.pipeline_layout));

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = ARRAY_SIZE(shader_stages),
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = r->display.pipeline_layout,
        .renderPass = r->display.render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_info, NULL,
                                       &r->display.pipeline));
}

static void destroy_display_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyPipeline(r->device, r->display.pipeline, NULL);
    r->display.pipeline = VK_NULL_HANDLE;

    vkDestroyPipelineLayout(r->device, r->display.pipeline_layout, NULL);
    r->display.pipeline_layout = VK_NULL_HANDLE;

    pgraph_vk_destroy_shader_module(r, r->display.display_frag);
    r->display.display_frag = NULL;
}

static void create_frame_buffer(PGRAPHState *pg, DisplayImage *img)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkFramebufferCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = r->display.render_pass,
        .attachmentCount = 1,
        .pAttachments = &img->image_view,
        .width = r->display.width,
        .height = r->display.height,
        .layers = 1,
    };
    VK_CHECK(vkCreateFramebuffer(r->device, &create_info, NULL,
                                 &img->framebuffer));
}

static void destroy_frame_buffer(PGRAPHState *pg, DisplayImage *img)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    vkDestroyFramebuffer(r->device, img->framebuffer, NULL);
    img->framebuffer = VK_NULL_HANDLE;
}

static void destroy_single_display_image(PGRAPHState *pg, DisplayImage *img)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (img->image == VK_NULL_HANDLE) {
        return;
    }

    destroy_frame_buffer(pg, img);

    if (img->fence != VK_NULL_HANDLE) {
        if (img->fence_submitted) {
            vkWaitForFences(r->device, 1, &img->fence, VK_TRUE, UINT64_MAX);
        }
        vkDestroyFence(r->device, img->fence, NULL);
        img->fence = VK_NULL_HANDLE;
    }
    img->fence_submitted = false;
    img->valid = false;

    if (img->cmd_buffer != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(r->device, r->command_pool, 1, &img->cmd_buffer);
        img->cmd_buffer = VK_NULL_HANDLE;
    }

#if HAVE_EXTERNAL_MEMORY
    if (img->gl_texture_id) {
        glDeleteTextures(1, &img->gl_texture_id);
    }
    img->gl_texture_id = 0;

#ifdef __ANDROID__
    if (img->egl_image != EGL_NO_IMAGE_KHR && p_eglDestroyImageKHR) {
        p_eglDestroyImageKHR(eglGetCurrentDisplay(), img->egl_image);
    }
    img->egl_image = EGL_NO_IMAGE_KHR;

    if (img->ahb) {
        AHardwareBuffer_release(img->ahb);
        img->ahb = NULL;
    }
#else
    if (img->gl_memory_obj && p_glDeleteMemoryObjectsEXT) {
        p_glDeleteMemoryObjectsEXT(1, &img->gl_memory_obj);
    }
    img->gl_memory_obj = 0;
#ifdef WIN32
    CloseHandle(img->handle);
    img->handle = 0;
#endif
#endif
#endif

    vkDestroyImageView(r->device, img->image_view, NULL);
    img->image_view = VK_NULL_HANDLE;

    vkDestroyImage(r->device, img->image, NULL);
    img->image = VK_NULL_HANDLE;

    vkFreeMemory(r->device, img->memory, NULL);
    img->memory = VK_NULL_HANDLE;
}

static void destroy_current_display_image(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

#ifdef __ANDROID__
    if (d->direct_present && d->swapchain != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(r->device);
        /* Safe to release now that nothing can still be copying into it. */
        destroy_display_capture_buffer(pg);
        for (uint32_t i = 0; i < d->image_count; i++) {
            DisplayImage *img = &d->images[i];
            if (img->framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(r->device, img->framebuffer, NULL);
            }
            if (img->cmd_buffer != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(r->device, r->command_pool, 1,
                                     &img->cmd_buffer);
            }
            if (img->image_view != VK_NULL_HANDLE) {
                vkDestroyImageView(r->device, img->image_view, NULL);
            }
            memset(img, 0, sizeof(*img));
        }
        for (uint32_t i = 0; i < NUM_DISPLAY_IMAGES; i++) {
            if (d->image_available[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(r->device, d->image_available[i], NULL);
                d->image_available[i] = VK_NULL_HANDLE;
            }
            if (d->render_finished[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(r->device, d->render_finished[i], NULL);
                d->render_finished[i] = VK_NULL_HANDLE;
            }
            if (d->present_fences[i] != VK_NULL_HANDLE) {
                vkDestroyFence(r->device, d->present_fences[i], NULL);
                d->present_fences[i] = VK_NULL_HANDLE;
            }
            d->image_in_flight[i] = VK_NULL_HANDLE;
        }
        vkDestroySwapchainKHR(r->device, d->swapchain, NULL);
        d->swapchain = VK_NULL_HANDLE;
        d->image_count = 0;
        d->present_frame = 0;
    } else
#endif
    {
#if HAVE_EXTERNAL_MEMORY
        if (d->use_external_memory) {
            pgraph_vk_gl_make_context_current();
        }
#endif

        for (int i = 0; i < NUM_DISPLAY_IMAGES; i++) {
            destroy_single_display_image(pg, &d->images[i]);
        }
    }

    if (d->blend_prev_view) {
        vkDestroyImageView(r->device, d->blend_prev_view, NULL);
        d->blend_prev_view = VK_NULL_HANDLE;
    }
    if (d->blend_prev_image) {
        vmaDestroyImage(r->allocator, d->blend_prev_image,
                        d->blend_prev_alloc);
        d->blend_prev_image = VK_NULL_HANDLE;
        d->blend_prev_alloc = VK_NULL_HANDLE;
    }
    d->blend_prev_valid = false;

    d->render_idx = 0;
    d->display_idx = 0;
    d->image_count = 0;
    d->draw_time = 0;
}

static bool create_single_display_image_resources(PGRAPHState *pg,
                                                   DisplayImage *img,
                                                   int width, int height,
                                                   bool use_optimal_tiling,
                                                   bool use_external_memory)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    memset(img, 0, sizeof(*img));
#if HAVE_EXTERNAL_MEMORY
#ifdef __ANDROID__
    img->egl_image = EGL_NO_IMAGE_KHR;
#elif !defined(WIN32)
    img->fd = -1;
#endif
#endif

#if HAVE_EXTERNAL_MEMORY && defined(__ANDROID__)
    if (use_external_memory) {
        AHardwareBuffer_Desc ahb_desc = {
            .width = width,
            .height = height,
            .layers = 1,
            .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
        };
        int ret = AHardwareBuffer_allocate(&ahb_desc, &img->ahb);
        if (ret != 0) {
            __android_log_print(ANDROID_LOG_ERROR, "hakuX",
                                "display: AHardwareBuffer_allocate failed (%d)", ret);
            return false;
        }

        VkAndroidHardwareBufferPropertiesANDROID ahb_props = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
        };
        VkResult result = p_vkGetAndroidHardwareBufferPropertiesANDROID(
            r->device, img->ahb, &ahb_props);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "hakuX",
                                "display: vkGetAndroidHardwareBufferProperties failed (%d)", result);
            destroy_single_display_image(pg, img);
            return false;
        }

        VkExternalMemoryImageCreateInfo ext_mem_info = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
        };
        VkImageCreateInfo image_create_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &ext_mem_info,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { width, height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        result = vkCreateImage(r->device, &image_create_info, NULL, &img->image);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "hakuX",
                                "display: vkCreateImage (AHB) failed (%d)", result);
            destroy_single_display_image(pg, img);
            return false;
        }

        uint32_t memory_type_index = pgraph_vk_get_memory_type(
            pg, ahb_props.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memory_type_index == 0xFFFFFFFF) {
            memory_type_index = pgraph_vk_get_memory_type(
                pg, ahb_props.memoryTypeBits, 0);
        }
        if (memory_type_index == 0xFFFFFFFF) {
            __android_log_print(ANDROID_LOG_ERROR, "hakuX",
                                "display: no compatible memory type for AHB");
            destroy_single_display_image(pg, img);
            return false;
        }

        VkImportAndroidHardwareBufferInfoANDROID import_ahb = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
            .buffer = img->ahb,
        };
        VkMemoryDedicatedAllocateInfo dedicated_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = &import_ahb,
            .image = img->image,
        };
        VkMemoryAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &dedicated_info,
            .allocationSize = ahb_props.allocationSize,
            .memoryTypeIndex = memory_type_index,
        };
        result = vkAllocateMemory(r->device, &alloc_info, NULL, &img->memory);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "hakuX",
                                "display: vkAllocateMemory (AHB import) failed (%d)", result);
            destroy_single_display_image(pg, img);
            return false;
        }
        result = vkBindImageMemory(r->device, img->image, img->memory, 0);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "hakuX",
                                "display: vkBindImageMemory (AHB) failed (%d)", result);
            destroy_single_display_image(pg, img);
            return false;
        }

        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = img->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .subresourceRange.levelCount = 1,
            .subresourceRange.layerCount = 1,
        };
        result = vkCreateImageView(r->device, &view_info, NULL, &img->image_view);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "hakuX",
                                "display: vkCreateImageView (AHB) failed (%d)", result);
            destroy_single_display_image(pg, img);
            return false;
        }

        VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VK_CHECK(vkCreateFence(r->device, &fence_info, NULL, &img->fence));

        VkCommandBufferAllocateInfo cmd_alloc = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = r->command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VK_CHECK(vkAllocateCommandBuffers(r->device, &cmd_alloc, &img->cmd_buffer));

        pgraph_vk_gl_make_context_current();

        EGLClientBuffer client_buf = p_eglGetNativeClientBufferANDROID(img->ahb);
        if (!client_buf) {
            __android_log_print(ANDROID_LOG_ERROR, "hakuX",
                                "display: eglGetNativeClientBufferANDROID failed");
            destroy_single_display_image(pg, img);
            return false;
        }

        EGLint img_attrs[] = { EGL_NONE };
        img->egl_image = (void *)p_eglCreateImageKHR(
            eglGetCurrentDisplay(), EGL_NO_CONTEXT,
            EGL_NATIVE_BUFFER_ANDROID, client_buf, img_attrs);
        if (img->egl_image == EGL_NO_IMAGE_KHR) {
            __android_log_print(ANDROID_LOG_ERROR, "hakuX",
                                "display: eglCreateImageKHR failed (0x%x)", eglGetError());
            destroy_single_display_image(pg, img);
            return false;
        }

        glGenTextures(1, &img->gl_texture_id);
        glBindTexture(GL_TEXTURE_2D, img->gl_texture_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img->egl_image);
        GLenum gl_err = glGetError();
        if (gl_err != GL_NO_ERROR) {
            __android_log_print(ANDROID_LOG_ERROR, "hakuX",
                                "display: glEGLImageTargetTexture2DOES failed (0x%x)", gl_err);
            destroy_single_display_image(pg, img);
            return false;
        }

        __android_log_print(ANDROID_LOG_INFO, "hakuX",
                            "display: AHB image created %dx%d tex=%u",
                            width, height, img->gl_texture_id);
        return true;
    }
#endif /* HAVE_EXTERNAL_MEMORY && __ANDROID__ */

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .tiling = use_optimal_tiling ? VK_IMAGE_TILING_OPTIMAL : VK_IMAGE_TILING_LINEAR,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

#if HAVE_EXTERNAL_MEMORY
    VkExternalMemoryImageCreateInfo external_memory_image_create_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
#ifdef WIN32
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
#else
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
#endif
    };
    if (use_external_memory) {
        image_create_info.pNext = &external_memory_image_create_info;
    }
#endif

    VkResult result = vkCreateImage(r->device, &image_create_info, NULL, &img->image);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "create_display_image: vkCreateImage failed (%d)\n", result);
        return false;
    }

    VkMemoryDedicatedRequirements dedicated_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 memory_requirements2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicated_requirements,
    };
    VkImageMemoryRequirementsInfo2 image_memory_requirements_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = img->image,
    };
    vkGetImageMemoryRequirements2(r->device, &image_memory_requirements_info,
                                  &memory_requirements2);
    VkMemoryRequirements memory_requirements = memory_requirements2.memoryRequirements;

    uint32_t memory_type_index =
        pgraph_vk_get_memory_type(pg, memory_requirements.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type_index == 0xFFFFFFFF) {
        memory_type_index =
            pgraph_vk_get_memory_type(pg, memory_requirements.memoryTypeBits, 0);
    }
    if (memory_type_index == 0xFFFFFFFF) {
        fprintf(stderr, "create_display_image: no compatible memory type\n");
        vkDestroyImage(r->device, img->image, NULL);
        img->image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory_requirements.size,
        .memoryTypeIndex = memory_type_index,
    };

#if HAVE_EXTERNAL_MEMORY
    VkExportMemoryAllocateInfo export_memory_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes =
#ifdef WIN32
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR
#else
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
#endif
            ,
    };
#endif
    void *alloc_p_next = NULL;
    VkMemoryDedicatedAllocateInfo dedicated_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = img->image,
    };
    if (dedicated_requirements.requiresDedicatedAllocation == VK_TRUE) {
        alloc_p_next = &dedicated_alloc_info;
    }
#if HAVE_EXTERNAL_MEMORY
    if (use_external_memory) {
        export_memory_alloc_info.pNext = alloc_p_next;
        alloc_p_next = &export_memory_alloc_info;
    }
#endif
    alloc_info.pNext = alloc_p_next;

    result = vkAllocateMemory(r->device, &alloc_info, NULL, &img->memory);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "create_display_image: vkAllocateMemory failed (%d)\n", result);
        vkDestroyImage(r->device, img->image, NULL);
        img->image = VK_NULL_HANDLE;
        return false;
    }
    result = vkBindImageMemory(r->device, img->image, img->memory, 0);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "create_display_image: vkBindImageMemory failed (%d)\n", result);
        vkFreeMemory(r->device, img->memory, NULL);
        img->memory = VK_NULL_HANDLE;
        vkDestroyImage(r->device, img->image, NULL);
        img->image = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_create_info.format,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.levelCount = 1,
        .subresourceRange.layerCount = 1,
    };
    result = vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &img->image_view);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "create_display_image: vkCreateImageView failed (%d)\n", result);
        vkFreeMemory(r->device, img->memory, NULL);
        img->memory = VK_NULL_HANDLE;
        vkDestroyImage(r->device, img->image, NULL);
        img->image = VK_NULL_HANDLE;
        return false;
    }

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VK_CHECK(vkCreateFence(r->device, &fence_info, NULL, &img->fence));

    {
        VkCommandBufferAllocateInfo cmd_alloc = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = r->command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VK_CHECK(vkAllocateCommandBuffers(r->device, &cmd_alloc, &img->cmd_buffer));
    }

#if HAVE_EXTERNAL_MEMORY && !defined(__ANDROID__)
    if (use_external_memory) {
#ifdef WIN32
        VkMemoryGetWin32HandleInfoKHR handle_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
            .memory = img->memory,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR
        };
        VK_CHECK(vkGetMemoryWin32HandleKHR(r->device, &handle_info, &img->handle));

        p_glCreateMemoryObjectsEXT(1, &img->gl_memory_obj);
        glImportMemoryWin32HandleEXT(img->gl_memory_obj, memory_requirements.size,
                                     GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, img->handle);
        assert(glGetError() == GL_NO_ERROR);
#else
        VkMemoryGetFdInfoKHR fd_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = img->memory,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        };
        result = vkGetMemoryFdKHR(r->device, &fd_info, &img->fd);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "create_display_image: vkGetMemoryFdKHR failed (%d)\n", result);
            destroy_single_display_image(pg, img);
            return false;
        }

        p_glCreateMemoryObjectsEXT(1, &img->gl_memory_obj);
        p_glImportMemoryFdEXT(img->gl_memory_obj, memory_requirements.size,
                              GL_HANDLE_TYPE_OPAQUE_FD_EXT, img->fd);
        if (!p_glIsMemoryObjectEXT(img->gl_memory_obj) || glGetError() != GL_NO_ERROR) {
            fprintf(stderr, "create_display_image: GL memory object import failed\n");
            destroy_single_display_image(pg, img);
            return false;
        }
#endif

        const GLint gl_internal_format = GL_RGBA8;
        glGenTextures(1, &img->gl_texture_id);
        glBindTexture(GL_TEXTURE_2D, img->gl_texture_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT,
                        use_optimal_tiling ? GL_OPTIMAL_TILING_EXT :
                                             GL_LINEAR_TILING_EXT);
        p_glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, gl_internal_format,
                               width, height, img->gl_memory_obj, 0);
        if (glGetError() != GL_NO_ERROR) {
            fprintf(stderr, "create_display_image: glTexStorageMem2DEXT failed\n");
            destroy_single_display_image(pg, img);
            return false;
        }
    }
#endif

    return true;
}

#ifdef __ANDROID__
static bool create_android_swapchain(PGRAPHState *pg, int width, int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;
    VkSurfaceCapabilitiesKHR caps;
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        r->physical_device, r->present_surface, &caps);
    if (result != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, "hakuX-vk",
                            "present: surface capabilities failed (%d)",
                            result);
        return false;
    }

    uint32_t format_count = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(
        r->physical_device, r->present_surface, &format_count, NULL));
    if (format_count == 0) {
        return false;
    }
    g_autofree VkSurfaceFormatKHR *formats =
        g_new(VkSurfaceFormatKHR, format_count);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(
        r->physical_device, r->present_surface, &format_count, formats));

    VkSurfaceFormatKHR selected = formats[0];
    bool found_rgba = false;
    for (uint32_t i = 0; i < format_count; i++) {
        if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            selected = formats[i];
            found_rgba = true;
            break;
        }
    }
    if (!found_rgba && selected.format != VK_FORMAT_UNDEFINED) {
        __android_log_print(ANDROID_LOG_ERROR, "hakuX-vk",
                            "present: RGBA8 swapchain format unavailable");
        return false;
    }
    if (selected.format == VK_FORMAT_UNDEFINED) {
        selected.format = VK_FORMAT_R8G8B8A8_UNORM;
        selected.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX || extent.height == UINT32_MAX) {
        int drawable_width = 0;
        int drawable_height = 0;
        extern void xemu_android_vulkan_get_drawable_size(
            int *width, int *height);
        xemu_android_vulkan_get_drawable_size(&drawable_width,
                                               &drawable_height);
        extent.width = CLAMP(drawable_width, (int)caps.minImageExtent.width,
                             (int)caps.maxImageExtent.width);
        extent.height = CLAMP(drawable_height, (int)caps.minImageExtent.height,
                              (int)caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        return false;
    }

    /*
     * FIFO is the only mode guaranteed to exist and is what we want when the
     * user asks for vsync. With vsync off, prefer MAILBOX so a frame finished
     * mid-refresh replaces the queued one instead of stalling the render
     * thread; fall back to IMMEDIATE, then FIFO.
     */
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!g_config.display.window.vsync) {
        uint32_t mode_count = 0;
        VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(
            r->physical_device, r->present_surface, &mode_count, NULL));
        if (mode_count > 0) {
            g_autofree VkPresentModeKHR *modes =
                g_new(VkPresentModeKHR, mode_count);
            VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(
                r->physical_device, r->present_surface, &mode_count, modes));
            for (uint32_t i = 0; i < mode_count; i++) {
                if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
                    present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
                    break;
                }
                if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                    present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
                }
            }
        }
    }

    /* MAILBOX needs a third image to have anything to swap in. */
    uint32_t min_images =
        present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? 3u : 2u;
    uint32_t image_count = MAX(caps.minImageCount, min_images);
    if (caps.maxImageCount && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
        if (present_mode == VK_PRESENT_MODE_MAILBOX_KHR && image_count < 3) {
            present_mode = VK_PRESENT_MODE_FIFO_KHR;
        }
    }
    if (image_count > NUM_DISPLAY_IMAGES) {
        __android_log_print(
            ANDROID_LOG_ERROR, "hakuX-vk",
            "present: swapchain requires %u images (capacity %u)",
            image_count, NUM_DISPLAY_IMAGES);
        return false;
    }

    QueueFamilyIndices queue_indices =
        pgraph_vk_find_queue_families(r->physical_device);
    VkBool32 present_supported = VK_FALSE;
    VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(
        r->physical_device, queue_indices.queue_family, r->present_surface,
        &present_supported));
    if (!present_supported) {
        __android_log_print(ANDROID_LOG_ERROR, "hakuX-vk",
                            "present: graphics queue cannot present");
        return false;
    }

    VkCompositeAlphaFlagBitsKHR composite_alpha =
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & composite_alpha)) {
        static const VkCompositeAlphaFlagBitsKHR choices[] = {
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };
        for (int i = 0; i < ARRAY_SIZE(choices); i++) {
            if (caps.supportedCompositeAlpha & choices[i]) {
                composite_alpha = choices[i];
                break;
            }
        }
    }

    /*
     * Android's currentTransform describes the device-panel rotation, not
     * the already-landscape SDL window coordinates. Prefer identity so the
     * compositor does not rotate the Vulkan game surface underneath the
     * correctly oriented Java controller overlay.
     */
    VkSurfaceTransformFlagBitsKHR pre_transform = caps.currentTransform;
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        pre_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }

    /*
     * Save state thumbnails read the presented frame back off the swapchain.
     * The extra usage bit is optional; without it thumbnails are skipped.
     */
    d->capture.supported =
        (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

    VkSwapchainCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = r->present_surface,
        .minImageCount = image_count,
        .imageFormat = selected.format,
        .imageColorSpace = selected.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      (d->capture.supported ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                            : 0),
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = pre_transform,
        .compositeAlpha = composite_alpha,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
    };
    result =
        vkCreateSwapchainKHR(r->device, &create_info, NULL, &d->swapchain);
    if (result != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, "hakuX-vk",
                            "present: vkCreateSwapchainKHR failed (%d)",
                            result);
        return false;
    }

    VK_CHECK(vkGetSwapchainImagesKHR(r->device, d->swapchain,
                                     &image_count, NULL));
    if (image_count > NUM_DISPLAY_IMAGES) {
        destroy_current_display_image(pg);
        return false;
    }
    VkImage swapchain_images[NUM_DISPLAY_IMAGES];
    VK_CHECK(vkGetSwapchainImagesKHR(r->device, d->swapchain,
                                     &image_count, swapchain_images));

    d->swapchain_format = selected.format;
    d->swapchain_extent = extent;
    extern int xemu_android_get_display_mode_setting(void);
    int display_mode = xemu_android_get_display_mode_setting();
    if (display_mode == 0) {
        d->present_viewport.extent = extent;
    } else {
        double target_aspect =
            display_mode == 1 ? (4.0 / 3.0) : (16.0 / 9.0);
        double surface_aspect = (double)extent.width / extent.height;
        if (surface_aspect > target_aspect) {
            d->present_viewport.extent.height = extent.height;
            d->present_viewport.extent.width =
                MAX(1u, (uint32_t)lround(extent.height * target_aspect));
        } else {
            d->present_viewport.extent.width = extent.width;
            d->present_viewport.extent.height =
                MAX(1u, (uint32_t)lround(extent.width / target_aspect));
        }
    }
    d->present_viewport.offset.x =
        (int32_t)(extent.width - d->present_viewport.extent.width) / 2;
    d->present_viewport.offset.y =
        (int32_t)(extent.height - d->present_viewport.extent.height) / 2;
    d->image_count = image_count;
    d->width = width;
    d->height = height;
    d->render_idx = 0;
    d->display_idx = 0;
    d->present_frame = 0;
    memset(d->image_in_flight, 0, sizeof(d->image_in_flight));

    for (uint32_t i = 0; i < image_count; i++) {
        DisplayImage *img = &d->images[i];
        memset(img, 0, sizeof(*img));
        img->image = swapchain_images[i];
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = img->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = selected.format,
            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .subresourceRange.levelCount = 1,
            .subresourceRange.layerCount = 1,
        };
        VK_CHECK(vkCreateImageView(r->device, &view_info, NULL,
                                   &img->image_view));

        VkFramebufferCreateInfo framebuffer_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = d->render_pass,
            .attachmentCount = 1,
            .pAttachments = &img->image_view,
            .width = extent.width,
            .height = extent.height,
            .layers = 1,
        };
        VK_CHECK(vkCreateFramebuffer(r->device, &framebuffer_info, NULL,
                                     &img->framebuffer));

        VkCommandBufferAllocateInfo command_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = r->command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VK_CHECK(vkAllocateCommandBuffers(r->device, &command_info,
                                          &img->cmd_buffer));
    }

    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    for (uint32_t i = 0; i < image_count; i++) {
        VK_CHECK(vkCreateSemaphore(r->device, &semaphore_info, NULL,
                                   &d->image_available[i]));
        VK_CHECK(vkCreateSemaphore(r->device, &semaphore_info, NULL,
                                   &d->render_finished[i]));
        VK_CHECK(vkCreateFence(r->device, &fence_info, NULL,
                               &d->present_fences[i]));
    }

    __android_log_print(
        ANDROID_LOG_INFO, "hakuX-vk",
        "present: all-Vulkan swapchain %ux%u images=%u format=%d present_mode=%d "
        "transform current=0x%x supported=0x%x chosen=0x%x mode=%d "
        "viewport=%d,%d %ux%u source=%dx%d",
        extent.width, extent.height, image_count, selected.format, present_mode,
        caps.currentTransform, caps.supportedTransforms, pre_transform,
        display_mode, d->present_viewport.offset.x,
        d->present_viewport.offset.y, d->present_viewport.extent.width,
        d->present_viewport.extent.height, width, height);
    return true;
}
#endif

static bool create_display_image(PGRAPHState *pg, int width, int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (d->images[0].image != VK_NULL_HANDLE) {
        destroy_current_display_image(pg);
    }

#ifdef __ANDROID__
    if (d->direct_present) {
        return create_android_swapchain(pg, width, height);
    }
#endif

    bool use_optimal_tiling = true;
#if HAVE_EXTERNAL_MEMORY
    bool use_external_memory = d->use_external_memory;

#ifdef __ANDROID__
    if (use_external_memory && !load_ahb_interop_symbols(r->device)) {
        __android_log_print(ANDROID_LOG_WARN, "hakuX",
                            "display: AHB interop not available, using download fallback");
        d->use_external_memory = false;
        use_external_memory = false;
    }
#else
    if (use_external_memory) {
        pgraph_vk_gl_make_context_current();
    }

    if (use_external_memory && !load_gl_external_memory_symbols()) {
        fprintf(stderr, "Vulkan display: GL_EXT_memory_object not available\n");
        d->use_external_memory = false;
        use_external_memory = false;
    }

    if (use_external_memory) {
        const GLint gl_internal_format = GL_RGBA8;
        GLint num_tiling_types;
        glGetInternalformativ(GL_TEXTURE_2D, gl_internal_format,
                              GL_NUM_TILING_TYPES_EXT, 1, &num_tiling_types);
        GLint tiling_types[num_tiling_types];
        glGetInternalformativ(GL_TEXTURE_2D, gl_internal_format,
                              GL_TILING_TYPES_EXT, num_tiling_types, tiling_types);
        for (int i = 0; i < num_tiling_types; i++) {
            if (tiling_types[i] == GL_LINEAR_TILING_EXT) {
                use_optimal_tiling = false;
                break;
            }
        }
    }
#endif
#else
    bool use_external_memory = false;
#endif

    for (int i = 0; i < NUM_DISPLAY_IMAGES; i++) {
        if (!create_single_display_image_resources(pg, &d->images[i], width, height,
                                                   use_optimal_tiling, use_external_memory)) {
            destroy_current_display_image(pg);
            return false;
        }
    }

    d->width = width;
    d->height = height;
    d->image_count = NUM_DISPLAY_IMAGES;
    d->render_idx = 0;
    d->display_idx = 0;

    for (int i = 0; i < NUM_DISPLAY_IMAGES; i++) {
        create_frame_buffer(pg, &d->images[i]);
    }

    {
        VkImageCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { width, height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VmaAllocationCreateInfo ai = {
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };
        VK_CHECK(vmaCreateImage(r->allocator, &ci, &ai,
                                &d->blend_prev_image,
                                &d->blend_prev_alloc, NULL));

        VkImageViewCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = d->blend_prev_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .subresourceRange.levelCount = 1,
            .subresourceRange.layerCount = 1,
        };
        VK_CHECK(vkCreateImageView(r->device, &vi, NULL,
                                   &d->blend_prev_view));
        d->blend_prev_valid = false;
    }

    return true;
}

static void update_descriptor_set(PGRAPHState *pg, SurfaceBinding *surface)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorImageInfo image_infos[3];
    VkWriteDescriptorSet descriptor_writes[3];

    image_infos[0] = (VkDescriptorImageInfo){
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView = surface->image_view,
        .sampler = r->display.sampler,
    };
    VkDescriptorSet current_set = r->display.descriptor_sets[r->display.render_idx];

    descriptor_writes[0] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = current_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &image_infos[0],
    };

    if (r->display.pvideo.state.enabled) {
        assert(r->display.pvideo.image_views[r->display.render_idx] !=
               VK_NULL_HANDLE);
        assert(r->display.pvideo.sampler != VK_NULL_HANDLE);
        image_infos[1] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView =
                r->display.pvideo.image_views[r->display.render_idx],
            .sampler = r->display.pvideo.sampler,
        };
    } else {
        image_infos[1] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = r->dummy_texture.image_view,
            .sampler = r->dummy_texture.sampler,
        };
    }

    if (r->display.blend_prev_valid && r->display.blend_prev_view) {
        image_infos[2] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = r->display.blend_prev_view,
            .sampler = r->display.sampler,
        };
    } else {
        image_infos[2] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = r->dummy_texture.image_view,
            .sampler = r->dummy_texture.sampler,
        };
    }
    descriptor_writes[1] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = current_set,
        .dstBinding = 1,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &image_infos[1],
    };

    descriptor_writes[2] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = current_set,
        .dstBinding = 2,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &image_infos[2],
    };

    vkUpdateDescriptorSets(r->device, ARRAY_SIZE(descriptor_writes),
                           descriptor_writes, 0, NULL);
}

static PvideoState get_pvideo_state(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PvideoState state = { 0 };

    // FIXME: This check against PVIDEO_SIZE_IN does not match HW behavior.
    // Many games seem to pass this value when initializing or tearing down
    // PVIDEO. On its own, this generally does not result in the overlay being
    // hidden, however there are certain games (e.g., Ultimate Beach Soccer)
    // that use an unknown mechanism to hide the overlay without explicitly
    // stopping it.
    // Since the value seems to be set to 0xFFFFFFFF only in cases where the
    // content is not valid, it is probably good enough to treat it as an
    // implicit stop.
    state.enabled = (d->pvideo.regs[NV_PVIDEO_BUFFER] & NV_PVIDEO_BUFFER_0_USE)
        && d->pvideo.regs[NV_PVIDEO_SIZE_IN] != 0xFFFFFFFF;
    if (!state.enabled) {
        return state;
    }

    state.base = d->pvideo.regs[NV_PVIDEO_BASE];
    state.limit = d->pvideo.regs[NV_PVIDEO_LIMIT];
    state.offset = d->pvideo.regs[NV_PVIDEO_OFFSET];

    state.pitch =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_PITCH);
    state.format =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_COLOR);

    /* TODO: support other color formats */
    if (state.format != NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8) {
        warn_report_once("PVIDEO disabled: unsupported color format 0x%x",
                         state.format);
        state.enabled = false;
        return state;
    }

    state.in_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_WIDTH);
    state.in_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_HEIGHT);

    state.out_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_WIDTH);
    state.out_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_HEIGHT);

    if (state.in_width <= 0 || state.in_height <= 0 || state.out_width <= 0 ||
        state.out_height <= 0 || state.pitch <= 0) {
        warn_report_once("PVIDEO disabled: invalid dimensions or pitch "
                         "(pitch=%d, input=%dx%d, output=%dx%d)",
                         state.pitch, state.in_width, state.in_height,
                         state.out_width, state.out_height);
        state.enabled = false;
        return state;
    }

    state.in_s =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN], NV_PVIDEO_POINT_IN_S);
    state.in_t =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN], NV_PVIDEO_POINT_IN_T);

    uint32_t ds_dx = d->pvideo.regs[NV_PVIDEO_DS_DX];
    uint32_t dt_dy = d->pvideo.regs[NV_PVIDEO_DT_DY];
    state.scale_x = ds_dx == NV_PVIDEO_DIN_DOUT_UNITY ?
                        1.0f :
                        pvideo_calculate_scale(ds_dx, state.out_width);
    state.scale_y = dt_dy == NV_PVIDEO_DIN_DOUT_UNITY ?
                        1.0f :
                        pvideo_calculate_scale(dt_dy, state.out_height);

    // On HW, setting NV_PVIDEO_SIZE_IN larger than NV_PVIDEO_SIZE_OUT results
    // in them being capped to the output size, content is not scaled. This is
    // particularly important as NV_PVIDEO_SIZE_IN may be set to 0xFFFFFFFF
    // during initialization or teardown.
    if (state.in_width > state.out_width) {
        state.in_width = floorf((float)state.out_width * state.scale_x + 0.5f);
    }
    if (state.in_height > state.out_height) {
        state.in_height =
            floorf((float)state.out_height * state.scale_y + 0.5f);
    }

    if (state.in_width <= 0 || state.in_height <= 0) {
        warn_report_once("PVIDEO disabled: scaling produced invalid input "
                         "dimensions %dx%d",
                         state.in_width, state.in_height);
        state.enabled = false;
        return state;
    }

    uint64_t row_bytes = (uint64_t)state.in_width * 2;
    if ((uint64_t)state.pitch < row_bytes) {
        warn_report_once(
            "PVIDEO disabled: pitch %d is smaller than row size %" PRIu64,
            state.pitch, row_bytes);
        state.enabled = false;
        return state;
    }

    if ((uint64_t)state.pitch > UINT64_MAX / (uint64_t)state.in_height) {
        warn_report_once("PVIDEO disabled: source span overflow");
        state.enabled = false;
        return state;
    }

    uint64_t span = (uint64_t)state.pitch * (uint64_t)state.in_height;
    uint64_t offset = state.offset;
    uint64_t limit = state.limit;
    if (offset > limit || span > limit - offset) {
        warn_report_once("PVIDEO disabled: source exceeds its DMA limit");
        state.enabled = false;
        return state;
    }

    uint64_t base = state.base;
    uint64_t vram_size = memory_region_size(d->vram);
    if (base > vram_size || offset > vram_size - base ||
        span > vram_size - base - offset) {
        warn_report_once("PVIDEO disabled: source exceeds guest VRAM");
        state.enabled = false;
        return state;
    }

    uint64_t pixel_count = (uint64_t)state.in_width * state.in_height;
    if (pixel_count > SIZE_MAX / 4) {
        warn_report_once("PVIDEO disabled: converted image size overflow");
        state.enabled = false;
        return state;
    }

    state.out_x =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_X);
    state.out_y =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_Y);

    state.color_key_enabled =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_DISPLAY);

    // Note: PVIDEO color keying ignores alpha.
    state.color_key = d->pvideo.regs[NV_PVIDEO_COLOR_KEY] & 0xFFFFFF;

    return state;
}

static void update_uniforms(PGRAPHState *pg, SurfaceBinding *surface)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    ShaderUniformLayout *l = &r->display.display_frag->push_constants;

    int display_size_loc = uniform_index(l, "display_size");  // FIXME: Cache
    uniform2f(l, display_size_loc, r->display.width, r->display.height);

#ifdef __ANDROID__
    if (r->display.direct_present) {
        uniform2f(l, uniform_index(l, "output_offset"),
                  r->display.present_viewport.offset.x,
                  r->display.present_viewport.offset.y);
        uniform2f(l, uniform_index(l, "output_size"),
                  r->display.present_viewport.extent.width,
                  r->display.present_viewport.extent.height);
    } else
#endif
    {
        uniform2f(l, uniform_index(l, "output_offset"), 0.0f, 0.0f);
        uniform2f(l, uniform_index(l, "output_size"), r->display.width,
                  r->display.height);
    }
    uniform1i(l, uniform_index(l, "flip_y"),
              !r->display.direct_present);

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);
    int line_offset = vga_display_params.line_offset ?
                          surface->pitch / vga_display_params.line_offset :
                          1;
    int line_offset_loc = uniform_index(l, "line_offset");
    uniform1f(l, line_offset_loc, line_offset);

    PvideoState *pvideo = &r->display.pvideo.state;
    uniform1i(l, uniform_index(l, "pvideo_enable"), pvideo->enabled);
    if (pvideo->enabled) {
        uniform1i(l, uniform_index(l, "pvideo_color_key_enable"),
                  pvideo->color_key_enabled);
        uniform3f(
            l, uniform_index(l, "pvideo_color_key"),
            GET_MASK(pvideo->color_key, NV_PVIDEO_COLOR_KEY_RED) / 255.0,
            GET_MASK(pvideo->color_key, NV_PVIDEO_COLOR_KEY_GREEN) / 255.0,
            GET_MASK(pvideo->color_key, NV_PVIDEO_COLOR_KEY_BLUE) / 255.0);
        uniform2f(l, uniform_index(l, "pvideo_in_pos"), pvideo->in_s / 16.f,
                  pvideo->in_t / 8.f);
        uniform4f(l, uniform_index(l, "pvideo_pos"), pvideo->out_x,
                  pvideo->out_y, pvideo->out_width, pvideo->out_height);
        uniform4f(l, uniform_index(l, "pvideo_scale"), pvideo->scale_x,
                  pvideo->scale_y, 1.0f / pg->surface_scale_factor, 1.0);
    }

    uniform1f(l, uniform_index(l, "blend_factor"),
              r->display.blend_active ? 0.5f : 0.0f);
}

static void render_display(PGRAPHState *pg, SurfaceBinding *surface)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;
    uint32_t present_sync_index = 0;
#ifdef __ANDROID__
    bool capture_this_frame = false;
#endif

#ifdef __ANDROID__
    if (disp->direct_present) {
        present_sync_index = disp->present_frame % disp->image_count;
        VK_CHECK(vkWaitForFences(
            r->device, 1, &disp->present_fences[present_sync_index],
            VK_TRUE, UINT64_MAX));

        uint32_t acquired_index = 0;
        VkResult acquire_result = vkAcquireNextImageKHR(
            r->device, disp->swapchain, UINT64_MAX,
            disp->image_available[present_sync_index], VK_NULL_HANDLE,
            &acquired_index);
        if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
            __android_log_print(ANDROID_LOG_WARN, "hakuX-vk",
                                "present: swapchain out of date");
            return;
        }
        if (acquire_result != VK_SUCCESS &&
            acquire_result != VK_SUBOPTIMAL_KHR) {
            __android_log_print(ANDROID_LOG_ERROR, "hakuX-vk",
                                "present: acquire failed (%d)",
                                acquire_result);
            return;
        }
        disp->render_idx = acquired_index;
        if (disp->image_in_flight[acquired_index] != VK_NULL_HANDLE) {
            VK_CHECK(vkWaitForFences(
                r->device, 1, &disp->image_in_flight[acquired_index],
                VK_TRUE, UINT64_MAX));
        }

        if (disp->capture.supported &&
            qatomic_read(&disp->capture.state) == DISPLAY_CAPTURE_REQUESTED) {
            size_t capture_size =
                (size_t)disp->present_viewport.extent.width *
                disp->present_viewport.extent.height * 4;
            if (ensure_display_capture_buffer(pg, capture_size)) {
                capture_this_frame = true;
            } else {
                __android_log_print(ANDROID_LOG_WARN, "hakuX-vk",
                                    "capture: staging buffer allocation failed");
                disp->capture.supported = false;
                qatomic_set(&disp->capture.state, DISPLAY_CAPTURE_IDLE);
            }
        }
    }
#endif

    DisplayImage *img = &disp->images[disp->render_idx];

    if (img->image == VK_NULL_HANDLE || img->framebuffer == VK_NULL_HANDLE) {
        return;
    }

    /* The descriptor set and PVIDEO image use the same display-image slot.
     * Wait before updating either resource when the compatibility presenter
     * cycles back to a slot that is still in flight. */
    if (!disp->direct_present && img->fence_submitted) {
        VK_CHECK(vkWaitForFences(r->device, 1, &img->fence, VK_TRUE,
                                 UINT64_MAX));
        img->fence_submitted = false;
    }

    {
        static int dbg_render = 0;
        if (dbg_render < 30) {
            DBG_LOG("[DISP] render_display: in_cb=%d draw_time=%lu cb_start=%lu",
                    r->in_command_buffer,
                    (unsigned long)surface->draw_time,
                    (unsigned long)r->command_buffer_start_time);
            dbg_render++;
        }
    }

    if (r->in_command_buffer &&
        surface->draw_time >= r->command_buffer_start_time) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_PRESENTING);
    }

    pgraph_vk_upload_surface_data(d, surface, !tcg_enabled());

    disp->pvideo.state = get_pvideo_state(pg);
    if (disp->pvideo.state.enabled) {
        upload_pvideo_image(pg, disp->pvideo.state);
    }

    update_uniforms(pg, surface);
    update_descriptor_set(pg, surface);

    VK_CHECK(vkResetCommandBuffer(img->cmd_buffer, 0));
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(img->cmd_buffer, &begin_info));
    VkCommandBuffer cmd = img->cmd_buffer;

    if (disp->pvideo.state.enabled &&
        disp->pvideo.upload_pending[disp->render_idx]) {
        int pvideo_index = disp->render_idx;
        VkDeviceSize upload_size =
            (VkDeviceSize)disp->pvideo.width * disp->pvideo.height * 4;
        VkBufferMemoryBarrier host_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = disp->pvideo.staging_buffers[pvideo_index],
            .offset = 0,
            .size = upload_size,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, NULL, 1, &host_barrier, 0, NULL);

        pgraph_vk_transition_image_layout(
            pg, cmd, disp->pvideo.images[pvideo_index],
            VK_FORMAT_R8G8B8A8_UNORM,
            disp->pvideo.image_valid[pvideo_index]
                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy region = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = {
                disp->pvideo.width,
                disp->pvideo.height,
                1,
            },
        };
        vkCmdCopyBufferToImage(
            cmd, disp->pvideo.staging_buffers[pvideo_index],
            disp->pvideo.images[pvideo_index],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        pgraph_vk_transition_image_layout(
            pg, cmd, disp->pvideo.images[pvideo_index],
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        disp->pvideo.image_valid[pvideo_index] = true;
        disp->pvideo.upload_pending[pvideo_index] = false;
    }

    pgraph_vk_begin_debug_marker(r, cmd, RGBA_YELLOW,
        "Display Surface %08"HWADDR_PRIx, surface->vram_addr);

    VkImageLayout surface_layout = surface->image_layout;
    pgraph_vk_transition_image_layout(pg, cmd, surface->image,
                                      surface->host_fmt.vk_format,
                                      surface_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkImageLayout display_old_layout =
        img->valid ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                   : VK_IMAGE_LAYOUT_UNDEFINED;
    if (!disp->direct_present) {
        display_old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    pgraph_vk_transition_image_layout(
        pg, cmd, img->image, VK_FORMAT_R8G8B8A8_UNORM,
        display_old_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkClearValue clear_value = {
        .color.float32 = { 0.0f, 0.0f, 0.0f, 1.0f },
    };
    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = disp->render_pass,
        .framebuffer = img->framebuffer,
        .renderArea.extent.width =
            disp->direct_present ? disp->swapchain_extent.width : disp->width,
        .renderArea.extent.height =
            disp->direct_present ? disp->swapchain_extent.height : disp->height,
        .clearValueCount = 1,
        .pClearValues = &clear_value,
    };
    vkCmdBeginRenderPass(cmd, &render_pass_begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      disp->pipeline);

    VkDescriptorSet current_ds = disp->descriptor_sets[disp->render_idx];
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            disp->pipeline_layout, 0, 1, &current_ds,
                            0, NULL);

    VkViewport viewport = {
        .x = disp->direct_present ? disp->present_viewport.offset.x : 0,
        .y = disp->direct_present ? disp->present_viewport.offset.y : 0,
        .width = disp->direct_present ?
                     disp->present_viewport.extent.width :
                     disp->width,
        .height = disp->direct_present ?
                      disp->present_viewport.extent.height :
                      disp->height,
        .minDepth = 0.0,
        .maxDepth = 1.0,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {
        .offset = disp->direct_present ? disp->present_viewport.offset :
                                        (VkOffset2D){ 0, 0 },
        .extent = disp->direct_present ? disp->present_viewport.extent :
                                        (VkExtent2D){ disp->width,
                                                      disp->height },
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdPushConstants(cmd, disp->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, disp->display_frag->push_constants.total_size,
                       disp->display_frag->push_constants.allocation);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);

    pgraph_vk_transition_image_layout(pg, cmd, surface->image,
                                      surface->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      surface_layout);

    if (!disp->direct_present && !disp->blend_active &&
        disp->blend_prev_image) {
        pgraph_vk_transition_image_layout(pg, cmd, img->image,
                                          VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        pgraph_vk_transition_image_layout(pg, cmd, disp->blend_prev_image,
                                          VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkImageCopy region = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .extent = { disp->width, disp->height, 1 },
        };
        vkCmdCopyImage(cmd,
                       img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       disp->blend_prev_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region);

        pgraph_vk_transition_image_layout(pg, cmd, disp->blend_prev_image,
                                          VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        pgraph_vk_transition_image_layout(pg, cmd, img->image,
                                          VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        disp->blend_prev_valid = true;
    } else if (disp->direct_present) {
#ifdef __ANDROID__
        if (capture_this_frame) {
            pgraph_vk_transition_image_layout(
                pg, cmd, img->image, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

            VkBufferImageCopy capture_region = {
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageOffset = { disp->present_viewport.offset.x,
                                 disp->present_viewport.offset.y, 0 },
                .imageExtent = { disp->present_viewport.extent.width,
                                 disp->present_viewport.extent.height, 1 },
            };
            vkCmdCopyImageToBuffer(cmd, img->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   disp->capture.buffer, 1, &capture_region);

            pgraph_vk_transition_image_layout(
                pg, cmd, img->image, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        } else
#endif
        {
            pgraph_vk_transition_image_layout(
                pg, cmd, img->image, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        }
    } else {
        pgraph_vk_transition_image_layout(pg, cmd, img->image,
                                          VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    pgraph_vk_end_debug_marker(r, cmd);

    VK_CHECK(vkEndCommandBuffer(cmd));

    pgraph_vk_render_thread_wait_idle(r);

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
#ifdef __ANDROID__
    VkPipelineStageFlags present_wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (disp->direct_present) {
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores =
            &disp->image_available[present_sync_index];
        submit_info.pWaitDstStageMask = &present_wait_stage;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores =
            &disp->render_finished[present_sync_index];
        VK_CHECK(vkResetFences(
            r->device, 1, &disp->present_fences[present_sync_index]));
        VK_CHECK(vkQueueSubmit(
            r->queue, 1, &submit_info,
            disp->present_fences[present_sync_index]));

        uint32_t present_image_index = disp->render_idx;
        disp->image_in_flight[present_image_index] =
            disp->present_fences[present_sync_index];
        VkPresentInfoKHR present_info = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &disp->render_finished[present_sync_index],
            .swapchainCount = 1,
            .pSwapchains = &disp->swapchain,
            .pImageIndices = &present_image_index,
        };
        VkResult present_result = vkQueuePresentKHR(r->queue, &present_info);
        if (present_result != VK_SUCCESS &&
            present_result != VK_SUBOPTIMAL_KHR &&
            present_result != VK_ERROR_OUT_OF_DATE_KHR) {
            __android_log_print(ANDROID_LOG_ERROR, "hakuX-vk",
                                "present: queue present failed (%d)",
                                present_result);
        }
        disp->present_frame++;

        if (capture_this_frame) {
            /*
             * Stalling for the readback is acceptable here: a capture is only
             * ever requested for a single frame when a save state is written.
             */
            VK_CHECK(vkWaitForFences(
                r->device, 1, &disp->present_fences[present_sync_index],
                VK_TRUE, UINT64_MAX));
            VK_CHECK(vmaInvalidateAllocation(r->allocator,
                                             disp->capture.allocation, 0,
                                             VK_WHOLE_SIZE));
            disp->capture.width = disp->present_viewport.extent.width;
            disp->capture.height = disp->present_viewport.extent.height;
            qatomic_set(&disp->capture.state, DISPLAY_CAPTURE_READY);
        }
    } else
#endif
    {
        VK_CHECK(vkResetFences(r->device, 1, &img->fence));
        VK_CHECK(vkQueueSubmit(r->queue, 1, &submit_info, img->fence));
        img->fence_submitted = true;
    }
    img->valid = true;

    disp->display_idx = disp->render_idx;
    if (!disp->direct_present) {
        disp->render_idx =
            (disp->render_idx + 1) % disp->image_count;
    }
#ifdef __ANDROID__
    {
        static int render_count = 0;
        if (render_count < 5) {
            __android_log_print(ANDROID_LOG_INFO, "hakuX",
                "display: render_display done #%d disp_idx=%d render_idx=%d tex=%u",
                render_count, disp->display_idx, disp->render_idx,
                disp->images[disp->display_idx].gl_texture_id);
            render_count++;
        }
    }
#endif
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_5);

    disp->draw_time = surface->draw_time;
}

static void create_surface_sampler(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_FALSE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE,
        .unnormalizedCoordinates = VK_FALSE,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    };

    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &r->display.sampler));
}

static void destroy_surface_sampler(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroySampler(r->device, r->display.sampler, NULL);
    r->display.sampler = VK_NULL_HANDLE;
}

void pgraph_vk_init_display(PGRAPHState *pg)
{
    create_descriptor_pool(pg);
    create_descriptor_set_layout(pg);
    create_descriptor_sets(pg);
    create_render_pass(pg);
    create_display_pipeline(pg);
    create_surface_sampler(pg);
}

void pgraph_vk_finalize_display(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    destroy_pvideo_image(pg);

#ifdef __ANDROID__
    destroy_display_capture_buffer(pg);
#endif

    if (r->display.images[0].image != VK_NULL_HANDLE) {
        destroy_current_display_image(pg);
    }

    destroy_surface_sampler(pg);
    destroy_display_pipeline(pg);
    destroy_render_pass(pg);
    destroy_descriptor_set_layout(pg);
    destroy_descriptor_pool(pg);
}

void pgraph_vk_render_display(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    hwaddr display_addr = d->pcrtc.start + vga_display_params.line_offset;

    if (r->frame_was_skipped && xemu_get_frame_skip() &&
        r->frame_skip_last_good_addr) {
        display_addr = r->frame_skip_last_good_addr;
    } else {
        r->frame_skip_last_good_addr = display_addr;
    }

    SurfaceBinding *surface = pgraph_vk_surface_get_within(d, display_addr);
    if (surface == NULL || !surface->color || !surface->width ||
        !surface->height) {
        static int dbg_no_surf = 0;
        if (dbg_no_surf < 30) {
            DBG_LOG("[DISP] no valid surface (surface=%p)", surface);
            dbg_no_surf++;
        }
        return;
    }

    unsigned int width = 0, height = 0;
    d->vga.get_resolution(&d->vga, (int *)&width, (int *)&height);

    /* Adjust viewport height for interlaced mode, used only in 1080i */
    if (d->vga.cr[NV_PRMCIO_INTERLACE_MODE] != NV_PRMCIO_INTERLACE_MODE_DISABLED) {
        height *= 2;
    }

    pgraph_apply_scaling_factor(pg, &width, &height);

    PGRAPHVkDisplayState *disp = &r->display;
    if (!disp->images[0].image || disp->width != width || disp->height != height) {
        if (!create_display_image(pg, width, height)) {
            return;
        }
    }

    disp->blend_active =
        !disp->direct_present && !r->frame_was_skipped &&
        r->blend_after_skip && xemu_get_frame_skip() &&
        disp->blend_prev_valid;
    if (disp->blend_active) {
        r->blend_after_skip = false;
    }

    render_display(pg, surface);
}
