/*
 * pds-nx: executable memory for the SH-2 dynarec on Nintendo Switch.
 *
 * Homebrew cannot map memory read/write/execute at once. libnx's Jit object
 * gives us one of two arrangements, depending on what the loader allows:
 *
 *  - CodeMemory: the same memory is mapped twice, a writable view the dynarec
 *    fills with code and an executable view the CPU runs it from. Both stay
 *    mapped, so switching between writing and running costs nothing.
 *
 *  - SetProcessMemoryPermission (fallback): the writable view and the
 *    executable view are never mapped at the same time. Before compiling we
 *    unmap the executable view, and before running we map it again.
 *
 * Either way the dynarec keeps writable pointers and converts them with
 * nx_jit_rx_delta() right before calling into generated code.
 *
 * This file is part of pds-nx and is licensed under the GNU GPL v2 or later.
 */
#include <switch.h>
#include <stdint.h>
#include <string.h>

#include "nx_log.h"

#define MAX_REGIONS 4

typedef struct {
    Jit jit;
    uintptr_t rw;
    uintptr_t rx;
    size_t size;
    int used;
    /* fallback mode: range written since the last switch to executable */
    uintptr_t dirty_lo, dirty_hi;
} Region;

static Region regions[MAX_REGIONS];
static int toggling;   /* 1 when any region uses the fallback mode */
static int executable; /* fallback mode: current state */

static size_t dcache_line(void)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return 4u << ((ctr >> 16) & 0xf);
}

static size_t icache_line(void)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return 4u << (ctr & 0xf);
}

static void dcache_clean(uintptr_t lo, uintptr_t hi)
{
    size_t line = dcache_line();
    for (uintptr_t a = lo & ~(uintptr_t)(line - 1); a < hi; a += line)
        __asm__ volatile("dc cvau, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb ish" : : : "memory");
}

static void icache_invalidate(uintptr_t lo, uintptr_t hi)
{
    size_t line = icache_line();
    for (uintptr_t a = lo & ~(uintptr_t)(line - 1); a < hi; a += line)
        __asm__ volatile("ic ivau, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb ish" : : : "memory");
    __asm__ volatile("isb" : : : "memory");
}

static Region *find(uintptr_t rw)
{
    for (int i = 0; i < MAX_REGIONS; i++)
        if (regions[i].used && rw >= regions[i].rw && rw < regions[i].rw + regions[i].size)
            return &regions[i];
    return NULL;
}

void *nx_jit_alloc(size_t size)
{
    size = (size + 0xFFF) & ~(size_t)0xFFF;
    for (int i = 0; i < MAX_REGIONS; i++) {
        Region *r = &regions[i];
        if (r->used)
            continue;
        Result rc = jitCreate(&r->jit, size);
        if (R_FAILED(rc)) {
            nxlog("jit: jitCreate(0x%zx) failed: 0x%x (launch with full RAM access: hold R over a game)",
                  size, rc);
            return NULL;
        }
        r->rw = (uintptr_t)jitGetRwAddr(&r->jit);
        r->rx = (uintptr_t)jitGetRxAddr(&r->jit);
        r->size = size;
        r->used = 1;
        r->dirty_lo = UINTPTR_MAX;
        r->dirty_hi = 0;
        if (r->jit.type != JitType_CodeMemory)
            toggling = 1;
        memset((void *)r->rw, 0, size);
        nxlog("jit: %zu KiB code buffer, mode=%s, rw=%p rx=%p", size >> 10,
              r->jit.type == JitType_CodeMemory ? "code-memory" : "toggle",
              (void *)r->rw, (void *)r->rx);
        return (void *)r->rw;
    }
    nxlog("jit: out of regions");
    return NULL;
}

void nx_jit_free(void *rw)
{
    Region *r = find((uintptr_t)rw);
    if (r) {
        jitClose(&r->jit);
        r->used = 0;
    }
}

intptr_t nx_jit_rx_delta(const void *rw)
{
    Region *r = find((uintptr_t)rw);
    return r ? (intptr_t)r->rx - (intptr_t)r->rw : 0;
}

void nx_jit_make_writable(void)
{
    if (!toggling || !executable)
        return;
    for (int i = 0; i < MAX_REGIONS; i++) {
        if (regions[i].used && regions[i].jit.type != JitType_CodeMemory) {
            Result rc = jitTransitionToWritable(&regions[i].jit);
            if (R_FAILED(rc))
                nxlog("jit: to-writable failed: 0x%x", rc);
        }
    }
    executable = 0;
}

void nx_jit_make_executable(void)
{
    if (!toggling || executable)
        return;
    for (int i = 0; i < MAX_REGIONS; i++) {
        Region *r = &regions[i];
        if (!r->used || r->jit.type == JitType_CodeMemory)
            continue;
        Result rc = jitTransitionToExecutable(&r->jit);
        if (R_FAILED(rc))
            nxlog("jit: to-executable failed: 0x%x", rc);
        if (r->dirty_hi > r->dirty_lo) {
            intptr_t d = (intptr_t)r->rx - (intptr_t)r->rw;
            icache_invalidate(r->dirty_lo + d, r->dirty_hi + d);
        }
        r->dirty_lo = UINTPTR_MAX;
        r->dirty_hi = 0;
    }
    executable = 1;
}

/* Called by the dynarec after it writes a block of code through the RW view. */
void nx_jit_code_written(uintptr_t lo, uintptr_t hi)
{
    Region *r = find(lo);
    dcache_clean(lo, hi);
    if (!r)
        return;
    if (r->jit.type == JitType_CodeMemory) {
        intptr_t d = (intptr_t)r->rx - (intptr_t)r->rw;
        icache_invalidate(lo + d, hi + d);
    } else {
        /* The executable view is unmapped right now; invalidate on remap. */
        if (lo < r->dirty_lo) r->dirty_lo = lo;
        if (hi > r->dirty_hi) r->dirty_hi = hi;
    }
}
