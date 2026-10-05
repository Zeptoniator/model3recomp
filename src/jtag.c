/* model3recomp -- the video board's JTAG chain (see jtag.h).
 *
 * An IEEE 1149.1 TAP controller and a chain of devices, each a shift
 * register in front of an instruction or data register. Written from the
 * standard; the board facts below -- the chain order, the IR lengths and
 * capture values, the instruction numbers and the IDCODEs per Step -- are
 * the ones Supermodel documents (Src/Model3/JTAG.cpp, JTAG.h, Model3.cpp),
 * and The Lost World and Scud Race agree with them: Scud compares what it
 * reads against 0x?16C3057..0x?16C7057.
 */
#include "model3recomp/jtag.h"

#include <string.h>

enum {  /* TAP states, IEEE 1149.1 */
    TLR, RTI, SEL_DR, CAP_DR, SH_DR, EX1_DR, PA_DR, EX2_DR, UPD_DR,
    SEL_IR, CAP_IR, SH_IR, EX1_IR, PA_IR, EX2_IR, UPD_IR
};

/* Next state for TMS = 0 and TMS = 1. */
static const uint8_t k_next[16][2] = {
    [TLR]    = { RTI,    TLR    }, [RTI]    = { RTI,    SEL_DR },
    [SEL_DR] = { CAP_DR, SEL_IR }, [CAP_DR] = { SH_DR,  EX1_DR },
    [SH_DR]  = { SH_DR,  EX1_DR }, [EX1_DR] = { PA_DR,  UPD_DR },
    [PA_DR]  = { PA_DR,  EX2_DR }, [EX2_DR] = { SH_DR,  UPD_DR },
    [UPD_DR] = { RTI,    SEL_DR }, [SEL_IR] = { CAP_IR, TLR    },
    [CAP_IR] = { SH_IR,  EX1_IR }, [SH_IR]  = { SH_IR,  EX1_IR },
    [EX1_IR] = { PA_IR,  UPD_IR }, [PA_IR]  = { PA_IR,  EX2_IR },
    [EX2_IR] = { SH_IR,  UPD_IR }, [UPD_IR] = { RTI,    SEL_DR },
};

enum { ASIC_IR_BITS = 5, RAM_IR_BITS = 4 };
enum { INSN_IDCODE = 3, INSN_MODEWORD = 8 };

typedef struct {
    int      asic;          /* 0: a 3D-RAM, which only knows BYPASS */
    int      kind;          /* jtag_modeword() index; -1 for a second copy */
    uint32_t idcode;
    uint32_t modeword;
    uint32_t ir;
    uint32_t shift;         /* bit 0 is the one next out on TDO */
    unsigned len;           /* bits in the shift register right now */
} jdev_t;

static jdev_t   g_dev[17];
static unsigned g_ndev;
static unsigned g_state;
static uint8_t  g_tck, g_tdo;
static uint32_t g_modeword[5];

static void dev_reset(jdev_t *d)
{
    if (d->asic) {
        d->ir = INSN_IDCODE;
        d->shift = d->idcode;
        d->len = 32;
    } else {
        d->ir = (1u << RAM_IR_BITS) - 1u;   /* all ones is BYPASS */
        d->shift = 0;
        d->len = 1;
    }
}

static void capture_dr(jdev_t *d)
{
    if (d->asic && d->ir == INSN_IDCODE) { d->shift = d->idcode;   d->len = 32; }
    else if (d->asic && d->ir == INSN_MODEWORD) { d->shift = d->modeword; d->len = 32; }
    else { d->shift = 0; d->len = 1; }        /* BYPASS captures 0 */
}

static void capture_ir(jdev_t *d)
{
    /* The standard requires the two low bits to capture as 01. */
    if (d->asic) { d->shift = 1;   d->len = ASIC_IR_BITS; }
    else         { d->shift = 0x9; d->len = RAM_IR_BITS;  }
}

static unsigned shift_one(jdev_t *d, unsigned tdi)
{
    unsigned out = d->shift & 1u;
    d->shift >>= 1;
    if (tdi) d->shift |= 1u << (d->len - 1u);
    return out;
}

static void update_dr(jdev_t *d)
{
    if (d->asic && d->ir == INSN_MODEWORD) {
        d->modeword = d->shift;
        if (d->kind >= 0)
            g_modeword[d->kind] = d->shift;
    }
}

static void update_ir(jdev_t *d)
{
    d->ir = d->shift & ((1u << d->len) - 1u);
}

void jtag_init(unsigned step)
{
    /* ID codes per Step, for Jupiter, Mercury, Venus, Earth, Mars. */
    static const uint32_t ids[4][5] = {
        { 0x116C7057u, 0x216C3057u, 0x116C4057u, 0x216C5057u, 0x116C6057u }, /* 1.0 */
        { 0x316C7057u, 0x316C3057u, 0x216C4057u, 0x316C5057u, 0x216C6057u }, /* 1.5 */
        { 0x416C7057u, 0x416C3057u, 0x316C4057u, 0x416C5057u, 0x316C6057u }, /* 2.0 */
        { 0x516C7057u, 0x416C3057u, 0x316C4057u, 0x516C5057u, 0x316C6057u }, /* 2.1 */
    };
    /* The chain, TDI end first. Step 1.x boards have the first ten. */
    static const struct { int asic, kind, id; } chain[17] = {
        { 1, 0, 0 }, { 1, 1, 1 }, { 1, 2, 2 }, { 1, 3, 3 }, { 0, -1, 0 },
        { 1, 4, 4 }, { 1, -1, 4 }, { 0, -1, 0 }, { 0, -1, 0 }, { 0, -1, 0 },
        { 1, -1, 3 }, { 0, -1, 0 }, { 1, -1, 4 }, { 1, -1, 4 }, { 0, -1, 0 },
        { 0, -1, 0 }, { 0, -1, 0 },
    };
    unsigned s = step == 0x15 ? 1 : step == 0x20 ? 2 : step == 0x21 ? 3 : 0;
    unsigned i;

    memset(g_dev, 0, sizeof g_dev);
    memset(g_modeword, 0, sizeof g_modeword);
    g_ndev = step >= 0x20 ? 17u : 10u;
    for (i = 0; i < g_ndev; i++) {
        g_dev[i].asic = chain[i].asic;
        g_dev[i].kind = chain[i].kind;
        g_dev[i].idcode = chain[i].asic ? ids[s][chain[i].id] : 0;
        dev_reset(&g_dev[i]);
    }
    g_state = TLR;
    g_tck = 0;
    g_tdo = 0;
}

void jtag_write(uint8_t v)
{
    unsigned tck = (v >> 6) & 1u, tms = (v >> 2) & 1u, tdi = (v >> 5) & 1u;
    unsigned trst = (v >> 7) & 1u, i;

    if (!trst) {                     /* TRST is active low */
        for (i = 0; i < g_ndev; i++) dev_reset(&g_dev[i]);
        g_state = TLR;
        return;
    }
    if (tck == g_tck)
        return;
    g_tck = (uint8_t)tck;

    if (tck) {                       /* rising edge: act, then move on */
        switch (g_state) {
        case TLR:    for (i = 0; i < g_ndev; i++) dev_reset(&g_dev[i]); break;
        case CAP_DR: for (i = 0; i < g_ndev; i++) capture_dr(&g_dev[i]); break;
        case CAP_IR: for (i = 0; i < g_ndev; i++) capture_ir(&g_dev[i]); break;
        case SH_DR:
        case SH_IR:
            for (i = 0; i < g_ndev; i++) tdi = shift_one(&g_dev[i], tdi);
            break;
        default: break;
        }
        g_state = k_next[g_state][tms];
    } else {                         /* falling edge: TDO changes, updates land */
        switch (g_state) {
        case SH_DR:
        case SH_IR:
            g_tdo = (uint8_t)(g_dev[g_ndev - 1].shift & 1u);
            break;
        case UPD_DR: for (i = 0; i < g_ndev; i++) update_dr(&g_dev[i]); break;
        case UPD_IR: for (i = 0; i < g_ndev; i++) update_ir(&g_dev[i]); break;
        default: break;
        }
    }
}

uint8_t jtag_read(void) { return (uint8_t)(g_tdo << 5); }

uint32_t jtag_modeword(unsigned kind) { return kind < 5 ? g_modeword[kind] : 0; }
