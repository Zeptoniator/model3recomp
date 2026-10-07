/* model3recomp -- Sega Model 3 hardware runtime for static recompilation.
 *
 * Link a statically recompiled Model 3 game against this and you have a
 * board. Your game project supplies the lifted PowerPC functions and a
 * main(); this library supplies everything they talk to.
 *
 * Title-agnostic: nothing here knows what game it is running.
 */
#ifndef MODEL3RECOMP_H
#define MODEL3RECOMP_H

#include <stdint.h>

#include "model3recomp/ppc.h"
#include "model3recomp/bus.h"
#include "model3recomp/input.h"
#include "model3recomp/func_table.h"
#include "model3recomp/irq.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Model 3 came in Steps. They differ in CPU clock, graphics revision and how
 * data reaches the Real3D; Step 1.x uses the 53C810 SCSI DMA engine for it. */
typedef enum {
    M3_STEP_1_0 = 0x10,
    M3_STEP_1_5 = 0x15,
    M3_STEP_2_0 = 0x20,
    M3_STEP_2_1 = 0x21
} m3_step_t;

typedef struct m3_config {
    m3_step_t   step;
    m3_roms_t   roms;
    int         width, height;   /* 0,0 -> 496x384, the board's native mode */
    const char *title;

    /* Optional, all of them; a game fills in what it supports. */
    const char *ini_path;        /* window and control settings            */
    const char *nvram_path;      /* battery RAM, loaded at boot, saved on quit */
    const char *state_prefix;    /* save states go to <prefix>.<slot>.m3s   */
    uint32_t    safepoint_pc;    /* see savestate.h; 0 means no save states */

    /* Where the board puts the 53C810. It is a PCI device and the ROM board
     * decides its window: 0xC1000000 on The Lost World, 0xF9000000 on Scud
     * Race (MAME's init_scud and init_lostwsga). 0 means 0xC1000000. */
    uint32_t    scsi_base;

    /* Raise the 53C810's interrupt when a SCRIPTS program stops (INT, or a
     * single step). On the board it is non-maskable (Supermodel gives it a
     * bit outside the 8-bit mask) and it is how Scud Race learns a transfer
     * finished; without it the game waits for the next VBlank, spinning,
     * after every transfer. Off by default: The Lost World was brought up
     * without it and has not been re-checked with it. */
    int         scsi_irq;

    /* The drive/analog cabinet's inputs (Supermodel's model). With adc
     * set, register 0x3C of the input block is an 8-channel ADC: a write
     * selects a channel, every read returns that channel and moves to the
     * next. Its channels come from m3_input_t.adc. Off, the old behaviour
     * stays (all ones, or M3_ADC). */
    int         adc;
    /* What the cabinet reads when nobody touches it: a wheel centred
     * (0x80), pedals up (0). Copied into each field's record before the
     * host's controls are sampled. */
    uint8_t     adc_rest[8];
    /* Turns the host's pad (m3_input_t.pad) into this cabinet's inputs --
     * a wheel, pedals, a gear lever. Called once per field after sampling;
     * NULL for a game whose platform mapping is enough. */
    void      (*input_map)(m3_input_t *in);
} m3_config_t;

int  model3recomp_init(const m3_config_t *cfg);
void model3recomp_shutdown(void);
const m3_config_t *model3recomp_config(void);

/* Save battery RAM, leave a netplay session, close the window and exit.
 * The guest never returns from its main loop, so this is how a run ends. */
void model3recomp_quit(int code);
void model3recomp_save_nvram(void);

/* Frame pacing. These exist and work, but for a game that owns its own loop
 * they are called BY irq_status_read(), not by your main(). Keep a loop over
 * them in main() only as a fallback for a guest whose init returns early. */
int  model3recomp_field_due(void);
void model3recomp_begin_frame(void);
void model3recomp_end_frame(void);

/* Pumps host events and returns 0 when the user has asked to quit. */
int  model3recomp_poll(void);

/* Fields presented since init. */
uint64_t model3recomp_frame_count(void);

/* Called after every field is presented. For screenshots, tracing and
 * conformance capture -- a Model 3 main loop never returns, so this is the
 * only place a host gets a look in. NULL clears it. */
void model3recomp_set_frame_hook(void (*hook)(void));

/* Write the current framebuffer to a binary PPM. Returns 0 on failure. */
int  model3recomp_screenshot(const char *path);

/* Write Real3D scene memory and tilemap VRAM to <prefix>_cull_lo.bin and
 * friends. The renderer is developed against these, so that what it parses is
 * what a game really wrote rather than what a document claims. */
int  model3recomp_dump_scene(const char *prefix);

/* A monotonically increasing measure of how much work the guest has done:
 * dispatched calls and device accesses. The headless platform paces fields
 * off it, because there is no wall clock to pace against and lifted code has
 * no instruction count to read. */
extern uint64_t m3_work;

/* Wall-clock microseconds of the host (the Dreamcast's timer, CLOCK_MONOTONIC
 * elsewhere) -- unlike platform_ticks_us(), which paces fields off m3_work
 * on hosts without a display. For measuring, never for pacing. */
uint64_t m3_now_us(void);
/* Accumulated by the parts of a field M3_PROFILE breaks out: disc reads,
 * waiting for the PVR, drawing the 2D layers. */
extern uint64_t m3_t_disc_us, m3_t_pvrwait_us, m3_t_2d_us;

/* Boot. Resets the CPU and enters the guest at its reset vector, which for
 * every Model 3 title is 0xFFF00100. Does not return while the game runs --
 * Model 3 main loops never do. */
void model3recomp_run(void);

#ifdef __cplusplus
}
#endif
#endif /* MODEL3RECOMP_H */
