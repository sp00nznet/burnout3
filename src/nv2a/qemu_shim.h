/*
 * QEMU Shim Layer for xemu NV2A extraction
 *
 * Replaces QEMU types and functions with minimal equivalents so that
 * xemu's NV2A GPU emulation code can compile standalone.
 *
 * This file is included instead of the real QEMU headers.
 */

#ifndef BURNOUT3_QEMU_SHIM_H
#define BURNOUT3_QEMU_SHIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

/* The QEMU shim uses Win32-style threading primitives. On Windows this comes
 * from <windows.h>; on POSIX it comes from win32_compat (via xbox_winnt.h). */
#include "platform/xbox_winnt.h"

/* ============================================================
 * Basic QEMU types
 * ============================================================ */

typedef uint64_t hwaddr;
typedef int64_t  ram_addr_t;
typedef uintptr_t target_ulong;

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define ROUND_UP(n, d) (((n) + (d) - 1) & ~((d) - 1))

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Units */
#define KiB (1024ULL)
#define MiB (1024ULL * KiB)
#define GiB (1024ULL * MiB)

#define NANOSECONDS_PER_SECOND 1000000000LL

/* ============================================================
 * Bit manipulation
 * ============================================================ */

/* For MSVC which doesn't have __builtin_ctz: */
#ifdef _MSC_VER
#include <intrin.h>
static inline int __builtin_ctz(unsigned int x) {
    unsigned long index;
    _BitScanForward(&index, x);
    return (int)index;
}
static inline int __builtin_clz(unsigned int x) {
    unsigned long index;
    _BitScanReverse(&index, x);
    return 31 - (int)index;
}
#endif

/* QEMU's ctz32 = count trailing zeros (32-bit) */
static inline int ctz32(uint32_t val) {
    return __builtin_ctz(val);
}

/* GET_MASK/SET_MASK - defined here, guarded in nv2a_regs.h */
#define GET_MASK(v, mask) (((v) & (mask)) >> ctz32(mask))
#define SET_MASK(v, mask, val) \
    do { (v) &= ~(mask); (v) |= ((val) << ctz32(mask)) & (mask); } while (0)

/* muldiv64: (a * b) / c with 128-bit intermediate */
#ifdef _MSC_VER
#include <intrin.h>
static inline uint64_t muldiv64(uint64_t a, uint32_t b, uint32_t c) {
    uint64_t hi, lo;
    lo = _umul128(a, (uint64_t)b, &hi);
    /* Simple fallback: if hi is 0, just do lo/c */
    if (hi == 0) return lo / c;
    /* Otherwise use 128-bit division via shifts */
    /* For our use case (timer math), overflow is unlikely */
    return (uint64_t)(((double)a * b) / c);
}
#else
static inline uint64_t muldiv64(uint64_t a, uint32_t b, uint32_t c) {
    return (uint64_t)(((unsigned __int128)a * b) / c);
}
#endif

/* ============================================================
 * Threading (Win32-style; portable via win32_compat on POSIX)
 * ============================================================ */

typedef struct QemuMutex {
    CRITICAL_SECTION cs;
} QemuMutex;

typedef struct QemuCond {
    CONDITION_VARIABLE cv;
} QemuCond;

typedef struct QemuThread {
    HANDLE thread;
} QemuThread;

static inline void qemu_mutex_init(QemuMutex *m) {
    InitializeCriticalSection(&m->cs);
}
static inline void qemu_mutex_destroy(QemuMutex *m) {
    DeleteCriticalSection(&m->cs);
}
static inline void qemu_mutex_lock(QemuMutex *m) {
    EnterCriticalSection(&m->cs);
}
static inline void qemu_mutex_unlock(QemuMutex *m) {
    LeaveCriticalSection(&m->cs);
}

static inline void qemu_cond_init(QemuCond *c) {
    InitializeConditionVariable(&c->cv);
}
static inline void qemu_cond_destroy(QemuCond *c) {
    (void)c; /* no-op on Win32 */
}
static inline void qemu_cond_signal(QemuCond *c) {
    WakeConditionVariable(&c->cv);
}
static inline void qemu_cond_broadcast(QemuCond *c) {
    WakeAllConditionVariable(&c->cv);
}
static inline void qemu_cond_wait(QemuCond *c, QemuMutex *m) {
    SleepConditionVariableCS(&c->cv, &m->cs, INFINITE);
}

/* ============================================================
 * Memory regions (stub)
 * ============================================================ */

typedef struct MemoryRegion {
    uint64_t size;
    void *opaque;
} MemoryRegion;

static inline uint64_t memory_region_size(MemoryRegion *mr) {
    return mr->size;
}

/* MemoryRegionOps - function pointers for MMIO read/write */
typedef struct MemoryRegionOps {
    uint64_t (*read)(void *opaque, hwaddr addr, unsigned int size);
    void (*write)(void *opaque, hwaddr addr, uint64_t val, unsigned int size);
} MemoryRegionOps;

/* ============================================================
 * PCI (stub)
 * ============================================================ */

#define PCI_CONFIG_SPACE_SIZE 256

typedef struct PCIDevice {
    int devfn;
    uint8_t config[PCI_CONFIG_SPACE_SIZE];
} PCIDevice;

typedef struct PCIBus {
    int dummy;
} PCIBus;

typedef int qemu_irq;

#define PCI_DEVICE(x) ((PCIDevice*)(x))

/* PCI config register offsets */
#define PCI_VENDOR_ID   0x00
#define PCI_COMMAND     0x04
#define PCI_CLASS_REVISION 0x08

static inline uint32_t pci_get_long(const uint8_t *config) {
    return *(const uint32_t *)config;
}
static inline void pci_set_long(uint8_t *config, uint32_t val) {
    *(uint32_t *)config = val;
}

static inline void pci_irq_assert(PCIDevice *d) { (void)d; }
static inline void pci_irq_deassert(PCIDevice *d) { (void)d; }

/* ============================================================
 * VGA (stub - minimal needed by NV2A)
 * ============================================================ */

typedef struct VGACommonState {
    uint8_t *vram_ptr;
    uint32_t vram_size;
    uint8_t cr[256];
    uint8_t sr[256];
} VGACommonState;

typedef struct VGADisplayParams {
    int line_offset;
    int start_addr;
    int line_compare;
} VGADisplayParams;

#define VGA_CRTC_LINE_COMPARE 0x18
#define VGA_CRTC_OVERFLOW     0x07
#define VGA_CRTC_MAX_SCAN     0x09
#define VGA_SEQ_CLOCK_MODE    0x01
#define VGA_SR01_SCREEN_OFF   0x20

typedef struct GraphicHwOps {
    void *gfx_update;
} GraphicHwOps;

typedef struct QEMUTimer {
    int dummy;
} QEMUTimer;

/* container_of macro */
#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* ============================================================
 * Byte order helpers (LE host assumed for x86)
 * ============================================================ */

static inline uint32_t ldl_le_p(const void *p) {
    return *(const uint32_t *)p;
}
static inline uint16_t lduw_le_p(const void *p) {
    return *(const uint16_t *)p;
}
static inline void stl_le_p(void *p, uint32_t v) {
    *(uint32_t *)p = v;
}
static inline void stw_le_p(void *p, uint16_t v) {
    *(uint16_t *)p = v;
}

/* ============================================================
 * Bitmap operations (from QEMU)
 * ============================================================ */

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define DECLARE_BITMAP(name, bits) \
    unsigned long name[((bits) + BITS_PER_LONG - 1) / BITS_PER_LONG]

static inline void bitmap_zero(unsigned long *dst, long nbits) {
    memset(dst, 0, ((nbits + BITS_PER_LONG - 1) / BITS_PER_LONG) * sizeof(unsigned long));
}
static inline void set_bit(long nr, unsigned long *addr) {
    addr[nr / BITS_PER_LONG] |= 1UL << (nr % BITS_PER_LONG);
}
static inline void clear_bit(long nr, unsigned long *addr) {
    addr[nr / BITS_PER_LONG] &= ~(1UL << (nr % BITS_PER_LONG));
}
static inline bool test_bit(long nr, const unsigned long *addr) {
    return (addr[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG)) & 1;
}

/* ============================================================
 * Logging and error handling
 * ============================================================ */

#define error_report(...) fprintf(stderr, "[NV2A] " __VA_ARGS__)
#define warn_report(...) fprintf(stderr, "[NV2A-WARN] " __VA_ARGS__)
#define warn_report_once(...) do { \
    static int _once = 0; \
    if (!_once) { _once = 1; fprintf(stderr, "[NV2A-WARN] " __VA_ARGS__); } \
} while (0)

/* ============================================================
 * Tracing (stub - no-op)
 * ============================================================ */

#define trace_nv2a_irq(...)             do {} while(0)
#define trace_nv2a_dma_map(...)         do {} while(0)
#define trace_nv2a_reg_read(...)        do {} while(0)
#define trace_nv2a_reg_write(...)       do {} while(0)
#define trace_nv2a_pgraph_method(...)   do {} while(0)
#define trace_nv2a_pfifo_dma_push(...) do {} while(0)

/* ============================================================
 * Migration / VM state (stub)
 * ============================================================ */

typedef struct VMStateDescription {
    const char *name;
} VMStateDescription;

/* ============================================================
 * QEMU queue macros (minimal)
 * ============================================================ */

#define QTAILQ_ENTRY(type) struct { type *tqe_next; type **tqe_prev; }
#define QTAILQ_HEAD(name, type) struct name { type *tqh_first; type **tqh_last; }
#define QTAILQ_INIT(head) do { \
    (head)->tqh_first = NULL; \
    (head)->tqh_last = &(head)->tqh_first; \
} while (0)
#define QTAILQ_INSERT_TAIL(head, elm, field) do { \
    (elm)->field.tqe_next = NULL; \
    (elm)->field.tqe_prev = (head)->tqh_last; \
    *(head)->tqh_last = (elm); \
    (head)->tqh_last = &(elm)->field.tqe_next; \
} while (0)
#define QTAILQ_FOREACH(var, head, field) \
    for ((var) = (head)->tqh_first; (var); (var) = (var)->field.tqe_next)
#define QTAILQ_FOREACH_SAFE(var, head, field, next_var) \
    for ((var) = (head)->tqh_first; (var) && ((next_var) = (var)->field.tqe_next, 1); (var) = (next_var))
#define QTAILQ_REMOVE(head, elm, field) do { \
    if ((elm)->field.tqe_next) (elm)->field.tqe_next->field.tqe_prev = (elm)->field.tqe_prev; \
    else (head)->tqh_last = (elm)->field.tqe_prev; \
    *(elm)->field.tqe_prev = (elm)->field.tqe_next; \
} while (0)

/* ============================================================
 * Misc QEMU helpers
 * ============================================================ */

#define g_malloc0(n) calloc(1, (n))
#define g_malloc(n) malloc(n)
#define g_realloc(p, n) realloc((p), (n))
#define g_free(p) free(p)
#define g_new0(type, n) ((type*)calloc((n), sizeof(type)))
#define g_new(type, n) ((type*)malloc((n) * sizeof(type)))

/* QEMU's OBJECT_CHECK just casts */
#define OBJECT_CHECK(type, obj, name) ((type*)(obj))

/* atomic operations */
#define qatomic_read(ptr) (*(ptr))
#define qatomic_set(ptr, val) (*(ptr) = (val))
#define qatomic_fetch_add(ptr, val) __sync_fetch_and_add((ptr), (val))

#ifdef _MSC_VER
/* MSVC doesn't have __sync_fetch_and_add, use _InterlockedExchangeAdd */
#undef qatomic_fetch_add
#define qatomic_fetch_add(ptr, val) _InterlockedExchangeAdd((long volatile*)(ptr), (val))
#endif

/* Address space stub */
static inline void *address_space_map(void *as, hwaddr addr, hwaddr *len,
                                       bool is_write, int attrs) {
    (void)as; (void)addr; (void)len; (void)is_write; (void)attrs;
    return NULL;
}
static inline void address_space_unmap(void *as, void *buffer, hwaddr len,
                                        bool is_write, hwaddr access_len) {
    (void)as; (void)buffer; (void)len; (void)is_write; (void)access_len;
}

/* Main loop lock stubs */
static inline void bql_lock(void) {}
static inline void bql_unlock(void) {}
#define qemu_mutex_lock_iothread() bql_lock()
#define qemu_mutex_unlock_iothread() bql_unlock()

/* Timer */
static inline int64_t qemu_clock_get_ns(int type) {
    (void)type;
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (int64_t)(count.QuadPart * 1000000000LL / freq.QuadPart);
}
#define QEMU_CLOCK_VIRTUAL 0

/* Error type stub */
typedef struct Error { char msg[256]; } Error;
#define error_setg(errp, ...) do { if (errp && *errp == NULL) { \
    *errp = (Error*)calloc(1, sizeof(Error)); \
    snprintf((*errp)->msg, sizeof((*errp)->msg), __VA_ARGS__); \
}} while (0)
#define error_free(e) free(e)

/* Config stubs */
static inline bool xemu_is_xbe_loaded(void) { return true; }

#endif /* BURNOUT3_QEMU_SHIM_H */
