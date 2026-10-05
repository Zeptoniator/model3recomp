/* model3recomp -- PowerPC 603e execution context.
 *
 * This is the register file that lifted code operates on. It is not a CPU:
 * there is no fetch, no decode, no pipeline. The game's instructions are
 * native functions; this struct is the state they mutate.
 *
 * Sega Model 3 Step 1.x runs a PPC603e at 66 MHz, big-endian, MMU effectively
 * flat (the boot code programs the BATs/segment registers to an identity map,
 * so we do not translate -- see docs/technical/execution-model.md).
 */
#ifndef MODEL3RECOMP_PPC_H
#define MODEL3RECOMP_PPC_H

#include <math.h>
#include <stdint.h>
#include <string.h>

/* Lifted functions are large, and GCC declines to inline small helpers into
 * them: on the SH-4 m3_cmp_s, m3_ld8, m3_cr_set and friends each showed up
 * in the profile as a call per guest instruction (18% of a field). These
 * are one-liners that must disappear into the caller. */
#if defined(__GNUC__)
#define M3_INLINE inline __attribute__((always_inline))
#else
#define M3_INLINE inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct m3_ppc {
    uint32_t r[32];        /* GPRs */
    double   f[32];        /* FPRs -- 603e is scalar only, no paired singles */

    uint32_t lr, ctr, xer;
    uint32_t cr;           /* 8 x 4-bit fields, cr0 in the HIGH nibble (PPC order) */
    uint32_t msr;
    uint32_t fpscr;

    uint32_t sr[16];       /* segment registers: stored, never used (flat map) */
    uint32_t spr[1024];    /* catch-all for mtspr/mfspr: HID0, BATs, DEC, ... */

    uint32_t reserve_addr; /* lwarx/stwcx. reservation */
    int      reserve;
} m3_ppc_t;

/* ---- SPR numbers we actually care about ------------------------------- */
#define M3_SPR_XER   1
#define M3_SPR_LR    8
#define M3_SPR_CTR   9
#define M3_SPR_DEC   22
#define M3_SPR_HID0  1008
#define M3_SPR_TBL_R 268
#define M3_SPR_TBU_R 269

/* ---- Condition register ------------------------------------------------
 * PPC numbers CR bits from the MSB: cr0 is bits 0..3, i.e. the top nibble.
 * Field n occupies shift (28 - 4*n).
 */
static M3_INLINE uint32_t m3_cr_get(const m3_ppc_t *c, unsigned n) {
    return (c->cr >> (28u - 4u * n)) & 0xFu;
}
static M3_INLINE void m3_cr_set(m3_ppc_t *c, unsigned n, uint32_t v) {
    unsigned s = 28u - 4u * n;
    c->cr = (c->cr & ~(0xFu << s)) | ((v & 0xFu) << s);
}
/* Bit index as PPC counts it (0 = MSB of cr0) -> our uint32_t. */
static M3_INLINE uint32_t m3_crbit(const m3_ppc_t *c, unsigned b) {
    return (c->cr >> (31u - b)) & 1u;
}
static M3_INLINE void m3_crbit_set(m3_ppc_t *c, unsigned b, uint32_t v) {
    uint32_t m = 1u << (31u - b);
    c->cr = v ? (c->cr | m) : (c->cr & ~m);
}

/* ---- XER --------------------------------------------------------------- */
#define M3_XER_SO 0x80000000u
#define M3_XER_OV 0x40000000u
#define M3_XER_CA 0x20000000u

static M3_INLINE uint32_t m3_xer_ca(const m3_ppc_t *c) { return (c->xer & M3_XER_CA) ? 1u : 0u; }
static M3_INLINE void m3_set_ca(m3_ppc_t *c, uint32_t carry) {
    c->xer = carry ? (c->xer | M3_XER_CA) : (c->xer & ~M3_XER_CA);
}
static M3_INLINE void m3_set_ov(m3_ppc_t *c, uint32_t ov) {
    if (ov) c->xer |= (M3_XER_OV | M3_XER_SO);
    else    c->xer &= ~M3_XER_OV;
}

/* ---- Compare / record forms -------------------------------------------
 * cmp sets LT,GT,EQ,SO. Rc=1 forms do the same against zero.
 */
static M3_INLINE void m3_cmp_s(m3_ppc_t *c, unsigned fld, int32_t a, int32_t b) {
    uint32_t v = (a < b) ? 8u : (a > b) ? 4u : 2u;
    if (c->xer & M3_XER_SO) v |= 1u;
    m3_cr_set(c, fld, v);
}
static M3_INLINE void m3_cmp_u(m3_ppc_t *c, unsigned fld, uint32_t a, uint32_t b) {
    uint32_t v = (a < b) ? 8u : (a > b) ? 4u : 2u;
    if (c->xer & M3_XER_SO) v |= 1u;
    m3_cr_set(c, fld, v);
}
static M3_INLINE void m3_cr0(m3_ppc_t *c, int32_t v) { m3_cmp_s(c, 0, v, 0); }

/* Floating compare -> cr field. Unordered (NaN) sets bit 0 of the field. */
static M3_INLINE void m3_fcmp(m3_ppc_t *c, unsigned fld, double a, double b) {
    uint32_t v;
    if (a != a || b != b) v = 1u;          /* unordered */
    else                  v = (a < b) ? 8u : (a > b) ? 4u : 2u;
    m3_cr_set(c, fld, v);
}

/* ---- FPSCR result flags ---------------------------------------------------
 * FPRF -- the class of the last arithmetic result, C and FPCC (FL FG FE FU),
 * FPSCR bits 15..19 in PPC numbering. Games read it with mcrfs to learn a
 * value's sign without a compare: Scud Race's atan2 does `frsp f1,f1 ;
 * mcrfs cr7,cr4`. Only functions that read the FPSCR keep it (see the
 * lifter), so it is not maintained everywhere. */
#define M3_FPRF_MASK 0x0001F000u
static M3_INLINE void m3_fprf(m3_ppc_t *c, double d) {
    uint32_t f;
    if (d != d)                    f = 0x11u;              /* QNaN       C  FU */
    else if (d == 0.0)             f = signbit(d) ? 0x12u : 0x02u; /* -0 C FE, +0 FE */
    else if (d < 0.0) {            /* denormals are classed as normals */
        if (d == -INFINITY)        f = 0x09u;              /* -inf    FL FU */
        else                       f = 0x08u;              /* -normal FL    */
    } else {
        if (d == INFINITY)         f = 0x05u;              /* +inf    FG FU */
        else                       f = 0x04u;              /* +normal FG    */
    }
    c->fpscr = (c->fpscr & ~M3_FPRF_MASK) | (f << 12);
}
/* fcmpu / fcmpo set FPCC to the compare result as well as the CR field. */
static M3_INLINE void m3_fpcc(m3_ppc_t *c, uint32_t v) {
    c->fpscr = (c->fpscr & ~0x0000F000u) | ((v & 0xFu) << 12);
}
/* mcrfs: copy FPSCR field s to CR field d, clearing the sticky exception
 * bits that were copied (FX, OX..VXVC, VXSOFT..VXCVI). */
static M3_INLINE void m3_mcrfs(m3_ppc_t *c, unsigned d, unsigned s) {
    unsigned sh = 28u - 4u * s;
    m3_cr_set(c, d, (c->fpscr >> sh) & 0xFu);
    c->fpscr &= ~((0x9FF80700u >> sh & 0xFu) << sh);
}

/* ---- Rotate helpers ---------------------------------------------------- */
static M3_INLINE uint32_t m3_rotl32(uint32_t v, unsigned n) {
    n &= 31u;
    return n ? ((v << n) | (v >> (32u - n))) : v;
}
/* PPC MB/ME mask: bits MB..ME inclusive, counted from the MSB, and the mask
 * WRAPS when mb > me. Getting the wrap case wrong silently corrupts every
 * rlwinm-based bitfield extract in the game, so it is spelled out. */
static M3_INLINE uint32_t m3_mask(unsigned mb, unsigned me) {
    uint32_t a = 0xFFFFFFFFu >> mb;                                  /* bits mb..31 */
    uint32_t b = (me >= 31u) ? 0xFFFFFFFFu : (0xFFFFFFFFu << (31u - me)); /* bits 0..me */
    return (mb <= me) ? (a & b) : (a | b);   /* mb > me wraps -- PPC says OR */
}

static M3_INLINE uint32_t m3_cntlzw(uint32_t v) {
    uint32_t n = 0;
    if (!v) return 32u;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}

/* ---- float<->int bit punning (no aliasing UB) -------------------------- */
static M3_INLINE float  m3_bits_to_f32(uint32_t b) { float  f; memcpy(&f, &b, 4); return f; }
static M3_INLINE uint32_t m3_f32_to_bits(float f)  { uint32_t b; memcpy(&b, &f, 4); return b; }
static M3_INLINE double m3_bits_to_f64(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static M3_INLINE uint64_t m3_f64_to_bits(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }

/* ---- The context -------------------------------------------------------
 * One global, like every other recomp in this family. Lifted code says
 * PPC_R(3), not ctx->r[3]. Interrupt handlers nest on the C stack and save
 * and restore state themselves -- which is exactly what they do on hardware,
 * so there is nothing to model.
 */
extern m3_ppc_t m3_ctx;

#define PPC_R(n)    (m3_ctx.r[(n)])
#define PPC_F(n)    (m3_ctx.f[(n)])
#define PPC_LR      (m3_ctx.lr)
#define PPC_CTR     (m3_ctx.ctr)
#define PPC_XER     (m3_ctx.xer)
#define PPC_CR      (m3_ctx.cr)
#define PPC_MSR     (m3_ctx.msr)

void ppc_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* MODEL3RECOMP_PPC_H */
