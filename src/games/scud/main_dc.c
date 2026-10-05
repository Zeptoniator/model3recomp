/* Scud Race on a Dreamcast, headless (milestone DC-03).
 *
 * The CROM stays on the disc, in SCUD.DAT (tools/scud/pack.py, built from
 * your own romset), read through the bus's page cache. No
 * sound board, no battery file. Everything is reported on the serial
 * console: frame signatures (the same lines a desktop prints with
 * M3_SIG_EVERY, so the two can be diffed), memory, ROM traffic. */
#include "scud.h"
#include "model3recomp/bus.h"
#include "scudpack.h"
#include "../../renderer/pvr/pvr_r3d.h"

#include <kos.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SCUD_DC_FIELDS
#define SCUD_DC_FIELDS 600
#endif
#ifndef SCUD_DC_SIG_EVERY
#define SCUD_DC_SIG_EVERY "100"
#endif
#ifndef SCUD_DC_SHOT_AT
#define SCUD_DC_SHOT_AT 1300     /* prints "SCUD SHOT" for the harness to photograph */
#endif
#ifndef M3_GIT_REV
#define M3_GIT_REV "unknown"
#endif

static scudpack_t g_pack;
static uint64_t g_t0, g_rom_us;   /* time spent reading the disc */

/* SCUD.DAT through the bus's page cache; see scudpack.c. */
static size_t read_rom(void *ctx, int which, uint32_t off, void *dst, size_t len)
{
    uint64_t t = timer_us_gettime64();
    size_t got = scudpack_read(ctx, which, off, dst, len);
    g_rom_us += timer_us_gettime64() - t;
    return got;
}

#ifdef SCUD_DC_PROFILE
/* A sampling profiler: a second thread wakes every few milliseconds and
 * reads the program counter the main thread was preempted at (its saved
 * context). Counts per 16-byte bucket; the end of the run prints the top
 * ones, to be named with sh-elf-addr2line against scudrace.elf. Samples are
 * taken from SCUD_DC_PROFILE_FROM on. */
#ifndef SCUD_DC_PROFILE_FROM
#define SCUD_DC_PROFILE_FROM 450
#endif
#define PROF_SLOTS 8192
static kthread_t *g_main;
static uint32_t g_prof_pc[PROF_SLOTS], g_prof_n[PROF_SLOTS], g_prof_total;
static volatile int g_prof_on = 1;

static void *profiler(void *arg)
{
    (void)arg;
    while (g_prof_on) {
        thd_sleep(2);
        if (model3recomp_frame_count() < SCUD_DC_PROFILE_FROM) continue;
        {
            uint32_t pc = g_main->context.pc & ~15u, h = (pc >> 4) % PROF_SLOTS, k;
            for (k = 0; k < PROF_SLOTS; k++, h = (h + 1) % PROF_SLOTS) {
                if (g_prof_pc[h] == pc || !g_prof_n[h]) {
                    g_prof_pc[h] = pc; g_prof_n[h]++; g_prof_total++;
                    break;
                }
            }
        }
    }
    return NULL;
}

static void profile_report(void)
{
    int top, i, best;
    g_prof_on = 0;
    printf("[prof] %lu samples\n", (unsigned long)g_prof_total);
    for (top = 0; top < 80; top++) {
        best = -1;
        for (i = 0; i < PROF_SLOTS; i++)
            if (g_prof_n[i] && (best < 0 || g_prof_n[i] > g_prof_n[best])) best = i;
        if (best < 0) break;
        printf("[prof] %08lX %lu\n", (unsigned long)g_prof_pc[best],
               (unsigned long)g_prof_n[best]);
        g_prof_n[best] = 0;
    }
}
#endif

static void report(const char *when)
{
    struct mallinfo mi = mallinfo();
    printf("[scud] %s: field %llu, %llu ms, heap %lu KB, Real3D %u KB, "
           "ROM pages missed %llu (VROM %llu), disc reads %llu (%llu ms)\n",
           when,
           (unsigned long long)model3recomp_frame_count(),
           (unsigned long long)((timer_us_gettime64() - g_t0) / 1000),
           (unsigned long)mi.uordblks >> 10,
           (unsigned)(bus_real3d_bytes() >> 10),
           (unsigned long long)bus_rom_cache_misses(),
           (unsigned long long)bus_vrom_cache_misses(),
           (unsigned long long)g_pack.reads,
           (unsigned long long)(g_rom_us / 1000));
}

static void on_field(void)
{
    uint64_t f = model3recomp_frame_count();
    scud_operator_settings();
    if (f % 100 == 0) {
        report("progress");
        printf("[pvr] last frame: %u opaque, %u punch-through, %u translucent"
               " polygons; textures %u made, %u refused, %u KB\n",
               pvr_r3d_last_opaque(), pvr_r3d_last_punch(),
               pvr_r3d_last_translucent(), pvr_r3d_textures_made(),
               pvr_r3d_textures_failed(), pvr_r3d_texture_kb());
    }
    if (f == SCUD_DC_SHOT_AT)
        printf("SCUD SHOT field %llu\n", (unsigned long long)f);
    if (f > SCUD_DC_FIELDS) {          /* after field N's signature */
        report("end");
#ifdef SCUD_DC_PROFILE
        profile_report();
#endif
        printf("SCUD DC DONE\n");
        fflush(stdout);
        exit(0);
    }
}

int main(void)
{
    m3_config_t cfg;

    printf("================================\n");
    printf(" SCUD RACE -- DREAMCAST (headless)\n");
    printf("================================\n");
    printf("model3recomp %s, built %s %s\n", M3_GIT_REV, __DATE__, __TIME__);
    setenv("M3_NO_SOUND", "1", 1);
    setenv("M3_SIG_EVERY", SCUD_DC_SIG_EVERY, 1);
#ifdef SCUD_DC_SIGMAP
    setenv("M3_SIGMAP", SCUD_DC_SIGMAP, 1);
#endif

    memset(&cfg, 0, sizeof cfg);
    scud_board_config(&cfg);
    if (!scudpack_open(&g_pack, "/cd/SCUD.DAT", 8)) {
        printf("no /cd/SCUD.DAT (tools/scud/pack.py)\nSCUD DC FAILED\n");
        return 1;
    }
    printf("SCUD.DAT: %lu pages, %lu hot first, read 8 at a time\n",
           (unsigned long)g_pack.count, (unsigned long)g_pack.hot);
    cfg.roms.crom_size = g_pack.prog_bytes;
    cfg.roms.crom_bank_size = g_pack.bank_bytes;
    /* The models the Real3D draws are in VROM; a version-1 pack has none,
     * and then nothing is drawn. */
    cfg.roms.vrom_size = g_pack.vrom_bytes;
    cfg.roms.read = read_rom;
    cfg.roms.read_ctx = &g_pack;

    if (!model3recomp_init(&cfg)) {
        printf("model3recomp_init failed\nSCUD DC FAILED\n");
        return 1;
    }
    if (!pvr_r3d_install())
        printf("PVR: init failed, running headless\n");
    else
        printf("PVR: Real3D sink installed\n");
    model3recomp_set_frame_hook(on_field);
    scud_register_lifted();
#ifdef SCUD_DC_PROFILE
    g_main = thd_get_current();
    thd_create(1, profiler, NULL);
#endif
    g_t0 = timer_us_gettime64();
    report("start");
    printf("SCUD BOOT\n");
    model3recomp_run();
    report("guest returned");
    printf("SCUD DC DONE\n");
    return 0;
}
