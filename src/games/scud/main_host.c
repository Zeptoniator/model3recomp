/* Scud Race on a desktop: read the ROM images from a directory, set the
 * board up and enter at the reset vector.
 *
 *   scudrace [FIELDS] [--roms DIR]
 *
 * FIELDS stops after that many fields and writes shot.ppm; with it the run
 * is a test harness, without it a game. */
#include "scud.h"
#include "model3recomp/bus.h"
#include "model3recomp/lift.h"
#include "scudpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_roms_dir = "roms";
static uint64_t g_stop_after;

static uint8_t *slurp(const char *name, size_t *len, int required)
{
    char path[512];
    FILE *f;
    long n;
    uint8_t *p;

    snprintf(path, sizeof path, "%s/%s", g_roms_dir, name);
    f = fopen(path, "rb");
    if (!f) {
        if (required) perror(path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "%s: short read\n", path);
        fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return p;
}

/* M3_SHOT_EVERY=N[,DIR]: shot_<field>.ppm every N fields, to watch a run
 * go by without stopping it. */
static void shots(void)
{
    static long every = -1;
    static char dir[256];
    uint64_t f = model3recomp_frame_count();
    char path[512];

    if (every < 0) {
        const char *e = getenv("M3_SHOT_EVERY");
        const char *c = e ? strchr(e, ',') : NULL;
        every = e ? strtol(e, NULL, 0) : 0;
        snprintf(dir, sizeof dir, "%s", c ? c + 1 : ".");
    }
    if (every <= 0 || f % (uint64_t)every)
        return;
    snprintf(path, sizeof path, "%s/shot_%06llu.ppm", dir, (unsigned long long)f);
    model3recomp_screenshot(path);
}

/* m3_roms_t.read, both CROMs from their files (M3_ROM_PAGED). With
 * M3_ROM_ORDER=<file> it also records, once each, the 32 KB pages in the
 * order the game first touches them: tools/scud/pack.py lays SCUD.DAT out
 * in that order, so a Dreamcast reads the disc sequentially. */
static FILE *g_romf[2], *g_order;
static uint8_t g_seen[2][2048];

static size_t read_rom(void *ctx, int which, uint32_t off, void *dst, size_t len)
{
    FILE *f;
    (void)ctx;
    if (which < 0 || which > 1 || !(f = g_romf[which])) return 0;
    if (g_order && len > 8 && !g_seen[which][(off >> 15) & 2047]) {
        g_seen[which][(off >> 15) & 2047] = 1;
        fprintf(g_order, "%d %u %llu\n", which, off >> 15,
                (unsigned long long)model3recomp_frame_count());
    }
    if (fseek(f, (long)off, SEEK_SET)) return 0;
    return fread(dst, 1, len, f);
}

static void on_field(void)
{
    scud_operator_settings();
    if (getenv("M3_ROM_PAGED") && model3recomp_frame_count() % 500 == 0)
        fprintf(stderr, "[rom] field %llu: %llu pages fetched\n",
                (unsigned long long)model3recomp_frame_count(),
                (unsigned long long)bus_rom_cache_misses());
    shots();
    if (!g_stop_after || model3recomp_frame_count() < g_stop_after)
        return;
    model3recomp_screenshot("shot.ppm");
#ifdef M3_LOOP_GUARD
    m3_fn_report(40);
#endif
    if (getenv("M3_DROPPED")) {
        bus_report_dropped_writes();
        bus_report_device_reads();
    }
    if (getenv("M3_DUMP_SCENE"))
        model3recomp_dump_scene(getenv("M3_DUMP_SCENE"));
    fprintf(stderr, "captured shot.ppm after %llu fields\n",
            (unsigned long long)model3recomp_frame_count());
    model3recomp_quit(0);
}

int main(int argc, char **argv)
{
    m3_config_t cfg;
    size_t n = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--roms") && i + 1 < argc)
            g_roms_dir = argv[++i];
        else
            g_stop_after = strtoull(argv[i], NULL, 0);
    }

    memset(&cfg, 0, sizeof cfg);
    scud_board_config(&cfg);

    cfg.roms.crom = slurp("scud_prog.bin", &n, 1);
    if (!cfg.roms.crom) {
        fprintf(stderr, "No program ROM. Build the images from your own "
                        "romset:\n    python tools/scud/build_roms.py "
                        "/path/to/scud.zip roms/\n");
        return 2;
    }
    cfg.roms.crom_size = n;
    if (getenv("M3_SCUD_PACK")) {
        /* The CROM out of SCUD.DAT, the way the Dreamcast reads it. */
        static scudpack_t pack;
        free((void *)cfg.roms.crom);
        cfg.roms.crom = NULL;
        if (!scudpack_open(&pack, getenv("M3_SCUD_PACK"), 8)) return 2;
        cfg.roms.crom_size = pack.prog_bytes;
        cfg.roms.crom_bank_size = pack.bank_bytes;
        cfg.roms.read = scudpack_read;
        cfg.roms.read_ctx = &pack;
    } else if (getenv("M3_ROM_PAGED")) {
        /* Both CROMs through the bus's page cache, as a Dreamcast reads
         * them -- for checking that path against the in-memory one. */
        static const char *const names[2] = { "scud_prog.bin", "scud_bank.bin" };
        size_t *sizes[2] = { &cfg.roms.crom_size, &cfg.roms.crom_bank_size };
        char path[512];
        int k;
        free((void *)cfg.roms.crom);
        cfg.roms.crom = NULL;
        for (k = 0; k < 2; k++) {
            snprintf(path, sizeof path, "%s/%s", g_roms_dir, names[k]);
            if ((g_romf[k] = fopen(path, "rb")) != NULL) {
                fseek(g_romf[k], 0, SEEK_END);
                *sizes[k] = (size_t)ftell(g_romf[k]);
            }
        }
        cfg.roms.read = read_rom;
        if (getenv("M3_ROM_ORDER"))
            g_order = fopen(getenv("M3_ROM_ORDER"), "w");
    } else {
        cfg.roms.crom_bank = slurp("scud_bank.bin", &n, 0);
        if (cfg.roms.crom_bank) cfg.roms.crom_bank_size = n;
    }
    cfg.roms.vrom = slurp("scud_vrom.bin", &n, 0);
    if (cfg.roms.vrom) cfg.roms.vrom_size = n;
    cfg.roms.sndrom = slurp("scud_snd.bin", &n, 0);
    if (cfg.roms.sndrom) cfg.roms.sndrom_size = n;
    cfg.roms.samples = slurp("scud_samples.bin", &n, 0);
    if (cfg.roms.samples) cfg.roms.samples_size = n;

    cfg.ini_path   = "scudrace.ini";
    cfg.nvram_path = "scudrace.nv";

    if (!model3recomp_init(&cfg)) {
        fprintf(stderr, "model3recomp_init failed\n");
        return 1;
    }
    model3recomp_set_frame_hook(on_field);
    scud_register_lifted();
    model3recomp_run();
    model3recomp_shutdown();
    return 0;
}
