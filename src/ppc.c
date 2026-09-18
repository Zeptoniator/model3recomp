/* model3recomp -- PowerPC 603e context.
 *
 * There is almost nothing here, and that is the point: lifted code mutates
 * m3_ctx directly through the macros in lift.h. This file owns the storage,
 * the reset state, and the few helpers that need a real function.
 */
#include "model3recomp/ppc.h"
#include "model3recomp/lift.h"
#include "model3recomp/model3recomp.h"
#include "model3recomp/irq.h"

#include <stdio.h>

m3_ppc_t m3_ctx;

uint64_t m3_work;

/* The 603e comes out of reset with MSR[IP] set, so the vectors are the ones
 * at 0xFFF00000 in ROM. Games clear it once they have installed their own in
 * RAM -- see docs/technical/execution-model.md. */
#define MSR_IP 0x00000040u
#define MSR_ME 0x00001000u

void ppc_reset(void)
{
    unsigned i;
    for (i = 0; i < 32; i++) { m3_ctx.r[i] = 0; m3_ctx.f[i] = 0.0; }
    for (i = 0; i < 16; i++) m3_ctx.sr[i] = 0;
    for (i = 0; i < 1024; i++) m3_ctx.spr[i] = 0;
    m3_ctx.lr = m3_ctx.ctr = m3_ctx.xer = m3_ctx.cr = m3_ctx.fpscr = 0;
    m3_ctx.reserve_addr = 0;
    m3_ctx.reserve = 0;
    m3_ctx.msr = MSR_IP | MSR_ME;
}

/* The time base counts at the bus clock / 4. Nothing in the attract path
 * depends on its absolute rate, only that it advances monotonically. */
static uint64_t g_tb;

uint32_t m3_timebase(unsigned spr)
{
    g_tb += 1;
    return (spr == 269) ? (uint32_t)(g_tb >> 32) : (uint32_t)g_tb;
}

#ifdef M3_WATCH
/* Reported with the guest function that did the write, which is the part that
 * matters: the address alone says a word changed, not who changed it. */
void m3_watch_hit(uint32_t addr, uint32_t val, unsigned size)
{
    fprintf(stderr, "[model3recomp] watch %08X = %08X (size %u) from guest "
                    "function %08X, field %llu\n",
            addr, val, size, m3_fn_now,
            (unsigned long long)model3recomp_frame_count());
    fflush(stderr);
}
#endif

/* See M3_LOOPED in lift.h. Credits the guest with the work its loop did and
 * lets the field clock run, so a wait on an interrupt-set RAM word ends. */
uint32_t m3_loop_n;

void m3_loop_tick(void)
{
    m3_work += M3_LOOP_CREDIT;
    irq_tick();
}

/* Where lifted code last entered a guest function, and the guard that reports
 * it. Only compiled into a build that defines M3_LOOP_GUARD; see lift.h. */
#ifdef M3_LOOP_GUARD
uint32_t m3_fn_now;
uint32_t m3_fn_prev;

/* Which guest functions ran, and how often.
 *
 * M3_TRACE_CALLS only sees indirect dispatch, so a frame task's whole call
 * tree -- every direct call the lifter resolved statically -- is invisible to
 * it. This counts every function entry instead, which is the difference
 * between "the frame task ran" and "the frame task ran and here is what it
 * did". Open addressing, no allocation, no ordering: it runs on every call.
 */
#define FN_SLOTS 8192u
static uint32_t g_fn_addr[FN_SLOTS];
static uint32_t g_fn_hits[FN_SLOTS];

void m3_fn_count(uint32_t addr)
{
    uint32_t i = (addr * 2654435761u) & (FN_SLOTS - 1u);
    uint32_t probes = 0;
    for (; probes < 64u; probes++) {
        if (g_fn_addr[i] == addr) { g_fn_hits[i]++; return; }
        if (!g_fn_hits[i]) { g_fn_addr[i] = addr; g_fn_hits[i] = 1; return; }
        i = (i + 1u) & (FN_SLOTS - 1u);
    }
}

void m3_fn_report(unsigned top)
{
    unsigned n;
    fprintf(stderr, "[model3recomp] guest functions entered, most first:\n");
    for (n = 0; n < top; n++) {
        uint32_t best = 0, bi = FN_SLOTS;
        uint32_t i;
        for (i = 0; i < FN_SLOTS; i++)
            if (g_fn_hits[i] > best) { best = g_fn_hits[i]; bi = i; }
        if (bi == FN_SLOTS) break;
        fprintf(stderr, "    %08X  x%u\n", g_fn_addr[bi], g_fn_hits[bi]);
        g_fn_hits[bi] = 0;
    }
    fflush(stderr);
}

void m3_loop_guard(uint32_t branch_at)
{
    static uint64_t spins;
    static uint64_t seen_frame;
    uint64_t now = model3recomp_frame_count();

    if (now != seen_frame) {          /* progress: the guest is fine */
        seen_frame = now;
        spins = 0;
        return;
    }
    if (++spins < 500000000ull)
        return;
    fprintf(stderr, "[model3recomp] %llu backward branches and no field "
                    "advance; the guest is looping at %08X, in the function "
                    "entered at %08X\n",
            (unsigned long long)spins, branch_at, m3_fn_now);
    fprintf(stderr, "    in_dispatch=%d  pending=%08X  enable=%08X  field=%llu\n",
            irq_in_dispatch(), irq_pending(), irq_enable_read(),
            (unsigned long long)now);
    fflush(stderr);
    spins = 0;
}
#endif

/* An instruction the lifter did not translate. Loud once per site, then
 * silent -- a hot loop containing one would otherwise bury everything else. */
void m3_unimplemented(uint32_t addr, uint32_t raw, const char *mn)
{
    enum { SEEN_MAX = 64 };
    static uint32_t seen[SEEN_MAX];
    static unsigned n;
    unsigned i;
    for (i = 0; i < n; i++)
        if (seen[i] == addr) return;
    if (n < SEEN_MAX) seen[n++] = addr;
    fprintf(stderr, "[model3recomp] unimplemented %s at %08X (%08X)\n",
            mn ? mn : "?", addr, raw);
}
