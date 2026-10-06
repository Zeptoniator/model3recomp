/* model3recomp -- guest address -> native function dispatch.
 *
 * Open-addressed, power-of-two, linear probing. A lifted Model 3 game is a
 * couple of thousand functions; this never grows past a few tens of KB and
 * every indirect call in the game goes through it, so it stays simple and
 * branch-light rather than clever.
 */
#include "model3recomp/func_table.h"
#include "model3recomp/irq.h"
#include "model3recomp/model3recomp.h"
#include "model3recomp/bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t addr; m3_func_t fn; } slot_t;

static slot_t  *g_tab;
static uint32_t g_mask;
static uint32_t g_count;

/* Knuth multiplicative. Guest addresses are 4-byte aligned and heavily
 * clustered, so the low bits alone would collide constantly. */
static inline uint32_t hash(uint32_t a)
{
    return (a >> 2) * 2654435761u;
}

static void grow(void);

void func_table_init(void)
{
    free(g_tab);
    g_mask  = 1023;
    g_count = 0;
    g_tab   = calloc(g_mask + 1, sizeof(slot_t));
    if (!g_tab) { fprintf(stderr, "[model3recomp] func table alloc failed\n"); abort(); }
}

void func_table_register(uint32_t addr, m3_func_t fn)
{
    uint32_t i;
    if (!g_tab) func_table_init();
    /* Keep it under three-quarters full; linear probing degrades fast above. */
    if ((g_count + 1) * 4 > (g_mask + 1) * 3) grow();

    i = hash(addr) & g_mask;
    for (;;) {
        if (!g_tab[i].fn) {
            g_tab[i].addr = addr;
            g_tab[i].fn   = fn;
            g_count++;
            return;
        }
        if (g_tab[i].addr == addr) { g_tab[i].fn = fn; return; }
        i = (i + 1) & g_mask;
    }
}

static void grow(void)
{
    slot_t  *old  = g_tab;
    uint32_t oldn = g_mask + 1, i;
    g_mask = g_mask * 2 + 1;
    g_count = 0;
    g_tab = calloc(g_mask + 1, sizeof(slot_t));
    if (!g_tab) { fprintf(stderr, "[model3recomp] func table grow failed\n"); abort(); }
    for (i = 0; i < oldn; i++)
        if (old[i].fn) func_table_register(old[i].addr, old[i].fn);
    free(old);
}

m3_func_t func_table_lookup(uint32_t addr)
{
    uint32_t i;
    if (!g_tab) return NULL;
    i = hash(addr) & g_mask;
    for (;;) {
        if (!g_tab[i].fn) return NULL;
        if (g_tab[i].addr == addr) return g_tab[i].fn;
        i = (i + 1) & g_mask;
    }
}

/* A miss is a lifter coverage hole, not a game bug, and naming it is the most
 * useful thing this library can do when a port stops making progress. Each
 * address is reported once; an indirect call in a hot loop would otherwise
 * drown out everything else. */
/* M3_TRACE_CALLS=N prints the first N dispatched addresses. A recompiled game
 * has no program counter to inspect, so this is the equivalent of watching
 * where it goes. */
/* M3_TRACE_CALLS=N, or N@F to start at field F -- the only way to see where
 * a frozen game is looping, since a recompiled guest has no program counter. */
static long g_trace_from;
static long trace_budget(void)
{
    static long n = -1;
    if (n < 0) {
        const char *e = getenv("M3_TRACE_CALLS");
        const char *at = e ? strchr(e, '@') : NULL;
        n = e ? strtol(e, NULL, 0) : 0;
        g_trace_from = at ? strtol(at + 1, NULL, 0) : 0;
    }
    return n;
}

/* Where the last guest return was going; see M3_RETURNED in lift.h. */
uint32_t m3_ret_to;

#ifdef M3_LOOP_GUARD
void m3_unwind_note(uint32_t expected)
{
    extern uint32_t m3_fn_now;
    static long left = -1;
    static uint32_t only;
    if (left < 0) {
        const char *e = getenv("M3_UNWIND_TRACE");
        const char *o = getenv("M3_UNWIND_ONLY");    /* one target only */
        left = e ? strtol(e, NULL, 0) : 0;
        only = o ? (uint32_t)strtoul(o, NULL, 0) : 0;
    }
    if (left > 0 && (!only || m3_ret_to == only)) {
        left--;
        fprintf(stderr, "[unwind] frame waiting for %08X passed by a return "
                        "to %08X (last function entered %08X)\n",
                expected, m3_ret_to, m3_fn_now);
    }
}
#endif

/* M3_WORKSIG=from,to,base:size[,base:size...]: in fields from..to, at every
 * dispatched call, a hash of those RAM ranges, printed when it changes, with
 * the call. Two hosts print the same lines until the first call after which
 * their RAM differs -- a field-level signature mismatch narrowed to a
 * function, in one run. */
static void worksig(uint32_t addr)
{
    static int parsed;
    static unsigned long from, to, base[8], size[8];
    static int n;
    static uint32_t last;
    unsigned long f;
    uint32_t h = 2166136261u;
    const uint8_t *r;
    int k;
    if (!parsed) {
        const char *e = getenv("M3_WORKSIG");
        char *end;
        parsed = 1;
        if (!e) return;
        from = strtoul(e, &end, 0);
        if (*end == ',') to = strtoul(end + 1, &end, 0);
        while (*end == ',' && n < 8) {
            base[n] = strtoul(end + 1, &end, 0);
            if (*end != ':') break;
            size[n] = strtoul(end + 1, &end, 0);
            if (base[n] + size[n] <= M3_RAM_SIZE) n++;
        }
    }
    if (!n) return;
    f = (unsigned long)model3recomp_frame_count();
    if (f < from || f > to) return;
    r = bus_ram();
    for (k = 0; k < n; k++) {
        unsigned long i;
        for (i = 0; i < size[k]; i++) h = (h ^ r[base[k] + i]) * 16777619u;
    }
    if (h != last)
        fprintf(stderr, "[wsig] f %lu w %llu call %08X h %08X\n", f,
                (unsigned long long)m3_work, addr, (unsigned)h);
    last = h;
}

#ifdef M3_FN_TRACE
#include "model3recomp/ppc.h"
/* M3_FNSIG=w_from,w_to: for every function entered while m3_work is in
 * [w_from, w_to], its address and a hash of the GPRs, FPRs, CR, CTR and
 * FPSCR. Below M3_WORKSIG's dispatched calls: direct calls too, and the
 * registers, where a difference between two hosts shows before it reaches
 * RAM. */
void m3_fn_trace(uint32_t addr)
{
    static int parsed;
    static unsigned long long wf, wt;
    uint32_t h = 2166136261u;
    size_t i;
    if (!parsed) {
        const char *e = getenv("M3_FNSIG");
        parsed = 1;
        if (e) sscanf(e, "%llu,%llu", &wf, &wt);
    }
    if (!wt || m3_work < wf || m3_work > wt) return;
    for (i = 0; i < 32; i++)
        h = (h ^ m3_ctx.r[i]) * 16777619u;
    for (i = 0; i < 32; i++) {   /* as bits: hosts lay a double out differently */
        uint64_t b = m3_f64_to_bits(m3_ctx.f[i]);
        h = (h ^ (uint32_t)(b >> 32)) * 16777619u;
        h = (h ^ (uint32_t)b) * 16777619u;
    }
    h = (h ^ m3_ctx.cr) * 16777619u;
    h = (h ^ m3_ctx.ctr) * 16777619u;
    h = (h ^ m3_ctx.fpscr) * 16777619u;
    fprintf(stderr, "[fnsig] w %llu fn %08X h %08X\n",
            (unsigned long long)m3_work, addr, (unsigned)h);
}
#endif

int func_table_call(uint32_t addr)
{
    enum { MISS_MAX = 64 };
    static uint32_t miss[MISS_MAX];
    static unsigned nmiss;
    static long traced;
    unsigned k;

    m3_work += 16;                 /* a dispatched call is real work */
    worksig(addr);

    if (traced < trace_budget() &&
        (long)model3recomp_frame_count() >= g_trace_from) {
        traced++;
        fprintf(stderr, "[call %ld] %08X%s\n", traced, addr,
                func_table_lookup(addr) ? "" : "   <-- NOT LIFTED");
    }

    m3_func_t fn = func_table_lookup(addr);

    /* The frame boundary. A Model 3 main loop waits on a RAM flag that only
     * the VBlank handler sets, so nothing advances unless interrupts are
     * delivered on a timer -- and this is where the runtime gets control.
     * See docs/technical/execution-model.md. */
    irq_tick();

    if (fn) { fn(); return 1; }

    for (k = 0; k < nmiss; k++)
        if (miss[k] == addr) return 0;
    if (nmiss < MISS_MAX) miss[nmiss++] = addr;
#ifdef M3_LOOP_GUARD
    {
        /* Who asked: an address alone does not say whether it is a missing
         * entry or a pointer the guest never meant to call. */
        extern uint32_t m3_fn_now;
        fprintf(stderr, "[model3recomp] no function lifted at %08X "
                        "(called from guest function %08X, LR %08X)\n",
                addr, m3_fn_now, m3_ctx.lr);
    }
#else
    fprintf(stderr, "[model3recomp] no function lifted at %08X\n", addr);
#endif
    return 0;
}
