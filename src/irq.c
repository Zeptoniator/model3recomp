/* model3recomp -- interrupt controller and the frame boundary.
 *
 * Read docs/technical/execution-model.md first.
 *
 * The original design here hung the frame off the guest's read of the
 * interrupt status register at 0xF0100018, the way model2recomp hangs it off
 * a video-status poll. That was wrong, and the interpreter proved it: The
 * Lost World's main loop never reads 0xF0100018 at all. It looks like this:
 *
 *     0x00118284  lwz  r0, -0x126C(r29)    ; handler pointer in RAM
 *                 mtlr r0 ; blrl           ; call the service routine
 *     0x00118290  lbz  r0, 1(r30)          ; flag byte at RAM 0x6ED
 *                 cmpwi cr1, r0, 0
 *     0x001182A0  bc   0x00118284          ; spin until the flag changes
 *
 * Only the VBlank handler sets that flag. So interrupts have to be delivered
 * on a timer, not in response to a poll -- which is what the hardware does
 * anyway. The runtime gets control at every func_table_call, and a main loop
 * that waits on an interrupt-set flag necessarily dispatches through one, so
 * that is where the field advances.
 */
#include "model3recomp/savestate.h"
#include "model3recomp/irq.h"
#include "model3recomp/func_table.h"
#include "model3recomp/bus.h"
#include "model3recomp/model3recomp.h"

#include <stdio.h>
#include <stdlib.h>

#define MSR_EE 0x00008000u
#define MSR_IP 0x00000040u

static uint32_t g_pending;
static uint32_t g_enable;
static uint64_t g_field;
static int      g_in_dispatch;

void m3_irq_init(void)
{
    g_pending = 0;
    g_enable  = 0;
    g_field   = 0;
    g_in_dispatch = 0;
}

int irq_in_dispatch(void)          { return g_in_dispatch; }

static int g_scsi_on, g_scsi_pending;
void irq_set_scsi_enabled(int on)  { g_scsi_on = on; }
void irq_raise_scsi(void)          { if (g_scsi_on) g_scsi_pending = 1; }
void irq_raise(uint32_t bits)      { g_pending |= bits; }
void irq_ack(uint32_t bits)        { g_pending &= ~bits; }
uint32_t irq_pending(void)         { return g_pending; }
uint32_t irq_enable_read(void)     { return g_enable; }
void irq_enable_write(uint32_t v)  { g_enable = v; }

/* Advance the field if one is due, and deliver the interrupt.
 *
 * Safe to call from anywhere lifted code reaches the runtime. Dispatching
 * from inside a call looks alarming but is exactly what the hardware does --
 * an interrupt lands between two instructions -- and the guest's handler
 * saves and restores every register itself (stmw/lmw around the body), so
 * there is nothing of the interrupted function's state for us to preserve.
 * Our own lifted code keeps its temporaries in C locals on the host stack,
 * which the guest cannot touch.
 */
/* ---- The decrementer ----------------------------------------------------
 *
 * SPR 22 counts down, and the 0 -> -1 transition takes an exception at vector
 * 0x900. It is a separate exception from the external interrupt at 0x500 and
 * is gated only by MSR[EE], not by the interrupt controller's enable mask.
 *
 * The Lost World needs it. Its timing calibration at 0x00118340 unmasks a
 * source, then waits for a counter at RAM 0x001C10D0 to change; the only code
 * that writes that counter is the handler the game installs at 0x900, and the
 * pointer to it appears nowhere in RAM because the processor, not the game,
 * is what calls it. Without a decrementer the wait cannot ever end, and both
 * the interpreter and the recompiled binary sit in it forever.
 *
 * ponytail: the countdown runs off m3_work, the same clock the field runs
 * off, scaled so a field is about the right number of ticks. Real hardware
 * counts at the bus clock over four. Upgrade path: drive the time base from
 * m3_work too, so a guest that measures one against the other gets a
 * self-consistent answer rather than two unrelated clocks. */
#define DEC_PER_WORK 16u

static uint64_t g_dec_work;

static void dec_dispatch(void)
{
    uint32_t vec_base, saved_msr;

    if (g_in_dispatch)
        return;
    if (!(m3_ctx.msr & MSR_EE))
        return;
    { static int once; if (!once) { once = 1;
        fprintf(stderr, "[dec] first dispatch: guest r1=%08X (counter lives at "
                        "001C10D0; frame spans r1-0x100 .. r1)\n", m3_ctx.r[1]); } }

    vec_base = (m3_ctx.msr & MSR_IP) ? 0xFFF00000u : 0x00000000u;
    saved_msr = m3_ctx.msr;
    m3_ctx.spr[26] = 0;                /* SRR0 */
    m3_ctx.spr[27] = saved_msr;        /* SRR1 */
    m3_ctx.msr &= ~MSR_EE;

    g_in_dispatch = 1;
    {
        /* The interrupted code may be between a return and its check. */
        uint32_t ret_to = m3_ret_to;
        m3_ret_to = 0;
        func_table_call(vec_base + 0x900u);
        m3_ret_to = ret_to;
    }
    g_in_dispatch = 0;

    m3_ctx.msr = saved_msr;
}

static void dec_tick(void)
{
    uint64_t now = m3_work;
    uint32_t before, after;
    uint64_t elapsed;

    if (now <= g_dec_work)
        return;
    {
        static int probed;
        static uint32_t rate;
        if (!probed) {
            const char *e = getenv("M3_DEC_RATE");
            probed = 1;
            rate = e ? (uint32_t)strtoul(e, NULL, 0) : DEC_PER_WORK;
        }
        elapsed = (now - g_dec_work) * (uint64_t)rate;
    }
    g_dec_work = now;

    before = m3_ctx.spr[M3_SPR_DEC];
    after  = before - (uint32_t)elapsed;
    m3_ctx.spr[M3_SPR_DEC] = after;

    /* Only the 0 -> -1 crossing. Once it has gone negative it stays there
     * until the handler reloads it, which is what the hardware does too. */
    if (!(before & 0x80000000u) && (after & 0x80000000u))
        dec_dispatch();
}

void irq_tick(void)
{
    if (g_in_dispatch)
        return;
    dec_tick();
    if (g_scsi_pending)
        irq_dispatch();
    if (!model3recomp_field_due())
        return;

    g_field++;

    /* Only the start-of-VBlank line is raised. Raising the end line in the
     * same tick as well -- which this did at first -- leaves the guest seeing
     * 0x03000000 pending where the hardware would show 0x02000000, because
     * the two are separate events at opposite ends of the blanking interval.
     * The differential trace against tools/ppc_interp.py caught it. */
    irq_raise(M3_IRQ_VBLANK_START);
    irq_dispatch();

    /* Render after the guest's handler, not before it.
     *
     * A game draws its frame from inside the VBlank handler, and this one
     * clears the tilemap first and prints into it second. Presenting before
     * the handler runs therefore catches the buffer in the gap between the
     * clear and the print, and the screen is black however correct the
     * renderer is -- the console's own cursor had advanced through 887
     * characters that were never once seen.
     *
     * On hardware the tilemap is scanned out continuously and the guest draws
     * during blanking precisely so the picture is whole when it is not. One
     * render per field belongs at the same place: after the drawing. */
    model3recomp_end_frame();          /* render and present */
    model3recomp_begin_frame();
}

/* Kept as the routing for a read of 0xF0100018: a guest that *does* poll the
 * controller still gets a field out of it. */
uint32_t irq_status_read(void)
{
    irq_tick();
    return g_pending;
}

/* Walk the guest's own path. The vector base follows MSR[IP], which the game
 * clears once it has installed handlers in RAM, so the address is derived
 * rather than cached -- a game that moves its handlers keeps working, and one
 * the lifter missed shows up as a named func-table miss. */
int irq_dispatch(void)
{
    uint32_t vec_base, saved_msr;
    int ran = 0;

    if (g_in_dispatch)
        return 0;
    if (!(g_pending & g_enable) && !g_scsi_pending)
        return 0;
    if (!(m3_ctx.msr & MSR_EE))        /* guest has interrupts masked */
        return 0;
    g_scsi_pending = 0;                /* this dispatch serves it */

    vec_base = (m3_ctx.msr & MSR_IP) ? 0xFFF00000u : 0x00000000u;

    /* What the processor hands the handler. It reads these immediately
     * (mfspr r31,SRR0 / mfspr r30,SRR1) and restores them before its rfi. */
    saved_msr = m3_ctx.msr;
    m3_ctx.spr[26] = 0;                /* SRR0: no meaningful resume address */
    m3_ctx.spr[27] = saved_msr;        /* SRR1 */
    m3_ctx.msr &= ~MSR_EE;             /* EE off for the duration */

#ifdef M3_LOOP_GUARD
    {
        /* M3_WHERE=N: every N fields, the guest function the VBlank
         * interrupted -- where the main program is, as opposed to the
         * handler, which is what every other report sees. */
        extern uint32_t m3_fn_now;
        static long every = -1;
        if (every < 0) {
            const char *e = getenv("M3_WHERE");
            every = e ? strtol(e, NULL, 0) : 0;
        }
        if (every > 0 && g_field % (uint64_t)every == 0)
            fprintf(stderr, "[where] field %llu: interrupted %08X (LR %08X)\n",
                    (unsigned long long)g_field, m3_fn_now, m3_ctx.lr);
    }
#endif
    g_in_dispatch = 1;
    {
        /* The interrupted code may be between a return and its check
         * (M3_RETURNED in lift.h); the handler must not answer for it. */
        uint32_t ret_to = m3_ret_to;
        m3_ret_to = 0;
        if (!func_table_call(vec_base + 0x500))
            fprintf(stderr, "[model3recomp] no handler lifted at %08X "
                            "(external interrupt)\n", vec_base + 0x500);
        else
            ran++;
        m3_ret_to = ret_to;
    }
    g_in_dispatch = 0;

    m3_ctx.msr = saved_msr;            /* the guest's rfi became a return */
    return ran;
}

void irq_state(m3_state_t *st)
{
    M3_STATE_VAR(st, g_pending);
    M3_STATE_VAR(st, g_enable);
    M3_STATE_VAR(st, g_field);
    M3_STATE_VAR(st, g_in_dispatch);
    M3_STATE_VAR(st, g_dec_work);
    M3_STATE_VAR(st, g_scsi_pending);
}
