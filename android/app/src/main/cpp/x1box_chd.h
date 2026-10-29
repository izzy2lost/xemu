/*
 * MAME CHD disc images (chdman createdvd, or createcd of a single data
 * track) read as the plain 2048-byte-sector image they hold, through
 * libchdr. Shared by both cores: xemu's "chd" block driver (block/chd.c)
 * and hakuX's XDVDFS reader (android/hakux/core/native/core/xdvdfs.c).
 *
 * Not thread-safe: the caller serialises reads (xemu with a CoMutex, since
 * its file reads can yield; hakuX with a mutex).
 */
#ifndef X1BOX_CHD_H
#define X1BOX_CHD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define X1BOX_CHD_MAGIC "MComprHD"

typedef struct X1Chd X1Chd;

/* Reads len bytes of the CHD file at off; returns the bytes read, <0 on error. */
typedef int64_t (*X1ChdReadFn)(void *opaque, uint64_t off, void *buf, size_t len);

/* True when head (the file's first bytes) is a CHD's. */
bool x1box_chd_probe(const void *head, size_t len);

/* Opens a CHD of file_size bytes read through fn. NULL on failure, with the
 * reason in err when it is given. */
X1Chd *x1box_chd_open(X1ChdReadFn fn, void *opaque, uint64_t file_size,
                      char *err, size_t err_len);

/* The same over a file descriptor, which stays the caller's. */
X1Chd *x1box_chd_open_fd(int fd, char *err, size_t err_len);

/* The size of the image it holds, in bytes (a multiple of 2048). */
uint64_t x1box_chd_size(const X1Chd *c);

/* Reads len bytes of the image at off; false past its end or on a bad hunk. */
bool x1box_chd_read(X1Chd *c, uint64_t off, void *buf, size_t len);

void x1box_chd_close(X1Chd *c);

#ifdef __cplusplus
}
#endif

#endif
