/*
 * hakuX native core — XDVDFS reader.
 *
 * Volume descriptor at sector 32 of the game partition: "MICROSOFT*XBOX*MEDIA",
 * root directory sector and size. A directory is a binary tree of entries:
 *   u16 left, u16 right   (child offsets in dwords; 0 = none: the root is
 *                          nobody's child; 0xFFFF pads a sector's end)
 *   u32 sector, u32 size
 *   u8 attributes, u8 name length, name
 */
#include "native/core/xdvdfs.h"

#include "x1box_chd.h"

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static const char k_magic[] = "MICROSOFT*XBOX*MEDIA";

/* Game partition offsets: extracted XISO, then full XGD1/XGD2/XGD3 dumps. */
static const uint64_t k_part_offsets[] = {0, 0x18300000ull, 0xFD90000ull, 0x2080000ull};

static bool pread_all(int fd, void *buf, size_t len, uint64_t off)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t n = pread(fd, p, len, (off_t)off);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= (size_t)n;
        off += (uint64_t)n;
    }
    return true;
}

/* libchdr decodes one read at a time: the lock serialises the core's threads. */
struct NcDiscChd {
    X1Chd *chd;
    pthread_mutex_t lock;
};

bool nc_disc_image_read(const NcDisc *d, uint64_t off, void *buf, size_t len)
{
    if (!d->chd) {
        return pread_all(d->fd, buf, len, off);
    }
    pthread_mutex_lock(&d->chd->lock);
    const bool ok = x1box_chd_read(d->chd->chd, off, buf, len);
    pthread_mutex_unlock(&d->chd->lock);
    return ok;
}

uint64_t nc_disc_image_size(const NcDisc *d)
{
    struct stat st;
    return d->chd ? x1box_chd_size(d->chd->chd)
                  : fstat(d->fd, &st) == 0 ? (uint64_t)st.st_size : 0;
}

static void close_chd(NcDisc *d)
{
    if (d->chd) {
        x1box_chd_close(d->chd->chd);
        pthread_mutex_destroy(&d->chd->lock);
        free(d->chd);
        d->chd = NULL;
    }
}

static bool open_chd(NcDisc *d)
{
    uint8_t head[8];
    if (!pread_all(d->fd, head, sizeof(head), 0) || !x1box_chd_probe(head, sizeof(head))) {
        return true;    /* not a CHD: the image itself */
    }
    char err[256] = "";
    X1Chd *chd = x1box_chd_open_fd(d->fd, err, sizeof(err));
    if (!chd || !(d->chd = calloc(1, sizeof(*d->chd)))) {
        nc_log("hakuX-native", "disc: can't open the CHD: %s", err[0] ? err : "out of memory");
        x1box_chd_close(chd);
        return false;
    }
    d->chd->chd = chd;
    pthread_mutex_init(&d->chd->lock, NULL);
    nc_log("hakuX-native", "disc: CHD holding a %llu-byte image",
           (unsigned long long)x1box_chd_size(chd));
    return true;
}

bool nc_disc_open(NcDisc *d, int fd)
{
    memset(d, 0, sizeof(*d));
    d->fd = fd;
    if (!open_chd(d)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_part_offsets) / sizeof(k_part_offsets[0]); i++) {
        uint8_t vd[NC_XDVDFS_SECTOR];
        uint64_t off = k_part_offsets[i];
        if (!nc_disc_image_read(d, off + 32ull * NC_XDVDFS_SECTOR, vd, sizeof(vd))) {
            continue;
        }
        if (memcmp(vd, k_magic, 20) == 0 && memcmp(vd + 0x7EC, k_magic, 20) == 0) {
            d->part_off = off;
            d->root_sector = nc_rd32(vd + 20);
            d->root_size = nc_rd32(vd + 24);
            nc_log("hakuX-native", "disc: XDVDFS at +0x%llx, root sector %u (%u bytes)",
                   (unsigned long long)off, d->root_sector, d->root_size);
            return true;
        }
    }
    nc_log("hakuX-native", "disc: no XDVDFS volume found");
    close_chd(d);   /* the caller closes the descriptor */
    return false;
}

void nc_disc_close(NcDisc *d)
{
    close_chd(d);
    if (d->fd >= 0) {
        close(d->fd);
    }
    d->fd = -1;
}

static bool name_eq(const uint8_t *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen) {
        return false;
    }
    for (size_t i = 0; i < alen; i++) {
        if (toupper(a[i]) != toupper((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

/* Depth-first over the whole tree: robust against any ordering differences.
 * Iterative, with no depth cap: authoring tools can leave a directory's tree
 * as one long chain (Conker's dvddata\Audio is 78 entries, 77 deep). Every
 * entry takes at least 16 bytes, which bounds a well-formed walk; a looping
 * one stops there too. */
static bool dir_search(const uint8_t *dir, uint32_t size, uint32_t off, const char *name,
                       size_t namelen, NcDiscFile *out)
{
    const uint32_t cap = size / 16 + 1;
    uint32_t *stack = malloc(cap * 2 * sizeof(*stack));
    if (!stack) {
        return false;
    }
    uint32_t n = 0, visits = 0;
    bool found = false;
    stack[n++] = off;
    while (n && !found && visits++ < cap) {
        off = stack[--n];
        if (off + 14 > size) {
            continue;
        }
        const uint8_t *e = dir + off;
        uint16_t left = nc_rd16(e), right = nc_rd16(e + 2);
        if (left == 0xFFFF && right == 0xFFFF && e[12] == 0xFF) {
            continue; /* padding */
        }
        uint8_t nlen = e[13];
        if (off + 14 + nlen <= size && name_eq(e + 14, nlen, name, namelen)) {
            out->sector = nc_rd32(e + 4);
            out->size = nc_rd32(e + 8);
            out->attr = e[12];
            found = true;
            break;
        }
        if (right && right != 0xFFFF) {
            stack[n++] = right * 4u;
        }
        if (left && left != 0xFFFF) {
            stack[n++] = left * 4u;
        }
    }
    free(stack);
    return found;
}

/* the disc's own files */
static bool disc_find(const NcDisc *d, const char *path, NcDiscFile *out)
{
    NcDiscFile cur = {d->root_sector, d->root_size, 0x10};
    const char *p = path;
    while (*p) {
        while (*p == '\\' || *p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *end = p;
        while (*end && *end != '\\' && *end != '/') {
            end++;
        }
        if (!(cur.attr & 0x10) || !cur.size) {
            return false;
        }
        uint8_t *dir = malloc(cur.size);
        if (!dir) {
            return false;
        }
        bool ok = nc_disc_image_read(d, d->part_off + (uint64_t)cur.sector * NC_XDVDFS_SECTOR,
                                     dir, cur.size) &&
                  dir_search(dir, cur.size, 0, p, (size_t)(end - p), &cur);
        free(dir);
        if (!ok) {
            return false;
        }
        p = end;
    }
    *out = cur;
    return true;
}

/* ---- the overlay (mods) ----
 * Entries keyed by their path, normalized: no leading slash, backslashes,
 * lower case. Added in the mods' order, then sorted (the later of two at a
 * path stays); a file's descriptor opens at its first read. */
typedef struct OvEntry {
    char path[256];             /* normalized: the key */
    char shown[256];            /* as the mod has it (listings) */
    char *host;                 /* NULL: a folder */
    uint32_t size;
    unsigned seq;               /* the order added: the later wins */
    int fd;                     /* atomic: -1 until the first read */
} OvEntry;

typedef struct OvPatch {
    uint32_t sector;            /* the file it patches (its NcDiscFile) */
    uint64_t off;
    uint32_t len;
    uint8_t *bytes;
} OvPatch;

struct NcDiscOverlay {
    OvEntry *e;
    unsigned n, cap;
    bool sealed;
    OvPatch *p;
    unsigned np, pcap;
};

static void norm_case(const char *in, char *out, size_t cap, bool lower)
{
    size_t n = 0;
    while (*in == '\\' || *in == '/') {
        in++;
    }
    for (; *in && n + 1 < cap; in++) {
        char c = *in == '/' ? '\\' : lower ? (char)tolower((unsigned char)*in) : *in;
        if (c == '\\' && n && out[n - 1] == '\\') {
            continue;
        }
        out[n++] = c;
    }
    while (n && out[n - 1] == '\\') {
        n--;
    }
    out[n] = 0;
}

static void norm_path(const char *in, char *out, size_t cap)
{
    norm_case(in, out, cap, true);
}

static NcDiscOverlay *ov_of(NcDisc *d)
{
    if (!d->ov) {
        d->ov = calloc(1, sizeof(*d->ov));
    }
    return d->ov;
}

/* shown: the path as the mod has it (normalized but its case) */
static OvEntry *ov_push(NcDiscOverlay *o, const char *path, const char *shown)
{
    if (o->n == o->cap) {
        unsigned cap = o->cap ? o->cap * 2 : 64;
        OvEntry *e = realloc(o->e, cap * sizeof(*e));
        if (!e) {
            return NULL;
        }
        o->e = e;
        o->cap = cap;
    }
    OvEntry *e = &o->e[o->n];
    memset(e, 0, sizeof(*e));
    snprintf(e->path, sizeof(e->path), "%s", path);
    snprintf(e->shown, sizeof(e->shown), "%s", shown);
    e->seq = o->n++;
    e->fd = -1;
    return e;
}

bool nc_disc_overlay_file(NcDisc *d, const char *path, const char *host)
{
    NcDiscOverlay *o = ov_of(d);
    char p[256], shown[256];
    norm_path(path, p, sizeof(p));
    norm_case(path, shown, sizeof(shown), false);   /* the same length: only the case differs */
    struct stat st;
    if (!o || o->sealed || !*p || stat(host, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 0xFFFFFFFFll) {
        return false;
    }
    OvEntry *e = ov_push(o, p, shown);
    if (!e || !(e->host = strdup(host))) {
        return false;
    }
    e->size = (uint32_t)st.st_size;
    /* the folders on its way the disc doesn't have */
    for (char *s = strchr(p, '\\'); s; s = strchr(s + 1, '\\')) {
        const size_t at = (size_t)(s - p);
        *s = 0;
        shown[at] = 0;
        NcDiscFile f;
        if (!disc_find(d, p, &f)) {
            ov_push(o, p, shown);
        } else if (!(f.attr & 0x10)) {
            nc_log("hakuX-native-mods", "%s: %s is a file on the disc", path, p);
        }
        *s = '\\';
        shown[at] = '\\';
    }
    return true;
}

static int ov_cmp(const void *a, const void *b)
{
    const OvEntry *x = a, *y = b;
    const int c = strcmp(x->path, y->path);
    return c ? c : (x->seq > y->seq) - (x->seq < y->seq);
}

void nc_disc_overlay_seal(NcDisc *d)
{
    NcDiscOverlay *o = d->ov;
    if (!o || o->sealed) {
        return;
    }
    qsort(o->e, o->n, sizeof(*o->e), ov_cmp);
    unsigned n = 0;
    for (unsigned i = 0; i < o->n; i++) {
        /* of the entries at a path the last added stays; a file over a folder */
        if (n && !strcmp(o->e[n - 1].path, o->e[i].path)) {
            if (!o->e[i].host && o->e[n - 1].host) {
                continue;
            }
            free(o->e[n - 1].host);
            o->e[n - 1] = o->e[i];
            continue;
        }
        o->e[n++] = o->e[i];
    }
    o->n = n;
    o->sealed = true;
}

static const OvEntry *ov_find(const NcDiscOverlay *o, const char *norm)
{
    if (!o || !o->sealed) {
        return NULL;
    }
    unsigned lo = 0, hi = o->n;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        const int c = strcmp(o->e[mid].path, norm);
        if (!c) {
            return &o->e[mid];
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

bool nc_disc_find(const NcDisc *d, const char *path, NcDiscFile *out)
{
    if (d->ov) {
        char p[256];
        norm_path(path, p, sizeof(p));
        const OvEntry *e = ov_find(d->ov, p);
        if (e) {
            out->sector = NC_DISC_OVERLAY + (uint32_t)(e - d->ov->e);
            out->size = e->size;
            out->attr = e->host ? 0x01 : 0x10;      /* read-only, or a folder */
            return true;
        }
    }
    return disc_find(d, path, out);
}

bool nc_disc_overlay_patch(NcDisc *d, const char *path, uint64_t off, const uint8_t *bytes, uint32_t len)
{
    NcDiscOverlay *o = ov_of(d);
    NcDiscFile f;
    if (!o || !len || !nc_disc_find(d, path, &f) || (f.attr & 0x10) || off + len > f.size) {
        return false;
    }
    if (o->np == o->pcap) {
        unsigned cap = o->pcap ? o->pcap * 2 : 16;
        OvPatch *p = realloc(o->p, cap * sizeof(*p));
        if (!p) {
            return false;
        }
        o->p = p;
        o->pcap = cap;
    }
    OvPatch *p = &o->p[o->np];
    if (!(p->bytes = malloc(len))) {
        return false;
    }
    memcpy(p->bytes, bytes, len);
    p->sector = f.sector;
    p->off = off;
    p->len = len;
    o->np++;
    return true;
}

void nc_disc_overlay_list(const NcDisc *d, const char *dir,
                          void (*fn)(void *ctx, const char *name, uint32_t size, bool is_dir), void *ctx)
{
    const NcDiscOverlay *o = d->ov;
    if (!o || !o->sealed) {
        return;
    }
    char p[256];
    norm_path(dir, p, sizeof(p));
    const size_t n = strlen(p);
    for (unsigned i = 0; i < o->n; i++) {
        const char *e = o->e[i].path;
        if (n && (strncmp(e, p, n) || e[n] != '\\')) {
            continue;
        }
        const size_t at = n ? n + 1 : 0;
        if (e[at] && !strchr(e + at, '\\')) {
            fn(ctx, o->e[i].shown + at, o->e[i].size, !o->e[i].host);
        }
    }
}

static bool ov_read(const NcDisc *d, const NcDiscFile *f, uint64_t off, void *buf, size_t len)
{
    const NcDiscOverlay *o = d->ov;
    const uint32_t i = f->sector - NC_DISC_OVERLAY;
    if (!o || i >= o->n || !o->e[i].host) {
        return false;
    }
    OvEntry *e = &o->e[i];
    int fd = __atomic_load_n(&e->fd, __ATOMIC_ACQUIRE);
    if (fd < 0) {
        int mine = open(e->host, O_RDONLY | O_CLOEXEC), none = -1;
        if (mine < 0) {
            nc_log("hakuX-native-mods", "%s: can't open %s", e->path, e->host);
            return false;
        }
        if (!__atomic_compare_exchange_n(&e->fd, &none, mine, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            close(mine);                /* another thread's came first */
        }
        fd = __atomic_load_n(&e->fd, __ATOMIC_ACQUIRE);
    }
    return pread_all(fd, buf, len, off);
}

bool nc_disc_find_orig(const NcDisc *d, const char *path, NcDiscFile *out)
{
    return disc_find(d, path, out);
}

bool nc_disc_read_orig(const NcDisc *d, const NcDiscFile *f, uint64_t off, void *buf, size_t len)
{
    return f->sector < NC_DISC_OVERLAY && off + len <= f->size &&
           nc_disc_image_read(d, d->part_off + (uint64_t)f->sector * NC_XDVDFS_SECTOR + off, buf, len);
}

bool nc_disc_read(const NcDisc *d, const NcDiscFile *f, uint64_t off, void *buf, size_t len)
{
    if (off + len > f->size) {
        return false;
    }
    const bool ok = f->sector >= NC_DISC_OVERLAY
                        ? ov_read(d, f, off, buf, len)
                        : nc_disc_image_read(d, d->part_off + (uint64_t)f->sector * NC_XDVDFS_SECTOR + off, buf, len);
    if (ok && d->ov) {
        for (unsigned i = 0; i < d->ov->np; i++) {
            const OvPatch *p = &d->ov->p[i];
            if (p->sector != f->sector || p->off >= off + len || p->off + p->len <= off) {
                continue;
            }
            const uint64_t a = p->off > off ? p->off : off;
            const uint64_t b = p->off + p->len < off + len ? p->off + p->len : off + len;
            memcpy((uint8_t *)buf + (a - off), p->bytes + (a - p->off), (size_t)(b - a));
        }
    }
    return ok;
}
