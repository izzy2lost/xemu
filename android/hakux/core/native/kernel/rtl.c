/*
 * hakuX native core — runtime library exports.
 *
 * ANSI_STRING / UNICODE_STRING: { USHORT Length, USHORT MaximumLength,
 * Buffer }, lengths in bytes. TIME_FIELDS: Year, Month, Day, Hour, Minute,
 * Second, Milliseconds, Weekday (CSHORT each). Times are FILETIMEs.
 */
#include "native/kernel/kernel.h"
#include "native/kernel/rtl_case.h"

#include <ctype.h>

static uint16_t str_len(NcKernel *k, uint32_t s)
{
    return *(uint16_t *)nc_vm_host(k->vm, s);
}

static uint16_t str_max(NcKernel *k, uint32_t s)
{
    return *(uint16_t *)nc_vm_host(k->vm, s + 2);
}

static uint32_t str_buf(NcKernel *k, uint32_t s)
{
    return nc_vm_rd32(k->vm, s + 4);
}

static void str_set(NcKernel *k, uint32_t s, uint16_t len, uint16_t max, uint32_t buf)
{
    *(uint16_t *)nc_vm_host(k->vm, s) = len;
    *(uint16_t *)nc_vm_host(k->vm, s + 2) = max;
    nc_vm_wr32(k->vm, s + 4, buf);
}

/* Kernel pool blocks for strings, freed by RtlFree*String (same header as ExAllocatePool). */
static uint32_t str_alloc(NcKernel *k, uint32_t size)
{
    uint32_t p = nc_pool_alloc(k, size + 16);
    if (!p) {
        return 0;
    }
    nc_vm_wr32(k->vm, p, size);
    return p + 16;
}

static void str_free(NcKernel *k, uint32_t buf)
{
    if (buf) {
        nc_pool_free(k, buf - 16, nc_vm_rd32(k->vm, buf - 16) + 16);
    }
}

/* RtlInitAnsiString(DestinationString, SourceString) */
static uint64_t RtlInitAnsiString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint16_t len = a[1] ? (uint16_t)strlen((const char *)nc_vm_host(k->vm, a[1])) : 0;
    str_set(k, a[0], len, a[1] ? len + 1 : 0, a[1]);
    return 0;
}

/* RtlInitUnicodeString(DestinationString, SourceString) */
static uint64_t RtlInitUnicodeString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint16_t len = 0;
    if (a[1]) {
        const uint16_t *w = nc_vm_host(k->vm, a[1]);
        while (w[len / 2]) {
            len += 2;
        }
    }
    str_set(k, a[0], len, a[1] ? len + 2 : 0, a[1]);
    return 0;
}

static int compare(NcKernel *k, uint32_t s1, uint32_t s2, bool nocase)
{
    const uint8_t *p1 = nc_vm_host(k->vm, str_buf(k, s1)), *p2 = nc_vm_host(k->vm, str_buf(k, s2));
    uint16_t n1 = str_len(k, s1), n2 = str_len(k, s2);
    for (uint16_t i = 0; i < n1 && i < n2; i++) {
        int c1 = nocase ? toupper(p1[i]) : p1[i], c2 = nocase ? toupper(p2[i]) : p2[i];
        if (c1 != c2) {
            return c1 - c2;
        }
    }
    return (int)n1 - (int)n2;
}

/* RtlCompareString(String1, String2, CaseInSensitive) -> LONG */
static uint64_t RtlCompareString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return (uint32_t)compare(k, a[0], a[1], a[2] & 0xff);
}

/* RtlEqualString(String1, String2, CaseInSensitive) -> BOOLEAN */
static uint64_t RtlEqualString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return str_len(k, a[0]) == str_len(k, a[1]) && compare(k, a[0], a[1], a[2] & 0xff) == 0;
}

/* RtlCompareMemoryUlong(Source, Length, Pattern) -> bytes that match */
static uint64_t RtlCompareMemoryUlong(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t n = 0;
    while (n + 4 <= a[1] && nc_vm_rd32(k->vm, a[0] + n) == a[2]) {
        n += 4;
    }
    return n;
}

/* The memory routines run as guest code (kernel.c's install_fast_paths);
 * these are the same in the kernel, for NC_NO_FAST. */

/* RtlCompareMemory(Source1, Source2, Length) -> the bytes alike from the start */
static uint64_t RtlCompareMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint8_t *p = nc_vm_host(k->vm, a[0]), *q = nc_vm_host(k->vm, a[1]);
    uint32_t n = 0;
    while (n < a[2] && p[n] == q[n]) {
        n++;
    }
    return n;
}

/* RtlZeroMemory(Destination, Length) */
static uint64_t RtlZeroMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    memset(nc_vm_host(k->vm, a[0]), 0, a[1]);
    return 0;
}

/* RtlFillMemory(Destination, Length, Fill) */
static uint64_t RtlFillMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    memset(nc_vm_host(k->vm, a[0]), (uint8_t)a[2], a[1]);
    return 0;
}

/* RtlFillMemoryUlong(Destination, Length, Pattern): Length / 4 words */
static uint64_t RtlFillMemoryUlong(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    for (uint32_t n = 0; n + 4 <= a[1]; n += 4) {
        nc_vm_wr32(k->vm, a[0] + n, a[2]);
    }
    return 0;
}

/* RtlMoveMemory(Destination, Source, Length): the ranges may overlap */
static uint64_t RtlMoveMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    memmove(nc_vm_host(k->vm, a[0]), nc_vm_host(k->vm, a[1]), a[2]);
    return 0;
}

/* RtlLowerChar(Character) / RtlUpperChar(Character): ASCII case only */
static uint64_t RtlLowerChar(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint8_t c = (uint8_t)a[0];
    return c >= 'A' && c <= 'Z' ? c + 32u : c;
}

static uint64_t RtlUpperChar(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint8_t c = (uint8_t)a[0];
    return c >= 'a' && c <= 'z' ? c - 32u : c;
}

/* RtlUpcaseUnicodeChar(SourceCharacter) -> WCHAR (Midtown Madness 3, on its save names) */
static uint64_t RtlUpcaseUnicodeChar(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return nc_rtl_upcase((uint16_t)a[0]);
}

/* RtlUpcaseUnicodeString(DestinationString, SourceString, AllocateDestinationString) -> NTSTATUS */
static uint64_t RtlUpcaseUnicodeString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint16_t n = str_len(k, a[1]) & ~1u;
    uint32_t buf = str_buf(k, a[0]);
    uint16_t max = str_max(k, a[0]);
    if (a[2] & 0xff) {
        max = n;
        buf = str_alloc(k, max);
        if (!buf) {
            return NC_STATUS_NO_MEMORY;
        }
    } else if (max < n) {
        return 0x80000005u; /* STATUS_BUFFER_OVERFLOW */
    }
    const uint16_t *src = nc_vm_host(k->vm, str_buf(k, a[1]));
    uint16_t *dst = nc_vm_host(k->vm, buf);
    for (uint16_t i = 0; i < n / 2; i++) {
        dst[i] = nc_rtl_upcase(src[i]);
    }
    str_set(k, a[0], n, max, buf);
    return NC_STATUS_SUCCESS;
}

/* RtlUpcaseUnicodeToMultiByteN(MultiByteString, MaxBytesInMultiByteString,
 * BytesInMultiByteString, UnicodeString, BytesInUnicodeString) -> NTSTATUS:
 * the code page is Latin-1 (what isn't in it becomes '?'). */
static uint64_t RtlUpcaseUnicodeToMultiByteN(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t n = a[4] / 2 < a[1] ? a[4] / 2 : a[1];
    uint8_t *dst = nc_vm_host(k->vm, a[0]);
    const uint16_t *src = nc_vm_host(k->vm, a[3]);
    for (uint32_t i = 0; i < n; i++) {
        const uint16_t c = nc_rtl_upcase(src[i]);
        dst[i] = c < 0x100 ? (uint8_t)c : '?';
    }
    if (a[2]) {
        nc_vm_wr32(k->vm, a[2], n);
    }
    return NC_STATUS_SUCCESS;
}

/* RtlUpperString(DestinationString, SourceString): as much of it as fits, ASCII case */
static uint64_t RtlUpperString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint16_t max = str_max(k, a[0]);
    const uint16_t n = str_len(k, a[1]) < max ? str_len(k, a[1]) : max;
    const uint8_t *src = nc_vm_host(k->vm, str_buf(k, a[1]));
    uint8_t *dst = nc_vm_host(k->vm, str_buf(k, a[0]));
    for (uint16_t i = 0; i < n; i++) {
        dst[i] = (uint8_t)toupper(src[i]);
    }
    *(uint16_t *)nc_vm_host(k->vm, a[0]) = n;
    return 0;
}

/* RtlAnsiStringToUnicodeString(DestinationString, SourceString, AllocateDestinationString) */
static uint64_t RtlAnsiStringToUnicodeString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint16_t n = str_len(k, a[1]);
    uint32_t buf = str_buf(k, a[0]);
    uint16_t max = str_max(k, a[0]);
    if (a[2] & 0xff) {
        max = (uint16_t)(n * 2 + 2);
        buf = str_alloc(k, max);
        if (!buf) {
            return NC_STATUS_NO_MEMORY;
        }
    } else if (max < n * 2) {
        return 0x80000005u; /* STATUS_BUFFER_OVERFLOW */
    }
    const uint8_t *src = nc_vm_host(k->vm, str_buf(k, a[1]));
    uint16_t *dst = nc_vm_host(k->vm, buf);
    for (uint16_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
    if (max >= n * 2 + 2) {
        dst[n] = 0;
    }
    str_set(k, a[0], (uint16_t)(n * 2), max, buf);
    return NC_STATUS_SUCCESS;
}

/* RtlUnicodeStringToAnsiString(DestinationString, SourceString, AllocateDestinationString) */
static uint64_t RtlUnicodeStringToAnsiString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint16_t n = str_len(k, a[1]) / 2;
    uint32_t buf = str_buf(k, a[0]);
    uint16_t max = str_max(k, a[0]);
    if (a[2] & 0xff) {
        max = (uint16_t)(n + 1);
        buf = str_alloc(k, max);
        if (!buf) {
            return NC_STATUS_NO_MEMORY;
        }
    } else if (max < n) {
        return 0x80000005u;
    }
    const uint16_t *src = nc_vm_host(k->vm, str_buf(k, a[1]));
    uint8_t *dst = nc_vm_host(k->vm, buf);
    for (uint16_t i = 0; i < n; i++) {
        dst[i] = src[i] < 0x100 ? (uint8_t)src[i] : '?';
    }
    if (max > n) {
        dst[n] = 0;
    }
    str_set(k, a[0], n, max, buf);
    return NC_STATUS_SUCCESS;
}

/* RtlFreeAnsiString(AnsiString) / RtlFreeUnicodeString(UnicodeString) */
static uint64_t RtlFreeString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    str_free(k, str_buf(k, a[0]));
    str_set(k, a[0], 0, 0, 0);
    return 0;
}

/* RtlNtStatusToDosError(Status) */
static uint64_t RtlNtStatusToDosError(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return nc_rtl_status_to_dos(a[0]);
}

uint32_t nc_rtl_status_to_dos(uint32_t status)
{
    /* NT's own rules first: a customer status is its own code; 0xD is 0xC; a
     * Win32 error carried in a status (0xC001xxxx, or HRESULT_FROM_WIN32's
     * 0x8007xxxx) is that error. XNet's sockets hand Winsock errors through
     * here as 0x8007xxxx: WSAEWOULDBLOCK (0x80072733) must come out 10035, or
     * Halo takes a non-blocking connect for a failure and can't create a
     * System Link game. */
    if (!status || (status & 0x20000000u)) {
        return status;
    }
    if ((status & 0xF0000000u) == 0xD0000000u) {
        status &= ~0x10000000u;
    }
    if ((status >> 16) == 0xC001u || (status >> 16) == 0x8007u) {
        return status & 0xFFFFu;
    }
    /* every status the kernel returns (Windows' mapping) */
    static const uint32_t map[][2] = {
        {0x00000000u, 0},   {0x00000102u, 258}, {0x00000103u, 997}, {0x80000005u, 234}, {0x80000006u, 18},
        {0xC0000001u, 31},  {0xC0000002u, 1},   {0xC0000003u, 87},  {0xC0000008u, 6},   {0xC000000Du, 87},
        {0xC000000Fu, 2},   {0xC0000010u, 1},   {0xC0000011u, 38},  {0xC0000017u, 8},   {0xC0000018u, 487},
        {0xC0000022u, 5},   {0xC0000023u, 122}, {0xC0000024u, 6},   {0xC000002Du, 487}, {0xC0000033u, 123},
        {0xC0000034u, 2},   {0xC0000035u, 183}, {0xC000003Au, 3},   {0xC0000043u, 32},  {0xC0000047u, 298},
        {0xC0000056u, 5},   {0xC000007Fu, 112}, {0xC000009Au, 1450}, {0xC000009Cu, 23}, {0xC000009Fu, 487},
        {0xC00000A0u, 487}, {0xC00000A2u, 19},  {0xC00000BAu, 5},   {0xC0000101u, 145}, {0xC0000103u, 267},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i][0] == status) {
            return map[i][1];
        }
    }
    return 317; /* ERROR_MR_MID_NOT_FOUND */
}

/* --------------------------------------------------------------- time */

static bool leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static const int k_month_days[2][12] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
};

/* RtlTimeToTimeFields(Time, TimeFields) */
static uint64_t RtlTimeToTimeFields(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint64_t t = (uint64_t)nc_vm_rd32(k->vm, a[0]) | ((uint64_t)nc_vm_rd32(k->vm, a[0] + 4) << 32);
    uint64_t ms_total = t / 10000u;
    uint64_t days = ms_total / 86400000u;
    uint32_t ms_day = (uint32_t)(ms_total % 86400000u);
    int16_t tf[8];
    tf[7] = (int16_t)((days + 1) % 7); /* 1601-01-01 was a Monday */
    int y = 1601;
    while (days >= (uint64_t)(leap(y) ? 366 : 365)) {
        days -= leap(y) ? 366 : 365;
        y++;
    }
    int m = 0;
    while (days >= (uint64_t)k_month_days[leap(y)][m]) {
        days -= (uint64_t)k_month_days[leap(y)][m];
        m++;
    }
    tf[0] = (int16_t)y;
    tf[1] = (int16_t)(m + 1);
    tf[2] = (int16_t)(days + 1);
    tf[3] = (int16_t)(ms_day / 3600000u);
    tf[4] = (int16_t)(ms_day / 60000u % 60);
    tf[5] = (int16_t)(ms_day / 1000u % 60);
    tf[6] = (int16_t)(ms_day % 1000u);
    memcpy(nc_vm_host(k->vm, a[1]), tf, sizeof(tf));
    return 0;
}

/* RtlTimeFieldsToTime(TimeFields, Time) -> BOOLEAN */
static uint64_t RtlTimeFieldsToTime(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    int16_t tf[8];
    memcpy(tf, nc_vm_host(k->vm, a[0]), sizeof(tf));
    if (tf[0] < 1601 || tf[1] < 1 || tf[1] > 12 || tf[2] < 1 || tf[2] > k_month_days[leap(tf[0])][tf[1] - 1]) {
        return 0;
    }
    uint64_t days = 0;
    for (int y = 1601; y < tf[0]; y++) {
        days += leap(y) ? 366 : 365;
    }
    for (int m = 0; m < tf[1] - 1; m++) {
        days += (uint64_t)k_month_days[leap(tf[0])][m];
    }
    days += (uint64_t)(tf[2] - 1);
    uint64_t ms = days * 86400000ull + (uint64_t)tf[3] * 3600000u + (uint64_t)tf[4] * 60000u +
                  (uint64_t)tf[5] * 1000u + (uint64_t)tf[6];
    uint64_t t = ms * 10000u;
    nc_vm_wr32(k->vm, a[1], (uint32_t)t);
    nc_vm_wr32(k->vm, a[1] + 4, (uint32_t)(t >> 32));
    return 1;
}

void nc_rtl_register(NcKernel *k)
{
    nc_kernel_impl(k, "RtlInitAnsiString", RtlInitAnsiString);
    nc_kernel_impl(k, "RtlInitUnicodeString", RtlInitUnicodeString);
    nc_kernel_impl(k, "RtlCompareString", RtlCompareString);
    nc_kernel_impl(k, "RtlEqualString", RtlEqualString);
    nc_kernel_impl(k, "RtlCompareMemoryUlong", RtlCompareMemoryUlong);
    nc_kernel_impl(k, "RtlCompareMemory", RtlCompareMemory);
    nc_kernel_impl(k, "RtlZeroMemory", RtlZeroMemory);
    nc_kernel_impl(k, "RtlFillMemory", RtlFillMemory);
    nc_kernel_impl(k, "RtlFillMemoryUlong", RtlFillMemoryUlong);
    nc_kernel_impl(k, "RtlMoveMemory", RtlMoveMemory);
    nc_kernel_impl(k, "RtlLowerChar", RtlLowerChar);
    nc_kernel_impl(k, "RtlUpperChar", RtlUpperChar);
    nc_kernel_impl(k, "RtlUpcaseUnicodeChar", RtlUpcaseUnicodeChar);
    nc_kernel_impl(k, "RtlUpcaseUnicodeString", RtlUpcaseUnicodeString);
    nc_kernel_impl(k, "RtlUpcaseUnicodeToMultiByteN", RtlUpcaseUnicodeToMultiByteN);
    nc_kernel_impl(k, "RtlUpperString", RtlUpperString);
    nc_kernel_impl(k, "RtlAnsiStringToUnicodeString", RtlAnsiStringToUnicodeString);
    nc_kernel_impl(k, "RtlUnicodeStringToAnsiString", RtlUnicodeStringToAnsiString);
    nc_kernel_impl(k, "RtlFreeAnsiString", RtlFreeString);
    nc_kernel_impl(k, "RtlFreeUnicodeString", RtlFreeString);
    nc_kernel_impl(k, "RtlNtStatusToDosError", RtlNtStatusToDosError);
    nc_kernel_impl(k, "RtlTimeToTimeFields", RtlTimeToTimeFields);
    nc_kernel_impl(k, "RtlTimeFieldsToTime", RtlTimeFieldsToTime);
}
