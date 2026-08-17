/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
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

#include "renderer.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>
#endif

typedef struct MemoryBudget {
    size_t total_heap;
    unsigned memory_class_gib;
    size_t renderer_budget;
    size_t vertex_inline_cap;
    size_t index_cap;
    size_t staging_cap;
    size_t uniform_cap;
    size_t perframe_vtx_cap;
    size_t perframe_idx_cap;
    size_t perframe_uni_cap;
    size_t perframe_stg_cap;
    size_t shader_module_cache_entries;
    size_t texture_cache_entries;
    size_t allocation_soft_limit;
    int image_pool_max;
    int surface_image_pool_max;
} MemoryBudget;

/*
 * Common device RAM configurations, in GiB.
 *
 * Neither number available to us reports a device's nominal RAM. sysconf()
 * excludes kernel reservations and carveouts, and the largest Vulkan heap is
 * whatever the driver chooses to expose. A nominally-8GB Pixel 10a measured
 * 7.39GiB physical and a 7.17GiB Vulkan heap; a different 8GB device reported
 * a 7.63GiB heap. Tiering directly off either number puts devices of the same
 * class on different rungs, and leaves a borderline device free to change tier
 * between runs, because the heap size is not stable.
 *
 * So snap to the nearest real configuration and tier off that instead. The
 * result is stable across runs and is what the ladders below actually mean to
 * select on.
 */
static const unsigned k_memory_classes_gib[] = { 2, 3, 4, 6, 8, 12, 16, 24 };

static unsigned device_memory_class_gib(size_t total_heap)
{
    const size_t mib = 1024 * 1024;
    size_t phys = 0;

#ifdef __ANDROID__
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        phys = (size_t)pages * (size_t)page_size;
    }
#endif

    /* Physical memory is the better signal; fall back to the heap if the
     * platform will not tell us. */
    if (phys == 0) {
        phys = total_heap;
    }
    if (phys == 0) {
        return 0;
    }

    size_t phys_mib = phys / mib;
    unsigned best = k_memory_classes_gib[0];
    size_t best_delta = SIZE_MAX;

    for (size_t i = 0; i < ARRAY_SIZE(k_memory_classes_gib); i++) {
        size_t class_mib = (size_t)k_memory_classes_gib[i] * 1024;
        size_t delta = phys_mib > class_mib ? phys_mib - class_mib
                                            : class_mib - phys_mib;
        if (delta < best_delta) {
            best_delta = delta;
            best = k_memory_classes_gib[i];
        }
    }

    return best;
}

static MemoryBudget compute_memory_budget(PGRAPHVkState *r)
{
    const size_t mib = 1024 * 1024;
    const size_t gib = 1024 * mib;

    VkPhysicalDeviceMemoryProperties const *props;
    vmaGetMemoryProperties(r->allocator, &props);

    size_t total_heap = 0;
    for (uint32_t i = 0; i < props->memoryHeapCount; i++) {
        if (props->memoryHeaps[i].size > total_heap) {
            total_heap = props->memoryHeaps[i].size;
        }
    }

    MemoryBudget b = { .total_heap = total_heap };

    b.memory_class_gib = device_memory_class_gib(total_heap);

#ifdef __ANDROID__
    if (b.memory_class_gib <= 4) {
        b.renderer_budget = 384 * mib;
        b.allocation_soft_limit = 512 * mib;
    } else if (b.memory_class_gib <= 6) {
        b.renderer_budget = 512 * mib;
        b.allocation_soft_limit = 768 * mib;
    } else if (b.memory_class_gib <= 8) {
        b.renderer_budget = 768 * mib;
        b.allocation_soft_limit = 1024 * mib;
    } else if (b.memory_class_gib <= 12) {
        b.renderer_budget = 1024 * mib;
        b.allocation_soft_limit = 1536 * mib;
    } else if (b.memory_class_gib <= 16) {
        b.renderer_budget = 1536 * mib;
        b.allocation_soft_limit = 2048 * mib;
    } else {
        b.renderer_budget = 2048 * mib;
        b.allocation_soft_limit = 3072 * mib;
    }

    /*
     * The class is what the device nominally has, not what this driver will
     * actually hand out, so never budget more than half of the heap we can
     * really see. Only engages on parts that expose an unusually small heap.
     */
    if (total_heap && b.renderer_budget > total_heap / 2) {
        b.renderer_budget = total_heap / 2;
    }
#else
    b.renderer_budget = SIZE_MAX;
    b.allocation_soft_limit = SIZE_MAX;
#endif
    (void)gib;

    if (b.renderer_budget == SIZE_MAX) {
        b.vertex_inline_cap = SIZE_MAX;
        b.index_cap = SIZE_MAX;
        b.staging_cap = SIZE_MAX;
        b.uniform_cap = SIZE_MAX;
        b.perframe_vtx_cap = SIZE_MAX;
        b.perframe_idx_cap = SIZE_MAX;
        b.perframe_uni_cap = SIZE_MAX;
        b.perframe_stg_cap = SIZE_MAX;
        b.shader_module_cache_entries = 50 * 1024;
        /* Unbudgeted (desktop): keep at least the top mobile tier. */
        b.texture_cache_entries = 2048;
        b.image_pool_max = IMAGE_POOL_MAX_SIZE;
        b.surface_image_pool_max = SURFACE_IMAGE_POOL_MAX_SIZE;
    } else {
        size_t budget = b.renderer_budget;
        /*
         * Leave room for textures, surfaces, pipelines, and driver-private
         * allocations. These caps are per allocation; per-frame allocations
         * are multiplied by the active submit-frame count below.
         */
        b.vertex_inline_cap = MAX(32 * mib, budget / 16);
        b.index_cap = MAX(8 * mib, budget / 64);
        b.staging_cap = MAX(64 * mib, budget / 16);
#ifdef __ANDROID__
        /*
         * This buffer also controls the maximum amount of draw work in one
         * queue submission. Large desktop-sized batches can run for tens of
         * seconds on mobile GPUs and trigger the Android GPU watchdog.
         */
        if (total_heap <= 4 * gib) {
            b.uniform_cap = 1 * mib;
        } else if (total_heap <= 6 * gib) {
            b.uniform_cap = 2 * mib;
        } else if (total_heap <= 8 * gib) {
            b.uniform_cap = 4 * mib;
        } else {
            b.uniform_cap = 8 * mib;
        }
#else
        b.uniform_cap = MAX(16 * mib, budget / 32);
#endif
#ifdef __ANDROID__
        char uniform_kb_property[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.xemu.vk.uniform_kb",
                                  uniform_kb_property) > 0) {
            char *end = NULL;
            unsigned long uniform_kb =
                strtoul(uniform_kb_property, &end, 10);
            if (end != uniform_kb_property && *end == '\0' &&
                uniform_kb >= 8 && uniform_kb <= 8192) {
                b.uniform_cap = uniform_kb * 1024;
                __android_log_print(
                    ANDROID_LOG_WARN, "hakuX-vk",
                    "diagnostic uniform batch override: %luKB", uniform_kb);
            }
        }
#endif
        b.perframe_vtx_cap = MAX(16 * mib, budget / 32);
        b.perframe_idx_cap = MAX(4 * mib, budget / 128);
        b.perframe_uni_cap = MAX(8 * mib, budget / 64);
        b.perframe_stg_cap = MAX(32 * mib, budget / 16);

        size_t budget_mib = budget / mib;
        b.shader_module_cache_entries = budget_mib * 4;
        if (b.shader_module_cache_entries < 2048) {
            b.shader_module_cache_entries = 2048;
        }
        if (b.shader_module_cache_entries > 50 * 1024) {
            b.shader_module_cache_entries = 50 * 1024;
        }

        /*
         * Texture retention ladder.
         *
         * Deliberately keyed on the device memory class, NOT on
         * renderer_budget. renderer_budget sizes staging/vertex/index/uniform
         * *buffers* in bytes; texture retention is an entry count backed by a
         * separate allocator with its own reclaim path. Sharing one knob is
         * what produced the original bug: an 8GB device landed exactly on the
         * 768MB buffer tier and inherited only 256 texture entries with it.
         *
         * These are ceilings, not commitments, and it is safe to set them
         * generously -- memory is reclaimed by two independent mechanisms.
         * Proactively, pgraph_vk_check_memory_budget() trims a quarter of the
         * cache and drains the image pool whenever VMA reports allocation
         * approaching the heap budget. Reactively, a failed vmaCreateImage()
         * drains the pool, flushes frames, evicts up to 64 entries and retries,
         * then flushes the whole cache and retries again. An entry costs
         * nothing until it is actually holding a VkImage.
         *
         * Sizing them too small is not free: Mali has no S3TC support, so every
         * eviction of a compressed texture costs a full software DXT decompress
         * on next use. At 256 entries an 8GB device could not hold a Forza
         * race's working set and re-decoded textures *every frame* --
         * decompress_dxt1/dxt5_block plus write_block_to_texture were ~12.6% of
         * total process CPU. Raising that tier to 1024 removed those symbols
         * from the profile entirely.
         *
         * Keep this monotonic: a 12GB device must never get a smaller cache
         * than an 8GB one (it briefly did).
         */
        if (b.memory_class_gib <= 4) {
            b.texture_cache_entries = 256;
            b.image_pool_max = 16;
            b.surface_image_pool_max = 8;
        } else if (b.memory_class_gib <= 6) {
            b.texture_cache_entries = 512;
            b.image_pool_max = 32;
            b.surface_image_pool_max = 16;
        } else if (b.memory_class_gib <= 8) {
            /* Measured good on a Pixel 10a (8GB). */
            b.texture_cache_entries = 1024;
            b.image_pool_max = 64;
            b.surface_image_pool_max = 32;
        } else if (b.memory_class_gib <= 12) {
            b.texture_cache_entries = 1536;
            b.image_pool_max = 96;
            b.surface_image_pool_max = 48;
        } else {
            b.texture_cache_entries = 2048;
            b.image_pool_max = IMAGE_POOL_MAX_SIZE;
            b.surface_image_pool_max = SURFACE_IMAGE_POOL_MAX_SIZE;
        }
    }

    return b;
}

static const char *const buffer_names[BUFFER_COUNT] = {
    "BUFFER_STAGING_DST",
    "BUFFER_STAGING_SRC",
    "BUFFER_COMPUTE_DST",
    "BUFFER_COMPUTE_SRC",
    "BUFFER_INDEX",
    "BUFFER_INDEX_STAGING",
    "BUFFER_VERTEX_RAM",
    "BUFFER_VERTEX_INLINE",
    "BUFFER_VERTEX_INLINE_STAGING",
    "BUFFER_UNIFORM",
    "BUFFER_UNIFORM_STAGING",
};

/*
 * These buffers used to be the renderer's only upload buffers. Uploads are
 * now owned by FrameStagingState so that CPU writes cannot race GPU work from
 * an older submit frame. get_staging_buffer() consequently redirects every
 * access to the active frame, but keeping the old global allocations around
 * still committed a complete, permanently-unused second copy. That is a very
 * expensive leak on unified-memory Android devices (169 MiB with 64 MiB Xbox
 * RAM and the 4 GiB device caps).
 *
 * Keep their buffer_size metadata below because it is used to size the active
 * frame allocations, but do not create or map backing Vulkan memory for them.
 */
static bool is_per_frame_staging_buffer(int buffer_id)
{
    switch (buffer_id) {
    case BUFFER_STAGING_SRC:
    case BUFFER_INDEX_STAGING:
    case BUFFER_VERTEX_RAM:
    case BUFFER_VERTEX_INLINE_STAGING:
    case BUFFER_UNIFORM_STAGING:
        return true;
    default:
        return false;
    }
}

static bool create_buffer(PGRAPHState *pg, StorageBuffer *buffer,
                          const char *name, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkBufferCreateInfo buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer->buffer_size,
        .usage = buffer->usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = vmaCreateBuffer(r->allocator, &buffer_create_info,
                                      &buffer->alloc_info, &buffer->buffer,
                                      &buffer->allocation, NULL);
    if (result != VK_SUCCESS) {
        error_setg(errp, "Failed to create Vulkan buffer %s (%zu bytes): %d",
                   name, buffer->buffer_size, result);
        return false;
    }
    return true;
}

static void destroy_buffer(PGRAPHState *pg, StorageBuffer *buffer)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (buffer->buffer == VK_NULL_HANDLE && buffer->allocation == VK_NULL_HANDLE) {
        return;
    }
    vmaDestroyBuffer(r->allocator, buffer->buffer, buffer->allocation);
    buffer->buffer = VK_NULL_HANDLE;
    buffer->allocation = VK_NULL_HANDLE;
}

bool pgraph_vk_init_buffers(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    const size_t mib = 1024 * 1024;
    size_t vram_size = memory_region_size(d->vram);

    MemoryBudget mb = compute_memory_budget(r);
    r->shader_module_cache_target = mb.shader_module_cache_entries;
    r->texture_cache_target = mb.texture_cache_entries;
    r->allocation_soft_limit = mb.allocation_soft_limit;
    r->image_pool_max = mb.image_pool_max;
    r->surface_image_pool_max = mb.surface_image_pool_max;

    VK_LOG_ERROR(
        "memory_budget: total_heap=%zuMB mem_class=%uGB budget=%s%zuMB "
        "vtx_inline_cap=%zuMB index_cap=%zuMB staging_cap=%zuMB "
        "uniform_cap=%zuMB pf_vtx=%zuMB pf_idx=%zuMB "
        "pf_uni=%zuMB pf_stg=%zuMB "
        "shader_cache=%zu tex_cache=%zu alloc_soft=%zuMB "
        "img_pool=%d surf_pool=%d",
        mb.total_heap >> 20, mb.memory_class_gib,
        mb.renderer_budget == SIZE_MAX ? "uncapped/" : "",
        mb.renderer_budget == SIZE_MAX ? 0 : mb.renderer_budget >> 20,
        mb.vertex_inline_cap == SIZE_MAX ? 0 : mb.vertex_inline_cap >> 20,
        mb.index_cap == SIZE_MAX ? 0 : mb.index_cap >> 20,
        mb.staging_cap == SIZE_MAX ? 0 : mb.staging_cap >> 20,
        mb.uniform_cap == SIZE_MAX ? 0 : mb.uniform_cap >> 20,
        mb.perframe_vtx_cap == SIZE_MAX ? 0 : mb.perframe_vtx_cap >> 20,
        mb.perframe_idx_cap == SIZE_MAX ? 0 : mb.perframe_idx_cap >> 20,
        mb.perframe_uni_cap == SIZE_MAX ? 0 : mb.perframe_uni_cap >> 20,
        mb.perframe_stg_cap == SIZE_MAX ? 0 : mb.perframe_stg_cap >> 20,
        mb.shader_module_cache_entries, mb.texture_cache_entries,
        mb.allocation_soft_limit == SIZE_MAX ? 0 :
            mb.allocation_soft_limit >> 20,
        mb.image_pool_max, mb.surface_image_pool_max);

    size_t staging_size = vram_size * 2;
    if (staging_size < (32 * mib)) {
        staging_size = 32 * mib;
    }
    staging_size = MIN(staging_size, mb.staging_cap);

    size_t compute_size = vram_size * 2;
    if (compute_size < (64 * mib)) {
        compute_size = 64 * mib;
    }
#ifdef __ANDROID__
    if (compute_size > (64 * mib)) {
        compute_size = 64 * mib;
    }
#else
    if (compute_size > (256 * mib)) {
        compute_size = 256 * mib;
    }
#endif

    size_t index_size = sizeof(pg->inline_elements) * 100;
    index_size = MIN(index_size, mb.index_cap);

    size_t vertex_inline_size = NV2A_VERTEXSHADER_ATTRIBUTES *
                                NV2A_MAX_BATCH_LENGTH *
                                4 * sizeof(float) * 10;
    vertex_inline_size = MIN(vertex_inline_size, mb.vertex_inline_cap);

    VK_LOG("buffer_init: vram=%zu staging=%zu compute=%zu",
           vram_size, staging_size, compute_size);

    VmaAllocationCreateInfo host_alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
    };
    VmaAllocationCreateInfo device_alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .flags = 0,
    };

    r->storage_buffers[BUFFER_STAGING_DST] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .buffer_size = staging_size,
    };

    r->storage_buffers[BUFFER_STAGING_SRC] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = staging_size,
    };

    r->storage_buffers[BUFFER_COMPUTE_DST] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .buffer_size = compute_size,
    };

    r->storage_buffers[BUFFER_COMPUTE_SRC] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .buffer_size = compute_size,
    };

    r->storage_buffers[BUFFER_INDEX] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .buffer_size = index_size,
    };

    r->storage_buffers[BUFFER_INDEX_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = index_size,
    };

    r->storage_buffers[BUFFER_VERTEX_RAM] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .buffer_size = memory_region_size(d->vram),
    };

    r->bitmap_size = memory_region_size(d->vram) / 4096;
    r->uploaded_bitmap = bitmap_new(r->bitmap_size);
    if (!r->uploaded_bitmap) {
        error_setg(errp, "Failed to allocate uploaded surface bitmap");
        return false;
    }
    bitmap_clear(r->uploaded_bitmap, 0, r->bitmap_size);
    r->vertex_ram_flush_min = VK_WHOLE_SIZE;
    r->vertex_ram_flush_max = 0;

    r->storage_buffers[BUFFER_VERTEX_INLINE] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .buffer_size = vertex_inline_size,
    };

    r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = vertex_inline_size,
    };

    extern int xemu_get_submit_frames(void);
    int nframes = xemu_get_submit_frames();

    size_t uniform_size;
    if (nframes >= 3)
        uniform_size = 128 * mib;
    else if (nframes == 2)
        uniform_size = 64 * mib;
    else
        uniform_size = 32 * mib;
    uniform_size = MIN(uniform_size, mb.uniform_cap);

    r->storage_buffers[BUFFER_UNIFORM] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .buffer_size = uniform_size,
    };

    r->storage_buffers[BUFFER_UNIFORM_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = uniform_size,
    };

    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (is_per_frame_staging_buffer(i)) {
            VK_LOG("buffer_init: skip superseded global %s size=%zu",
                   buffer_names[i], r->storage_buffers[i].buffer_size);
            continue;
        }
        VK_LOG("buffer_init: create %s size=%zu",
               buffer_names[i], r->storage_buffers[i].buffer_size);
        if (!create_buffer(pg, &r->storage_buffers[i], buffer_names[i], errp)) {
            VK_LOG_ERROR("buffer_init: create %s FAILED", buffer_names[i]);
            goto fail;
        }
    }

    // FIXME: Add fallback path for device using host mapped memory

    int buffers_to_map[] = { BUFFER_STAGING_DST };

    for (int i = 0; i < ARRAY_SIZE(buffers_to_map); i++) {
        int idx = buffers_to_map[i];
        VK_LOG("buffer_init: map %s", buffer_names[idx]);
        VkResult result = vmaMapMemory(
            r->allocator, r->storage_buffers[idx].allocation,
            (void **)&r->storage_buffers[idx].mapped);
        if (result != VK_SUCCESS) {
            VK_LOG_ERROR("buffer_init: map %s FAILED: %d",
                         buffer_names[idx], result);
            error_setg(errp, "Failed to map Vulkan buffer %s (%zu bytes): %d",
                       buffer_names[idx], r->storage_buffers[idx].buffer_size,
                       result);
            goto fail;
        }
    }

    size_t idx_max, vtx_max, uni_max, stg_max;
    if (nframes >= 3) {
        idx_max = 32 * mib;
        vtx_max = 128 * mib;
        uni_max = 128 * mib;
        stg_max = 256 * mib;
    } else if (nframes == 2) {
        idx_max = 16 * mib;
        vtx_max = 64 * mib;
        uni_max = 64 * mib;
        stg_max = 128 * mib;
    } else {
        idx_max = 8 * mib;
        vtx_max = 32 * mib;
        uni_max = 32 * mib;
        stg_max = 64 * mib;
    }

    idx_max = MIN(idx_max, mb.perframe_idx_cap);
    vtx_max = MIN(vtx_max, mb.perframe_vtx_cap);
    uni_max = MIN(uni_max, mb.perframe_uni_cap);
    stg_max = MIN(stg_max, mb.perframe_stg_cap);

    /*
     * Submit depth is fixed before renderer initialization. Do not reserve
     * full staging and mirrored VRAM buffers for frame slots that cannot be
     * reached during this run.
     */
    for (int i = 0; i < nframes; i++) {
        FrameStagingState *fs = &r->frame_staging[i];

        size_t idx_cap =
            MIN(r->storage_buffers[BUFFER_INDEX].buffer_size, idx_max);
        size_t vtx_cap =
            MIN(r->storage_buffers[BUFFER_VERTEX_INLINE].buffer_size, vtx_max);
        size_t uni_cap =
            MIN(r->storage_buffers[BUFFER_UNIFORM].buffer_size, uni_max);
        size_t stg_cap =
            MIN(r->storage_buffers[BUFFER_STAGING_SRC].buffer_size, stg_max);

        fs->index_staging = (StorageBuffer){
            .alloc_info = host_alloc_create_info,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .buffer_size = idx_cap,
        };
        fs->vertex_inline_staging = (StorageBuffer){
            .alloc_info = host_alloc_create_info,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .buffer_size = vtx_cap,
        };
        fs->uniform_staging = (StorageBuffer){
            .alloc_info = host_alloc_create_info,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .buffer_size = uni_cap,
        };
        fs->staging_src = (StorageBuffer){
            .alloc_info = host_alloc_create_info,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .buffer_size = stg_cap,
        };
        fs->vertex_ram = (StorageBuffer){
            .alloc_info = host_alloc_create_info,
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .buffer_size = memory_region_size(d->vram),
        };
        fs->vertex_ram_flush_min = VK_WHOLE_SIZE;
        fs->vertex_ram_flush_max = 0;
        fs->vertex_ram_propagate_min = VK_WHOLE_SIZE;
        fs->vertex_ram_propagate_max = 0;
        fs->vertex_ram_initialized = false;

        char name[64];
        StorageBuffer *bufs[] = {
            &fs->index_staging, &fs->vertex_inline_staging,
            &fs->uniform_staging, &fs->staging_src, &fs->vertex_ram
        };
        const char *names[] = {
            "INDEX_STAGING", "VTXINLINE_STAGING", "UNIFORM_STAGING",
            "STAGING_SRC", "VERTEX_RAM"
        };
        for (int j = 0; j < ARRAY_SIZE(bufs); j++) {
            snprintf(name, sizeof(name), "FRAME%d_%s", i, names[j]);
            VK_LOG_ERROR("buffer_init: create %s size=%zu", name, bufs[j]->buffer_size);
            if (!create_buffer(pg, bufs[j], name, errp)) {
                goto fail;
            }
            VkResult res = vmaMapMemory(r->allocator, bufs[j]->allocation,
                                        (void **)&bufs[j]->mapped);
            if (res != VK_SUCCESS) {
                error_setg(errp, "Failed to map per-frame buffer %s: %d",
                           name, res);
                goto fail;
            }
        }

        fs->uploaded_bitmap = bitmap_new(r->bitmap_size);
        if (!fs->uploaded_bitmap) {
            error_setg(errp, "Failed to allocate per-frame uploaded bitmap");
            goto fail;
        }
        bitmap_clear(fs->uploaded_bitmap, 0, r->bitmap_size);
    }
    VK_LOG_ERROR("buffer_init: per-frame staging created (%d frames, "
                 "idx=%zuMB vtx=%zuMB uni=%zuMB stg=%zuMB per-frame)",
                 nframes, idx_max >> 20, vtx_max >> 20, uni_max >> 20,
                 stg_max >> 20);

    pgraph_prim_rewrite_init(&r->prim_rewrite_buf);

    r->draw_queue.index_buf = g_malloc0(INDEX_QUEUE_MAX * sizeof(uint32_t));

    return true;

fail:
    for (int i = 0; i < NUM_SUBMIT_FRAMES; i++) {
        FrameStagingState *fs = &r->frame_staging[i];
        StorageBuffer *bufs[] = {
            &fs->index_staging, &fs->vertex_inline_staging,
            &fs->uniform_staging, &fs->staging_src, &fs->vertex_ram
        };
        for (int j = 0; j < ARRAY_SIZE(bufs); j++) {
            if (bufs[j]->mapped) {
                vmaUnmapMemory(r->allocator, bufs[j]->allocation);
                bufs[j]->mapped = NULL;
            }
            destroy_buffer(pg, bufs[j]);
        }
        g_free(fs->uploaded_bitmap);
        fs->uploaded_bitmap = NULL;
    }
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (r->storage_buffers[i].mapped) {
            vmaUnmapMemory(r->allocator, r->storage_buffers[i].allocation);
            r->storage_buffers[i].mapped = NULL;
        }
        destroy_buffer(pg, &r->storage_buffers[i]);
    }
    g_free(r->uploaded_bitmap);
    r->uploaded_bitmap = NULL;
    r->bitmap_size = 0;
    return false;
}

void pgraph_vk_finalize_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < NUM_SUBMIT_FRAMES; i++) {
        FrameStagingState *fs = &r->frame_staging[i];
        StorageBuffer *bufs[] = {
            &fs->index_staging, &fs->vertex_inline_staging,
            &fs->uniform_staging, &fs->staging_src, &fs->vertex_ram
        };
        for (int j = 0; j < ARRAY_SIZE(bufs); j++) {
            if (bufs[j]->mapped) {
                vmaUnmapMemory(r->allocator, bufs[j]->allocation);
            }
            destroy_buffer(pg, bufs[j]);
        }
        g_free(fs->uploaded_bitmap);
        fs->uploaded_bitmap = NULL;
    }

    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (r->storage_buffers[i].mapped) {
            vmaUnmapMemory(r->allocator, r->storage_buffers[i].allocation);
        }
        destroy_buffer(pg, &r->storage_buffers[i]);
    }

    pgraph_prim_rewrite_finalize(&r->prim_rewrite_buf);

    g_free(r->draw_queue.index_buf);
    r->draw_queue.index_buf = NULL;

    g_free(r->uploaded_bitmap);
    r->uploaded_bitmap = NULL;
}

bool pgraph_vk_buffer_has_space_for(PGRAPHState *pg, int index,
                                    VkDeviceSize size,
                                    VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b = get_staging_buffer(r, index);
    return (ROUND_UP(b->buffer_offset, alignment) + size) <= b->buffer_size;
}

VkDeviceSize pgraph_vk_append_to_buffer(PGRAPHState *pg, int index, void **data,
                                        VkDeviceSize *sizes, size_t count,
                                        VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDeviceSize total_size = 0;
    for (int i = 0; i < count; i++) {
        total_size += sizes[i];
    }
    assert(pgraph_vk_buffer_has_space_for(pg, index, total_size, alignment));

    StorageBuffer *b = get_staging_buffer(r, index);
    VkDeviceSize starting_offset = ROUND_UP(b->buffer_offset, alignment);

    assert(b->mapped);

    for (int i = 0; i < count; i++) {
        b->buffer_offset = ROUND_UP(b->buffer_offset, alignment);
        memcpy(b->mapped + b->buffer_offset, data[i], sizes[i]);
        b->buffer_offset += sizes[i];
    }

    return starting_offset;
}

VkDeviceSize pgraph_vk_staging_alloc(PGRAPHState *pg, VkDeviceSize size)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b = get_staging_buffer(r, BUFFER_STAGING_SRC);
    VkDeviceSize offset = ROUND_UP(b->buffer_offset, 16);
    if (offset + size > b->buffer_size) {
        return VK_WHOLE_SIZE;
    }
    b->buffer_offset = offset + size;
    return offset;
}

void pgraph_vk_staging_reset(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    get_staging_buffer(r, BUFFER_STAGING_SRC)->buffer_offset = 0;
}
