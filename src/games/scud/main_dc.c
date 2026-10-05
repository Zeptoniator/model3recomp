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

static void report(const char *when)
{
    struct mallinfo mi = mallinfo();
    printf("[scud] %s: field %llu, %llu ms, heap %lu KB, Real3D %u KB, "
           "ROM pages missed %llu, disc reads %llu (%llu ms)\n", when,
           (unsigned long long)model3recomp_frame_count(),
           (unsigned long long)((timer_us_gettime64() - g_t0) / 1000),
           (unsigned long)mi.uordblks >> 10,
           (unsigned)(bus_real3d_bytes() >> 10),
           (unsigned long long)bus_rom_cache_misses(),
           (unsigned long long)g_pack.reads,
           (unsigned long long)(g_rom_us / 1000));
}

static void on_field(void)
{
    uint64_t f = model3recomp_frame_count();
    scud_operator_settings();
    if (f % 100 == 0)
        report("progress");
    if (f > SCUD_DC_FIELDS) {          /* after field N's signature */
        report("end");
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
    cfg.roms.read = read_rom;
    cfg.roms.read_ctx = &g_pack;

    if (!model3recomp_init(&cfg)) {
        printf("model3recomp_init failed\nSCUD DC FAILED\n");
        return 1;
    }
    model3recomp_set_frame_hook(on_field);
    scud_register_lifted();
    g_t0 = timer_us_gettime64();
    report("start");
    printf("SCUD BOOT\n");
    model3recomp_run();
    report("guest returned");
    printf("SCUD DC DONE\n");
    return 0;
}
