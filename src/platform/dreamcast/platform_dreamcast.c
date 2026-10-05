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
    if (st->buttons & CONT_START) in->buttons |= M3_BTN_START1;
    if (st->buttons & CONT_A)     in->buttons |= M3_BTN_COIN1;
    if (st->buttons & CONT_Y)     in->buttons |= M3_BTN_SERVICE;
}

void platform_set_status(const char *s)
{
    dbglog(DBG_INFO, "[status] %s\n", s);
}

void platform_audio(const int16_t *lr, int frames) { (void)lr; (void)frames; }
