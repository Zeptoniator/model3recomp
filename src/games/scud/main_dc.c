/* Scud Race on a Dreamcast, headless (milestone DC-03).
 *
 * The CROM stays on the disc: scud_prog.bin and scud_bank.bin, built by
 * tools/scud/build_roms.py from your own romset and put on the disc by
 * tools/dreamcast/make-cdi.sh, are read through the bus's page cache. No
 * sound board, no battery file. Everything is reported on the serial
 * console: frame signatures (the same lines a desktop prints with
 * M3_SIG_EVERY, so the two can be diffed), memory, ROM traffic. */
#include "scud.h"
#include "model3recomp/bus.h"

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

static FILE *g_rom[2];
static uint64_t g_t0;

static size_t read_rom(void *ctx, int which, uint32_t off, void *dst, size_t len)
{
    (void)ctx;
    if (which < 0 || which > 1 || !g_rom[which]) return 0;
    if (fseek(g_rom[which], (long)off, SEEK_SET)) return 0;
    return fread(dst, 1, len, g_rom[which]);
}

static size_t open_rom(int which, const char *path)
{
    long n;
    g_rom[which] = fopen(path, "rb");
    if (!g_rom[which]) { printf("cannot open %s\n", path); return 0; }
    fseek(g_rom[which], 0, SEEK_END);
    n = ftell(g_rom[which]);
    printf("ROM %-22s %ld bytes (paged, not loaded)\n", path, n);
    return n > 0 ? (size_t)n : 0;
}

static void report(const char *when)
{
    struct mallinfo mi = mallinfo();
    printf("[scud] %s: field %llu, %llu ms, heap %lu KB, Real3D %u KB, "
           "ROM pages read %llu\n", when,
           (unsigned long long)model3recomp_frame_count(),
           (unsigned long long)((timer_us_gettime64() - g_t0) / 1000),
           (unsigned long)mi.uordblks >> 10,
           (unsigned)(bus_real3d_bytes() >> 10),
           (unsigned long long)bus_rom_cache_misses());
}

static void on_field(void)
{
    uint64_t f = model3recomp_frame_count();
    scud_operator_settings();
    if (f % 100 == 0)
        report("progress");
    if (f >= SCUD_DC_FIELDS) {
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
    cfg.roms.crom_size = open_rom(0, "/cd/scud_prog.bin");
    cfg.roms.crom_bank_size = open_rom(1, "/cd/scud_bank.bin");
    if (!cfg.roms.crom_size) {
        printf("no program ROM on the disc\nSCUD DC FAILED\n");
        return 1;
    }
    cfg.roms.read = read_rom;

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
