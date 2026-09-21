/* model3recomp -- library entry points and frame pacing. */
#include "model3recomp/model3recomp.h"
#include "model3recomp/platform.h"
#include "model3recomp/ppc.h"
#include "model3recomp/tilegen.h"
#include "model3recomp/real3d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static m3_config_t g_cfg;
static int         g_running;
static uint64_t    g_last_field_us;

/* The board runs at 57.52 Hz, not 60 -- 496x384 at the Model 3's dot clock.
 * Using 60 here makes every attract-mode animation run 4% fast, which is the
 * sort of thing nobody notices until the music desyncs. */
#define FIELD_US 17385

int model3recomp_init(const m3_config_t *cfg)
{
    g_cfg = *cfg;
    if (!g_cfg.width)  g_cfg.width  = 496;
    if (!g_cfg.height) g_cfg.height = 384;

    bus_init(&g_cfg.roms);
    func_table_init();
    irq_init();
    ppc_reset();

    if (!platform_init(g_cfg.width, g_cfg.height,
                       g_cfg.title ? g_cfg.title : "model3recomp"))
        return 0;

    g_last_field_us = m3_work;
    g_running = 1;
    return 1;
}

void model3recomp_shutdown(void)
{
    platform_shutdown();
    bus_shutdown();
    g_running = 0;
}

int model3recomp_field_due(void)
{
    uint64_t now;
    /* irq_tick() runs from func_table_call(), which a test or a tool can
     * reach without ever bringing the board up. No window, no frames. */
    if (!g_running)
        return 0;

    /* Fields are paced by how much work the guest has done, not by the
     * wall clock, and the two are not interchangeable.
     *
     * This used to ask the platform layer for the time. Headless, that
     * returns m3_work and everything was fine. Against SDL it returns real
     * microseconds, so fields arrived sixty times a second whether or not
     * the guest had got anywhere -- and a recompiled 603e uploading three
     * megabytes of texture does not run at real time. The guest was handed
     * a fraction of the instructions per field that it needs and sat on the
     * Sega logo for thirty thousand fields, while the same build headless
     * walked into attract mode.
     *
     * A host that wants to run at the right speed throttles in
     * platform_present(); it does not get to decide when the guest's video
     * hardware retires a frame. */
    now = m3_work;
    if (now - g_last_field_us < FIELD_US)
        return 0;
    g_last_field_us = now;
    return 1;
}

void model3recomp_begin_frame(void) { }

static uint64_t g_frames;
static void (*g_frame_hook)(void);

uint64_t model3recomp_frame_count(void) { return g_frames; }

void model3recomp_set_frame_hook(void (*hook)(void)) { g_frame_hook = hook; }

void model3recomp_end_frame(void)
{
    g_frames++;
    if (getenv("M3_TRACE_FRAMES"))
        fprintf(stderr, "[model3recomp] field %llu\n",
                (unsigned long long)g_frames);

    /* Back to front: the tilemap layers the guest has put behind the 3D,
     * then the 3D, then the ones it has put in front. Which is which is a
     * field in the tilegen's register 0x20, and until it was read the sky
     * was drawn over the dinosaur. */
    {
        int fw = 0, fh = 0;
        uint32_t *fb = platform_framebuffer(&fw, &fh);
        if (fb) memset(fb, 0, (size_t)fw * (size_t)fh * sizeof(uint32_t));
    }
    tilegen_render_pass(0);
    real3d_render();
    tilegen_render_pass(1);
    platform_present();
    if (g_frame_hook)
        g_frame_hook();
    if (!model3recomp_poll())
        g_running = 0;
}

int model3recomp_poll(void) { return platform_poll(); }

void model3recomp_run(void)
{
    /* Every Model 3 title starts at the PowerPC reset vector. Unlike the i960
     * on Model 2 there is no relocation hop to follow: the 603e simply fetches
     * from 0xFFF00100 with MSR[IP] set. */
    if (!func_table_call(0xFFF00100u)) {
        fprintf(stderr, "[model3recomp] reset vector 0xFFF00100 was not "
                        "lifted -- nothing to run\n");
        return;
    }
    /* Reached only if the guest's init returns, which a running game never
     * does. Keep presenting so the host does not appear hung. */
    while (g_running)
        model3recomp_end_frame();
}
