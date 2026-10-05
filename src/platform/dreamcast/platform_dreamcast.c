/* model3recomp -- KallistiOS platform layer (Sega Dreamcast).
 *
 * The board runtime on a Dreamcast. Fields are paced off guest work, as
 * headless (platform_null.c): a recompiled 603e on a 200 MHz SH-4 does not
 * keep up with a 57.5 Hz field, and handing the guest fewer instructions per
 * field than it needs changes what the game does, not just how fast.
 *
 * What there is so far: the serial log, the clock, the first pad's buttons.
 * There is no framebuffer: the Real3D goes to the PVR (renderer/pvr, not
 * written yet), and a 496x384x32 software framebuffer is 760 KB of main RAM
 * the budget does not have (docs/dreamcast/MEMORY_BUDGET.md). Returning NULL
 * from platform_framebuffer() is how the software renderers are told to
 * stand down.
 */
#include "model3recomp/platform.h"
#include "model3recomp/model3recomp.h"

#include <kos.h>

int platform_init(int w, int h, const char *title)
{
    dbglog(DBG_INFO, "[platform] Dreamcast, %dx%d board mode, \"%s\"\n",
           w, h, title ? title : "");
    return 1;
}

void platform_shutdown(void) { }

uint32_t *platform_framebuffer(int *w, int *h)
{
    if (w) *w = 0;
    if (h) *h = 0;
    return NULL;
}

int platform_poll(void) { return 1; }

void platform_present(void) { }

/* Work, not wall time: see the note at the top. timer_us_gettime64() is for
 * the profiler, which wants real microseconds -- see dc_time_us(). */
uint64_t platform_ticks_us(void) { return m3_work; }

uint64_t dc_time_us(void) { return timer_us_gettime64(); }

/* The first controller. Start is start; A and B are coins, until the
 * driving controls (steering, pedals through the I/O board's ADC) have a
 * place in m3_input_t. */
void platform_sample(m3_input_t *in)
{
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st;

    if (!dev || !(st = (cont_state_t *)maple_dev_status(dev)))
        return;
    /* The pad as it is, for the game's input_map (a wheel and pedals for
     * a driving game). With no input_map, Start is start and Y a coin. */
    {
        m3_pad_t *p = &in->pad[0];
        uint32_t b = st->buttons;
        int jx = st->joyx, jy = st->joyy;
        p->present = 1;
        p->buttons = (uint16_t)(((b & CONT_A) ? M3_PAD_A : 0) | ((b & CONT_B) ? M3_PAD_B : 0)
                   | ((b & CONT_X) ? M3_PAD_X : 0) | ((b & CONT_Y) ? M3_PAD_Y : 0)
                   | ((b & CONT_START) ? M3_PAD_START : 0)
                   | ((b & CONT_DPAD_UP) ? M3_PAD_UP : 0) | ((b & CONT_DPAD_DOWN) ? M3_PAD_DOWN : 0)
                   | ((b & CONT_DPAD_LEFT) ? M3_PAD_LEFT : 0) | ((b & CONT_DPAD_RIGHT) ? M3_PAD_RIGHT : 0));
        p->sx = (int8_t)(jx < -128 ? -128 : jx > 127 ? 127 : jx);
        p->sy = (int8_t)(jy < -128 ? -128 : jy > 127 ? 127 : jy);
        p->lt = (uint8_t)st->ltrig;
        p->rt = (uint8_t)st->rtrig;
    }
    if (!model3recomp_config() || !model3recomp_config()->input_map) {
        if (st->buttons & CONT_START) in->buttons |= M3_BTN_START1;
        if (st->buttons & CONT_Y)     in->buttons |= M3_BTN_COIN1;
    }
}

void platform_set_status(const char *s)
{
    dbglog(DBG_INFO, "[status] %s\n", s);
}

void platform_audio(const int16_t *lr, int frames) { (void)lr; (void)frames; }
