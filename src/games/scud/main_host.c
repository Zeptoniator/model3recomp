/* Scud Race on a desktop: read the ROM images from a directory, set the
 * board up and enter at the reset vector.
 *
 *   scudrace [FIELDS] [--roms DIR]
 *
 * FIELDS stops after that many fields and writes shot.ppm; with it the run
 * is a test harness, without it a game. */
#include "scud.h"
#include "model3recomp/tilegen.h"
#include "model3recomp/bus.h"
#include "model3recomp/real3d.h"
#include "model3recomp/lift.h"
#include "scudpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

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

/* m3_roms_t.read, both CROMs and the VROM from their files (M3_ROM_PAGED). With
 * M3_ROM_ORDER=<file> it also records, once each, the 32 KB pages in the
 * order the game first touches them: tools/scud/pack.py lays SCUD.DAT out
 * in that order, so a Dreamcast reads the disc sequentially. */
static FILE *g_romf[3], *g_order;
static uint8_t g_seen[3][2048];

static size_t read_rom(void *ctx, int which, uint32_t off, void *dst, size_t len)
{
    FILE *f;
    (void)ctx;
    if (which < 0 || which > 2 || !(f = g_romf[which])) return 0;
    if (g_order && len > 8 && !g_seen[which][(off >> 15) & 2047]) {
        g_seen[which][(off >> 15) & 2047] = 1;
        fprintf(g_order, "%d %u %llu\n", which, off >> 15,
                (unsigned long long)model3recomp_frame_count());
    }
    if (fseek(f, (long)off, SEEK_SET)) return 0;
    return fread(dst, 1, len, f);
}

/* M3_SINK_COUNT=1: the Real3D scene goes to a sink that only counts what
 * it is handed, the path the Dreamcast's PVR back end takes. */
static unsigned g_sink_polys[2], g_sink_last[2];
static void sink_begin(void) { g_sink_polys[0] = g_sink_polys[1] = 0; }
static void sink_poly(const m3_r3d_vertex *v, int n, uint32_t argb, int tr,
                      const m3_r3d_texture *tex)
{
    (void)v; (void)n; (void)argb; (void)tex;
    g_sink_polys[tr ? 1 : 0]++;
}
static void sink_end(void)
{
    g_sink_last[0] = g_sink_polys[0];
    g_sink_last[1] = g_sink_polys[1];
}
static const m3_r3d_sink k_count_sink = { sink_begin, sink_poly, sink_end, NULL };

/* M3_TG_MAP_CHECK=1: build the four persistent 512x512 layer pictures used
 * by the PVR back end, compose them in map space on the host, and compare
 * every active field with the established screen-space renderer. This checks
 * incremental name-table updates as well as scroll, line scroll, stencil,
 * depth and priority changes over a complete scripted race. Colour offsets
 * are skipped because the Dreamcast deliberately falls back to screen-space
 * rendering for those fields. */
static void check_tilegen_maps(void)
{
    enum { MAP_W = 512, OUT_W = 512, OUT_H = 384 };
    static uint16_t *maps[4], *got, *ref;
    static int enabled = -1, ready;
    static unsigned passes, bad;
    int above, layer;

    if (enabled < 0) enabled = getenv("M3_TG_MAP_CHECK") != NULL;
    if (!enabled || tilegen_colour_offset()) return;
    if (!ready) {
        for (layer = 0; layer < 4; layer++)
            maps[layer] = (uint16_t *)calloc(MAP_W * MAP_W, sizeof(uint16_t));
        got = (uint16_t *)calloc(OUT_W * OUT_H, sizeof(uint16_t));
        ref = (uint16_t *)calloc(OUT_W * OUT_H, sizeof(uint16_t));
        for (layer = 0; layer < 4; layer++)
            if (!maps[layer]) break;
        if (layer != 4 || !got || !ref) {
            fprintf(stderr, "[tgmapcheck] allocation failed\n");
            model3recomp_quit(1);
            enabled = 0;
            return;
        }
        ready = 1;
    }

    for (layer = 0; layer < 4; layer++)
        tilegen_update_map_1555(layer, maps[layer], passes == 0);

    for (above = 0; above < 2; above++) {
        int y;
        memset(got, 0, OUT_W * OUT_H * sizeof(uint16_t));
        for (y = 0; y < OUT_H; y++) {
            uint32_t stencil = tilegen_stencil(y);
            int pair;
            for (pair = 1; pair >= 0; pair--) {
                unsigned half = pair ? (stencil & 0xFFFFu) : (stencil >> 16);
                int want[2], sx[2] = {0, 0}, sy[2] = {0, 0}, k, x;
                for (k = 0; k < 2; k++) {
                    layer = pair * 2 + k;
                    want[k] = tilegen_layer_drawn(layer, above);
                    if (want[k]) tilegen_line_scroll(layer, y, &sx[k], &sy[k]);
                }
                for (x = 0; x < 496; x++) {
                    uint16_t px;
                    k = ((half >> (15 - x / 32)) & 1u) ? 0 : 1;
                    if (!want[k]) continue;
                    layer = pair * 2 + k;
                    px = maps[layer][(size_t)sy[k] * MAP_W + ((x + sx[k]) & 511)];
                    if (px) got[(size_t)y * OUT_W + x] = px;
                }
            }
        }
        tilegen_render_pass_1555(above, ref, OUT_W);
        for (y = 0; y < OUT_H; y++) {
            if (memcmp(got + (size_t)y * OUT_W, ref + (size_t)y * OUT_W,
                       496u * sizeof(uint16_t))) {
                if (bad++ < 20)
                    fprintf(stderr, "[tgmapcheck] field %llu pass %d line %d differs\n",
                            (unsigned long long)model3recomp_frame_count(), above, y);
                break;
            }
        }
        passes++;
        if (passes % 2000u == 0)
            fprintf(stderr, "[tgmapcheck] %u passes compared, %u differ\n", passes, bad);
    }
}

static void on_field(void)
{
    check_tilegen_maps();
    if (getenv("M3_TG_CHECK")) {
        /* The Dreamcast's 2D path, both ways, compared (tilegen.c). */
        static uint16_t buf[512 * 384];
        tilegen_render_pass_1555(0, buf, 512);
        tilegen_render_pass_1555(1, buf, 512);
    }
    if (getenv("M3_TG_COUNT")) {
        extern uint32_t g_tg_vram_writes, g_tg_reg_writes, g_tg_reg_changes;
        {
            static uint32_t last_gen;
            static unsigned changed_fields;
            uint32_t gen = bus_tilegen_generation();
            changed_fields += gen != last_gen;
            last_gen = gen;
            if (model3recomp_frame_count() % 100 == 0) {
                fprintf(stderr, "[tg] field %llu: vram writes %u, reg writes %u, "
                        "reg changes %u (last field); 2D changed in %u of the "
                        "last 100 fields\n",
                        (unsigned long long)model3recomp_frame_count(),
                        g_tg_vram_writes, g_tg_reg_writes, g_tg_reg_changes,
                        changed_fields);
                changed_fields = 0;
            }
        }
        g_tg_vram_writes = g_tg_reg_writes = g_tg_reg_changes = 0;
    }
    if (getenv("M3_SINK_COUNT") && model3recomp_frame_count() % 100 == 0)
        fprintf(stderr, "[sink] field %llu: %u opaque, %u translucent\n",
                (unsigned long long)model3recomp_frame_count(),
                g_sink_last[0], g_sink_last[1]);
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

#if defined(__SSE__) || defined(__x86_64__)
    /* M3_FTZ=1: flush denormals to zero, inputs and results, as the SH-4
     * does with FPSCR.DN set -- to tell a Dreamcast divergence that comes
     * from denormals from one that doesn't. */
    if (getenv("M3_FTZ"))
        _mm_setcsr(_mm_getcsr() | 0x8040u);
#endif

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
        cfg.roms.vrom_size = pack.vrom_bytes;    /* 0 in a version-1 pack */
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
        snprintf(path, sizeof path, "%s/scud_vrom.bin", g_roms_dir);
        if ((g_romf[2] = fopen(path, "rb")) != NULL) {
            fseek(g_romf[2], 0, SEEK_END);
            cfg.roms.vrom_size = (size_t)ftell(g_romf[2]);
        }
        cfg.roms.read = read_rom;
        if (getenv("M3_ROM_ORDER"))
            g_order = fopen(getenv("M3_ROM_ORDER"), "w");
    } else {
        cfg.roms.crom_bank = slurp("scud_bank.bin", &n, 0);
        if (cfg.roms.crom_bank) cfg.roms.crom_bank_size = n;
    }
    if (!g_romf[2] && !cfg.roms.vrom_size) {
        cfg.roms.vrom = slurp("scud_vrom.bin", &n, 0);
        if (cfg.roms.vrom) cfg.roms.vrom_size = n;
    }
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
    if (getenv("M3_SINK_COUNT"))
        real3d_set_sink(&k_count_sink);
    model3recomp_set_frame_hook(on_field);
    scud_register_lifted();
    model3recomp_run();
    model3recomp_shutdown();
    return 0;
}
