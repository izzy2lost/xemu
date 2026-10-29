/*
 * MAME CHD disc images through libchdr (x1box_chd.h).
 *
 * A DVD CHD (chdman createdvd) is the image itself in hunks: byte n of the
 * image is byte n of the CHD's data. A CD CHD (chdman createcd) stores
 * 2448-byte frames (2352 of sector, 96 of subcode); the image is the first
 * track's 2048 bytes of user data from each, after a pregap chdman stored
 * ('V' pregap type). Anything else (createhd, createraw) is read linearly.
 */
#include "x1box_chd.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libchdr/cdrom.h>
#include <libchdr/chd.h>

#define SECTOR 2048u
/* Decoded hunks kept, by hunk number modulo this */
#define CACHE_SLOTS 8u
/* libchdr's compressed read-ahead window (fewer, larger reads of the file) */
#define READAHEAD_BYTES (1u << 20)

typedef struct ChdFile {
    X1ChdReadFn fn;
    void *opaque;
    uint64_t size;
    uint64_t pos;
} ChdFile;

struct X1Chd {
    ChdFile file;
    chd_file *chd;
    uint32_t hunk_bytes;
    uint64_t size;
    /* CD: frames of unit_bytes, each with SECTOR bytes at data_off */
    bool cd;
    uint32_t unit_bytes;
    uint32_t data_off;
    uint64_t first_frame;
    uint8_t *slot[CACHE_SLOTS];
    uint32_t slot_hunk[CACHE_SLOTS];
};

static uint64_t cf_fsize(void *p)
{
    return ((ChdFile *)p)->size;
}

static size_t cf_fread(void *buf, size_t size, size_t count, void *p)
{
    ChdFile *f = p;
    uint8_t *dst = buf;
    size_t want = size * count, got = 0;
    while (got < want) {
        int64_t n = f->fn(f->opaque, f->pos, dst + got, want - got);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
        f->pos += (uint64_t)n;
    }
    return size ? got / size : 0;
}

static int cf_fclose(void *p)
{
    (void)p; /* the file is the caller's */
    return 0;
}

static int cf_fseek(void *p, int64_t off, int whence)
{
    ChdFile *f = p;
    int64_t base = whence == SEEK_CUR ? (int64_t)f->pos
                 : whence == SEEK_END ? (int64_t)f->size : 0;
    if (base + off < 0) {
        return -1;
    }
    f->pos = (uint64_t)(base + off);
    return 0;
}

static const core_file_callbacks k_callbacks = {
    cf_fsize, cf_fread, cf_fclose, cf_fseek,
};

bool x1box_chd_probe(const void *head, size_t len)
{
    return len >= 8 && memcmp(head, X1BOX_CHD_MAGIC, 8) == 0;
}

static void set_err(char *err, size_t err_len, const char *fmt, const char *arg)
{
    if (err && err_len) {
        snprintf(err, err_len, fmt, arg);
    }
}

/* The first track of a CD CHD: false when it has no CD metadata. */
static bool cd_layout(X1Chd *c, char *err, size_t err_len, bool *ok)
{
    char meta[256], type[64] = "", pgtype[64] = "";
    uint32_t len = 0;
    int track = 0, frames = 0, pregap = 0;
    *ok = false;
    memset(meta, 0, sizeof(meta));
    if (chd_get_metadata(c->chd, CDROM_TRACK_METADATA2_TAG, 0, meta, sizeof(meta) - 1,
                         &len, NULL, NULL) == CHDERR_NONE) {
        char subtype[64], pgsub[64];
        int postgap = 0;
        if (sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &track, type, subtype, &frames,
                   &pregap, pgtype, pgsub, &postgap) < 4) {
            set_err(err, err_len, "bad CD track metadata: %s", meta);
            return true;
        }
    } else if (chd_get_metadata(c->chd, CDROM_TRACK_METADATA_TAG, 0, meta, sizeof(meta) - 1,
                                &len, NULL, NULL) == CHDERR_NONE) {
        char subtype[64];
        if (sscanf(meta, CDROM_TRACK_METADATA_FORMAT, &track, type, subtype, &frames) < 4) {
            set_err(err, err_len, "bad CD track metadata: %s", meta);
            return true;
        }
    } else {
        return false;
    }

    if (!strcmp(type, "MODE1") || !strcmp(type, "MODE2_FORM1")) {
        c->data_off = 0;
    } else if (!strcmp(type, "MODE1_RAW")) {
        c->data_off = 16;
    } else if (!strcmp(type, "MODE2_RAW")) {
        c->data_off = 24;
    } else {
        set_err(err, err_len, "first track is %s, not data", type);
        return true;
    }
    c->cd = true;
    c->unit_bytes = CD_FRAME_SIZE;
    c->first_frame = pgtype[0] == 'V' && pregap > 0 ? (uint64_t)pregap : 0;
    c->size = (uint64_t)(frames > 0 ? frames : 0) * SECTOR;
    if (c->hunk_bytes % c->unit_bytes) {
        set_err(err, err_len, "%s", "CD hunks are not whole frames");
        return true;
    }
    *ok = true;
    return true;
}

X1Chd *x1box_chd_open(X1ChdReadFn fn, void *opaque, uint64_t file_size,
                      char *err, size_t err_len)
{
    X1Chd *c = calloc(1, sizeof(*c));
    if (!c) {
        set_err(err, err_len, "%s", "out of memory");
        return NULL;
    }
    c->file = (ChdFile){ .fn = fn, .opaque = opaque, .size = file_size };
    chd_error e = chd_open_core_file_callbacks(&k_callbacks, &c->file, CHD_OPEN_READ,
                                               NULL, &c->chd);
    if (e != CHDERR_NONE) {
        set_err(err, err_len, "%s", chd_error_string(e));
        free(c);
        return NULL;
    }
    const chd_header *h = chd_get_header(c->chd);
    c->hunk_bytes = h->hunkbytes;

    bool ok;
    if (cd_layout(c, err, err_len, &ok)) {
        if (!ok) {
            x1box_chd_close(c);
            return NULL;
        }
    } else {
        c->size = h->logicalbytes - h->logicalbytes % SECTOR;
    }
    if (!c->hunk_bytes || !c->size) {
        set_err(err, err_len, "%s", "empty image");
        x1box_chd_close(c);
        return NULL;
    }
    for (unsigned i = 0; i < CACHE_SLOTS; i++) {
        c->slot_hunk[i] = UINT32_MAX;
    }
    chd_set_cache_budget(c->chd, READAHEAD_BYTES);
    return c;
}

static int64_t fd_read(void *opaque, uint64_t off, void *buf, size_t len)
{
    ssize_t n;
    do {
        n = pread(*(int *)opaque, buf, len, (off_t)off);
    } while (n < 0 && errno == EINTR);
    return n;
}

X1Chd *x1box_chd_open_fd(int fd, char *err, size_t err_len)
{
    struct stat st;
    if (fstat(fd, &st) != 0) {
        set_err(err, err_len, "%s", strerror(errno));
        return NULL;
    }
    /* The descriptor lives in the X1Chd's file, where fd_read finds it. */
    int *slot = malloc(sizeof(int));
    if (!slot) {
        return NULL;
    }
    *slot = fd;
    X1Chd *c = x1box_chd_open(fd_read, slot, (uint64_t)st.st_size, err, err_len);
    if (!c) {
        free(slot);
    }
    return c;
}

/* Hunk h, decoded (and cached). */
static const uint8_t *hunk(X1Chd *c, uint32_t h)
{
    unsigned s = h % CACHE_SLOTS;
    if (c->slot_hunk[s] == h) {
        return c->slot[s];
    }
    if (!c->slot[s] && !(c->slot[s] = malloc(c->hunk_bytes))) {
        return NULL;
    }
    c->slot_hunk[s] = UINT32_MAX;
    if (chd_read(c->chd, h, c->slot[s]) != CHDERR_NONE) {
        return NULL;
    }
    c->slot_hunk[s] = h;
    return c->slot[s];
}

bool x1box_chd_read(X1Chd *c, uint64_t off, void *buf, size_t len)
{
    uint8_t *dst = buf;
    if (off > c->size || len > c->size - off) {
        return false;
    }
    while (len) {
        uint64_t raw;   /* the byte's place in the CHD's data */
        size_t n;
        if (c->cd) {
            uint64_t frame = c->first_frame + off / SECTOR;
            uint32_t in = (uint32_t)(off % SECTOR);
            raw = frame * c->unit_bytes + c->data_off + in;
            n = SECTOR - in;
        } else {
            raw = off;
            n = c->hunk_bytes - (size_t)(raw % c->hunk_bytes);
        }
        if (n > len) {
            n = len;
        }
        const uint8_t *src = hunk(c, (uint32_t)(raw / c->hunk_bytes));
        if (!src) {
            return false;
        }
        memcpy(dst, src + raw % c->hunk_bytes, n);
        dst += n;
        off += n;
        len -= n;
    }
    return true;
}

uint64_t x1box_chd_size(const X1Chd *c)
{
    return c->size;
}

void x1box_chd_close(X1Chd *c)
{
    if (!c) {
        return;
    }
    if (c->chd) {
        chd_close(c->chd);
    }
    if (c->file.fn == fd_read) {
        free(c->file.opaque);
    }
    for (unsigned i = 0; i < CACHE_SLOTS; i++) {
        free(c->slot[i]);
    }
    free(c);
}
