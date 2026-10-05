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

static void on_field(void)
{
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
    cfg.roms.crom_bank = slurp("scud_bank.bin", &n, 0);
    if (cfg.roms.crom_bank) cfg.roms.crom_bank_size = n;
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
