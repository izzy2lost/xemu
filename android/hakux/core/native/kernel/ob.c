/*
 * hakuX native core — objects and handles.
 *
 * Objects live in guest memory as on the Xbox: an OBJECT_HEADER
 * (PointerCount, HandleCount, Type, Flags) followed by the body; handles
 * (multiples of 4) map to the body. Type is the VA of the kernel's type
 * object (PsThreadObjectType, ExEventObjectType, ...).
 */
#include "native/kernel/kernel.h"

#include <strings.h>

#define OB_HEADER 0x10u

/* Body size of each type's objects, for freeing them: file and symbolic-link
 * objects, all one size, whose host state goes with them (nc_io_close). Other
 * types stay: the scheduler, waits and timers keep their addresses. */
static uint32_t g_ob_size[NC_OB_TYPES];

static bool freeable(enum NcObType type)
{
    return type == NC_OB_FILE || type == NC_OB_SYMLINK;
}

static enum NcObType type_of(NcKernel *k, uint32_t object)
{
    const uint32_t tva = nc_vm_rd32(k->vm, object - OB_HEADER + 0x8);
    for (unsigned t = 0; t < NC_OB_TYPES; t++) {
        if (k->type_va[t] == tva) {
            return (enum NcObType)t;
        }
    }
    return NC_OB_TYPES;
}

/* Frees an object no handle or reference holds any more (PointerCount 1 is
 * the one it was created with). */
static void release(NcKernel *k, uint32_t object, enum NcObType type)
{
    const uint32_t hdr = object - OB_HEADER;
    if (type >= NC_OB_TYPES || !freeable(type) || !g_ob_size[type] || g_ob_size[type] == UINT32_MAX ||
        nc_vm_rd32(k->vm, hdr + 0x4) ||
        nc_vm_rd32(k->vm, hdr) > 1) {
        return;
    }
    nc_vm_wr32(k->vm, hdr + 0x8, 0);    /* a stale lookup of it fails the type check */
    k->pool_ob -= OB_HEADER + g_ob_size[type];
    nc_pool_free(k, hdr, OB_HEADER + g_ob_size[type]);
}

void nc_ob_dereference(NcKernel *k, uint32_t object)
{
    const uint32_t hdr = object - OB_HEADER;
    const uint32_t count = nc_vm_rd32(k->vm, hdr);
    if (count) {
        nc_vm_wr32(k->vm, hdr, count - 1);
    }
    release(k, object, type_of(k, object));
}

uint32_t nc_ob_create(NcKernel *k, enum NcObType type, uint32_t body_size)
{
    if (freeable(type) && g_ob_size[type] != UINT32_MAX) {
        if (g_ob_size[type] && g_ob_size[type] != body_size) {
            nc_log("hakuX-native", "ob: type %u objects of %u and %u bytes: not freed", type, g_ob_size[type],
                   body_size);
            g_ob_size[type] = UINT32_MAX;   /* for good: the sizes are mixed */
        } else {
            g_ob_size[type] = body_size;
        }
    }
    uint32_t h = nc_pool_alloc(k, OB_HEADER + body_size);
    if (!h) {
        return 0;
    }
    k->pool_ob += OB_HEADER + body_size;
    nc_vm_wr32(k->vm, h + 0x0, 1);              /* PointerCount */
    nc_vm_wr32(k->vm, h + 0x8, k->type_va[type]); /* Type */
    return h + OB_HEADER;
}

uint32_t nc_ob_handle(NcKernel *k, uint32_t object, enum NcObType type)
{
    for (unsigned i = 1; i < NC_MAX_HANDLES; i++) {
        if (!k->handle_table[i].object) {
            k->handle_table[i] = (NcHandle){object, type};
            uint32_t hdr = object - OB_HEADER;
            nc_vm_wr32(k->vm, hdr + 0x4, nc_vm_rd32(k->vm, hdr + 0x4) + 1); /* HandleCount */
            return i * 4;
        }
    }
    nc_log("hakuX-native", "ob: out of handles");
    return 0;
}

uint32_t nc_ob_lookup(NcKernel *k, uint32_t handle, enum NcObType *type)
{
    uint32_t i = handle / 4;
    if (handle & 3 || !i || i >= NC_MAX_HANDLES || !k->handle_table[i].object) {
        return 0;
    }
    if (type) {
        *type = k->handle_table[i].type;
    }
    return k->handle_table[i].object;
}

/* Closes a handle; a file's host state goes with its last handle. */
bool nc_ob_close(NcKernel *k, uint32_t handle)
{
    uint32_t i = handle / 4;
    if (handle & 3 || !i || i >= NC_MAX_HANDLES || !k->handle_table[i].object) {
        return false;
    }
    const uint32_t object = k->handle_table[i].object;
    const enum NcObType type = k->handle_table[i].type;
    const uint32_t hdr = object - OB_HEADER;
    const uint32_t count = nc_vm_rd32(k->vm, hdr + 0x4) - 1;
    nc_vm_wr32(k->vm, hdr + 0x4, count);
    k->handle_table[i].object = 0;
    if (!count && (type == NC_OB_FILE || type == NC_OB_SYMLINK)) {
        nc_io_close(k, object);
    }
    if (!count) {
        release(k, object, type);
    }
    return true;
}

/* Named objects, all in guest memory so savestates keep them: a word of the
 * kernel's area (between the export table and the stubs) heads a chain of
 * records in the pool, each { next record, object, name length, name }.
 * Named objects are never freed (freeable), so a record never dangles. */
#define NAMES_HEAD (NC_KERNEL_VA + 0x0EF0u)
#define NAME_MAX_LEN 128u

/* The name of OBJECT_ATTRIBUTES attrs (its ObjectName, an OBJECT_STRING), or false. */
static bool attrs_name(NcKernel *k, uint32_t attrs, char *out, uint32_t *len)
{
    const uint32_t str = attrs ? nc_vm_rd32(k->vm, attrs + 4) : 0;
    if (!str) {
        return false;
    }
    *len = *(uint16_t *)nc_vm_host(k->vm, str);
    if (!*len || *len > NAME_MAX_LEN) {
        return false;
    }
    memcpy(out, nc_vm_host(k->vm, nc_vm_rd32(k->vm, str + 4)), *len);
    out[*len] = 0;
    return true;
}

static uint32_t find_named(NcKernel *k, const char *name, uint32_t len)
{
    for (uint32_t r = nc_vm_rd32(k->vm, NAMES_HEAD); r; r = nc_vm_rd32(k->vm, r)) {
        if (nc_vm_rd32(k->vm, r + 8) == len &&
            !strncasecmp(nc_vm_host(k->vm, r + 12), name, len)) {
            return nc_vm_rd32(k->vm, r + 4);
        }
    }
    return 0;
}

uint32_t nc_ob_find_named(NcKernel *k, const char *name, enum NcObType *type)
{
    const uint32_t obj = find_named(k, name, (uint32_t)strlen(name));
    if (obj && type) {
        *type = type_of(k, obj);
    }
    return obj;
}

uint32_t nc_ob_create_named(NcKernel *k, enum NcObType type, uint32_t body_size, uint32_t attrs,
                            uint32_t *status)
{
    char name[NAME_MAX_LEN + 1];
    uint32_t len;
    *status = NC_STATUS_SUCCESS;
    if (!attrs_name(k, attrs, name, &len)) {
        return nc_ob_create(k, type, body_size);
    }
    const uint32_t old = find_named(k, name, len);
    if (old) {
        *status = type_of(k, old) == type ? NC_STATUS_OBJECT_NAME_EXISTS : NC_STATUS_OBJECT_TYPE_MISMATCH;
        return *status == NC_STATUS_OBJECT_NAME_EXISTS ? old : 0;
    }
    const uint32_t obj = nc_ob_create(k, type, body_size);
    const uint32_t r = obj ? nc_pool_alloc(k, 12 + len) : 0;
    if (!r) {
        return obj;     /* unnamed, then: nothing finds it by name */
    }
    nc_vm_wr32(k->vm, r, nc_vm_rd32(k->vm, NAMES_HEAD));
    nc_vm_wr32(k->vm, r + 4, obj);
    nc_vm_wr32(k->vm, r + 8, len);
    memcpy(nc_vm_host(k->vm, r + 12), name, len);
    nc_vm_wr32(k->vm, NAMES_HEAD, r);
    nc_log("hakuX-native", "ob: %s named %s", type == NC_OB_EVENT ? "event" : type == NC_OB_SEMAPHORE ? "semaphore"
                                              : type == NC_OB_MUTANT ? "mutant" : "timer", name);
    return obj;
}

/* NtClose(Handle): a file or symbolic link goes with its last handle and
 * reference (release). */
static uint64_t NtClose(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return nc_ob_close(k, a[0]) ? NC_STATUS_SUCCESS : NC_STATUS_INVALID_HANDLE;
}

/* NtDuplicateObject(SourceHandle, TargetHandle, Options): DUPLICATE_CLOSE_SOURCE is 1.
 * NtCurrentThread (-2) duplicates to a real handle of the running thread. */
static uint64_t NtDuplicateObject(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    enum NcObType type = NC_OB_THREAD;
    uint32_t obj = a[0] == 0xFFFFFFFEu && k->current ? k->current->ethread : nc_ob_lookup(k, a[0], &type);
    if (!obj) {
        return NC_STATUS_INVALID_HANDLE;
    }
    uint32_t h = nc_ob_handle(k, obj, type);
    if (!h) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    nc_vm_wr32(k->vm, a[1], h);
    if ((a[2] & 1) && a[0] != 0xFFFFFFFEu) {
        nc_ob_close(k, a[0]);
    }
    return NC_STATUS_SUCCESS;
}

void nc_ob_register(NcKernel *k)
{
    nc_kernel_impl(k, "NtClose", NtClose);
    nc_kernel_impl(k, "NtDuplicateObject", NtDuplicateObject);
}
