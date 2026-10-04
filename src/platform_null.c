/* model3recomp -- headless platform layer.
 *
 * Used when SDL2 is not available, and by the conformance harness, which
 * wants frames to tick as fast as possible with nothing on screen. The
 * framebuffer is real, so renderers can be tested against it by checksum
 * without a window.
 */
#include "model3recomp/platform.h"
#include "model3recomp/model3recomp.h"

#include <stdlib.h>
#include <time.h>

static uint32_t *g_fb;
static int g_w, g_h;
static uint64_t g_virtual_us;

/* Frames are not paced against a wall clock here: a headless run should go as
 * fast as it can, and a conformance run must be deterministic. Instead the
 * virtual clock advances a tick per query, so a field falls due after a fixed
 * amount of guest work.
 *
 * Returning "a field is always due" instead -- which this did at first -- is
 * not merely fast, it is broken: irq_tick() runs from func_table_call(), so
 * the guest takes an interrupt on every single indirect call, its main loop
 * never gets to run between them, and the game ticks frames forever without
 * making any progress. The symptom was 17,000 fields rendered and not one
 * byte written to tilemap VRAM. */
#define FIELD_US 17385

int platform_init(int w, int h, const char *title)
{
    (void)title;
    g_w = w; g_h = h;
    g_fb = calloc((size_t)w * h, sizeof(uint32_t));
    g_virtual_us = 0;
    return g_fb != NULL;
}

void platform_shutdown(void) { free(g_fb); g_fb = NULL; }

uint32_t *platform_framebuffer(int *w, int *h)
{
    if (w) *w = g_w;
    if (h) *h = g_h;
    return g_fb;
}

int platform_poll(void) { return 1; }

int platform_pointer(int *x, int *y) { (void)x; (void)y; return 0; }

void platform_present(void) { }

/* Pacing off calls alone stalls: the guest spends long stretches polling
 * devices without dispatching through a pointer, so fields stop arriving
 * entirely. m3_work counts both. */
uint64_t platform_ticks_us(void)
{
    g_virtual_us = m3_work;
    return g_virtual_us;
}

/* Headless input, for a harness.
 *
 *   M3_BUTTONS   a mask held down for the whole run
 *   M3_COIN_AT   field number at which to press coin 1 for a moment
 *   M3_START_AT  likewise for start 1
 *
 * The timed ones exist because a coin slot is an edge, not a level: a game
 * counts the transition, so a mask held from the first field to the last may
 * be seen once, or not at all if the guest was not looking yet.
 */
#define PRESS_FIELDS 8u

unsigned platform_buttons(void)
{
    static int probed;
    static unsigned held;
    static unsigned long coin_at, start_at;
    unsigned b;
    unsigned long f;

    if (!probed) {
        const char *e;
        probed = 1;
        e = getenv("M3_BUTTONS");
        held = e ? (unsigned)strtoul(e, NULL, 0) : 0u;
        e = getenv("M3_COIN_AT");
        coin_at = e ? strtoul(e, NULL, 0) : 0u;
        e = getenv("M3_START_AT");
        start_at = e ? strtoul(e, NULL, 0) : 0u;
    }
    b = held;
    f = (unsigned long)model3recomp_frame_count();
    if (coin_at && f >= coin_at && f < coin_at + PRESS_FIELDS)
        b |= M3_BTN_COIN1;
    if (start_at && f >= start_at && f < start_at + PRESS_FIELDS)
        b |= M3_BTN_START1 | M3_BTN_TRIG1;
    return b;
}
