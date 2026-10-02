/*
 * MemoryRegion-backed block driver
 *
 * Copyright (c) 2013 espes
 *
 * Based on "Add an in-memory block device" patch
 * Copyright IBM, Corp. 2007
 * Authors:
 * Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "block/block-common.h"
#include "block/block-io.h"
#include "system/memory.h"
#include "qemu/memalign.h"
#include "qobject/qdict.h"
#include "block/block_int.h"
#include "qemu/iov.h"

#include "block/blkmemory.h"

typedef struct BDRVMemoryState {
    uint64_t size;
    AddressSpace *as;
} BDRVMemoryState;

static int memory_open(BlockDriverState *bs, QDict *options, int flags,
                       Error **errp)
{
    return -EINVAL;
}

static void memory_close(BlockDriverState *bs)
{
}

static int64_t coroutine_fn memory_co_getlength(BlockDriverState *bs)
{
    BDRVMemoryState *s = bs->opaque;
    return s->size;
}

static int coroutine_fn memory_co_preadv(BlockDriverState *bs, int64_t offset,
                                         int64_t bytes, QEMUIOVector *qiov,
                                         BdrvRequestFlags flags)
{
    BDRVMemoryState *s = bs->opaque;

    if (offset >= (int64_t)s->size) {
        return 0;
    }
    bytes = MIN(bytes, (int64_t)s->size - offset);

    for (int i = 0; i < qiov->niov; i++) {
        size_t len = qiov->iov[i].iov_len;
        if ((int64_t)len > bytes) {
            len = bytes;
        }

        address_space_read(s->as, offset, MEMTXATTRS_UNSPECIFIED,
                           qiov->iov[i].iov_base, len);

        offset += len;
        bytes -= len;
        if (bytes == 0) {
            break;
        }
    }

    return 0;
}

static int coroutine_fn memory_co_pwritev(BlockDriverState *bs, int64_t offset,
                                          int64_t bytes, QEMUIOVector *qiov,
                                          BdrvRequestFlags flags)
{
    BDRVMemoryState *s = bs->opaque;

    if (offset >= (int64_t)s->size) {
        return -ENOSPC;
    }
    bytes = MIN(bytes, (int64_t)s->size - offset);

    for (int i = 0; i < qiov->niov; i++) {
        size_t len = qiov->iov[i].iov_len;
        if ((int64_t)len > bytes) {
            len = bytes;
        }

        address_space_write(s->as, offset, MEMTXATTRS_UNSPECIFIED,
                            qiov->iov[i].iov_base, len);

        offset += len;
        bytes -= len;
        if (bytes == 0) {
            break;
        }
    }

    return 0;
}

static BlockDriver bdrv_memory = {
    .format_name        = "memory",
    .instance_size      = sizeof(BDRVMemoryState),
    /* Its content is saved by the chihiro-dimm vmstate, not by block
     * snapshots. */
    .snapshots_covered_by_vmstate = true,
    .bdrv_open          = memory_open,
    .bdrv_close         = memory_close,
    .bdrv_co_getlength  = memory_co_getlength,
    .bdrv_co_preadv     = memory_co_preadv,
    .bdrv_co_pwritev    = memory_co_pwritev,
};

static void bdrv_memory_init(void)
{
    bdrv_register(&bdrv_memory);
}

block_init(bdrv_memory_init);

int bdrv_memory_open(BlockDriverState *bs, AddressSpace *as, uint64_t size)
{
    pstrcpy(bs->filename, sizeof(bs->filename), "<mem>");

    bs->drv = &bdrv_memory;
    /* A hand-opened BDS skips bdrv_open_inherit: block queries walk these
     * dicts unconditionally and crash on NULL. */
    bs->options = qdict_new();
    bs->explicit_options = qdict_new();
    bs->open_flags |= BDRV_O_RDWR;
    bs->bl.request_alignment = 512;
    bs->total_sectors = size / BDRV_SECTOR_SIZE;
    bs->opaque = g_malloc0(bdrv_memory.instance_size);

    BDRVMemoryState *s = bs->opaque;
    s->as = as;
    s->size = size;

    return 0;
}
