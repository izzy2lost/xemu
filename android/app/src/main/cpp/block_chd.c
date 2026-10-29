/*
 * "chd" block driver: a MAME CHD disc image (x1box_chd.h) in the DVD drive,
 * read-only. The image it holds is what the drive sees, so a CHD of an
 * XISO or of a full disc dump works like that ISO. Probed by its magic, so
 * the -drive without a format (system/vl.c) picks it.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "block/block-io.h"
#include "block/block_int.h"
#include "qemu/module.h"

#include "x1box_chd.h"

typedef struct BDRVChdState {
    CoMutex lock;
    X1Chd *chd;
} BDRVChdState;

static int chd_probe(const uint8_t *buf, int buf_size, const char *filename)
{
    return x1box_chd_probe(buf, buf_size > 0 ? (size_t)buf_size : 0) ? 100 : 0;
}

/* libchdr's reads of the file: from the coroutine of a request, or the main
 * loop at open (bdrv_pread works in either). Both hold the graph read lock;
 * libchdr's callback type can't say so. */
static int64_t TSA_NO_TSA chd_file_read(void *opaque, uint64_t off, void *buf,
                                        size_t len)
{
    BlockDriverState *bs = opaque;
    int ret = bdrv_pread(bs->file, off, len, buf, 0);
    return ret < 0 ? ret : (int64_t)len;
}

static int chd_open(BlockDriverState *bs, QDict *options, int flags,
                    Error **errp)
{
    BDRVChdState *s = bs->opaque;
    char err[256] = "";
    int64_t size;
    int ret;

    GLOBAL_STATE_CODE();

    bdrv_graph_rdlock_main_loop();
    ret = bdrv_apply_auto_read_only(bs, NULL, errp);
    bdrv_graph_rdunlock_main_loop();
    if (ret < 0) {
        return ret;
    }

    ret = bdrv_open_file_child(NULL, options, "file", bs, errp);
    if (ret < 0) {
        return ret;
    }

    GRAPH_RDLOCK_GUARD_MAINLOOP();

    size = bdrv_getlength(bs->file->bs);
    if (size < 0) {
        error_setg_errno(errp, -size, "can't get the CHD's size");
        return size;
    }
    s->chd = x1box_chd_open(chd_file_read, bs, size, err, sizeof(err));
    if (!s->chd) {
        error_setg(errp, "can't open the CHD: %s", err);
        return -EINVAL;
    }
    qemu_co_mutex_init(&s->lock);
    return 0;
}

static void chd_refresh_limits(BlockDriverState *bs, Error **errp)
{
    bs->bl.request_alignment = BDRV_SECTOR_SIZE;
}

static int64_t coroutine_fn GRAPH_RDLOCK chd_co_getlength(BlockDriverState *bs)
{
    BDRVChdState *s = bs->opaque;
    return x1box_chd_size(s->chd);
}

static int coroutine_fn GRAPH_RDLOCK
chd_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
              QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVChdState *s = bs->opaque;
    uint8_t buf[16384];
    int64_t done = 0;
    int ret = 0;

    qemu_co_mutex_lock(&s->lock);
    while (done < bytes) {
        size_t n = MIN(sizeof(buf), (size_t)(bytes - done));
        if (!x1box_chd_read(s->chd, offset + done, buf, n)) {
            ret = -EIO;
            break;
        }
        qemu_iovec_from_buf(qiov, done, buf, n);
        done += n;
    }
    qemu_co_mutex_unlock(&s->lock);
    return ret;
}

static void chd_close(BlockDriverState *bs)
{
    BDRVChdState *s = bs->opaque;
    x1box_chd_close(s->chd);
    s->chd = NULL;
}

static BlockDriver bdrv_chd = {
    .format_name         = "chd",
    .instance_size       = sizeof(BDRVChdState),
    .bdrv_probe          = chd_probe,
    .bdrv_open           = chd_open,
    .bdrv_child_perm     = bdrv_default_perms,
    .bdrv_refresh_limits = chd_refresh_limits,
    .bdrv_co_getlength   = chd_co_getlength,
    .bdrv_co_preadv      = chd_co_preadv,
    .bdrv_close          = chd_close,
    .is_format           = true,
};

static void bdrv_chd_init(void)
{
    bdrv_register(&bdrv_chd);
}

block_init(bdrv_chd_init);
