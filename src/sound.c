/* model3recomp -- the sound board. See sound.h.
 *
 * Wiring from MAME's sega/model3.cpp (BSD-3-Clause; R. Belmont, Ville
 * Linde): the 68000's map, the sample bank, and the hack it uses for the
 * PowerPC's sound interrupt.
 *
 *   68000   0x000000  512 KB RAM, also SCSP 1's sample RAM
 *           0x100000  SCSP 1 registers
 *           0x200000  512 KB RAM, also SCSP 2's sample RAM
 *           0x300000  SCSP 2 registers
 *           0x400001  bank select: bit 4 picks the 8 MB sample bank
 *           0x600000  the program, 512 KB
 *           0x800000  the sample ROM, banked
 *
 * The PowerPC talks to SCSP 1's MIDI port through an i8251 UART. Here the
 * UART is two byte queues: what the PowerPC writes goes to SCSP 1 at the
 * line's own rate (31250 baud, a byte every 14 samples), and what SCSP 1
 * sends back waits for the PowerPC to read it.
 */
#include "model3recomp/sound.h"
#include "model3recomp/model3recomp.h"
#include "model3recomp/platform.h"
#include "model3recomp/irq.h"
#include "model3recomp/savestate.h"
#include "scsp.h"
#include "m68k/m68k.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef M3_DREAMCAST
#include <malloc.h>
#endif

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <pthread.h>
#endif

#define SND_RAM      0x80000u
#define RATE         44100
#define CYCLES       256            /* 11 289 600 Hz / 44 100 */
/* The 68000's cycles per sample, as run: CYCLES on a desktop; fewer on a
 * host that cannot afford it (M3_SND_CYCLES, or SND_CYCLES at build time).
 * The driver spends most of its time polling its voices, so a slower
 * 68000 does the same work with fewer turns of that loop. */
#ifndef SND_CYCLES
#define SND_CYCLES CYCLES
#endif
static unsigned g_cycles = SND_CYCLES;
#define BYTE_SAMPLES 14             /* 31250 baud, 10 bits a byte */
#define QSIZE        256

static const uint8_t *g_prog, *g_samples;
static size_t g_prog_size, g_samples_size;
static uint8_t *g_ram1, *g_ram2;
static uint32_t g_ram_local = SND_RAM;      /* bytes of each RAM kept here */
static uint32_t g_mirror_from;              /* writes below this stay here only */
static const sound_voices_t *g_voices;

/* ---- ROMs read on demand (sound_init_paged) ----------------------------
 * Two caches, least recently used out first. The driver's code and tables
 * are a few dozen KB scattered over its 512 KB program: 4 KB pages, enough
 * of them to hold it all, so the samples it streams can never push it out
 * (sharing one cache, they did: 57,000 misses in a race). The samples are
 * read in runs as the driver copies them, ~5 KB a field, several looping
 * streams at once: sixteen 16 KB pages (eight missed 1,834 times in a race,
 * sixteen 298 -- a replay of the race's reads, M3_SND_PAGETRACE). */
typedef struct {
    uint32_t page, shift, slots, last, clock;
    uint8_t *buf;
    uint32_t tag[32], age[32];
    uint64_t misses;
} rom_cache_t;
static size_t (*g_rom_read)(void *, uint32_t, void *, size_t);
static void *g_rom_ctx;
static rom_cache_t g_rc_prog = { .page = 0x1000u, .shift = 12u, .slots = 24u, .last = ~0u };
static rom_cache_t g_rc_smp = { .page = 0x4000u, .shift = 14u, .slots = 16u, .last = ~0u };
uint64_t g_snd_rom_misses;

#if defined(__GNUC__) || defined(__clang__)
#define SND_NOINLINE __attribute__((noinline))
#else
#define SND_NOINLINE
#endif

/* Fill or select the page at off. Kept out of the hit path: the 68000 asks
 * for an instruction word every few host instructions, and almost every one
 * is in the page it just used. Entering the out-of-line cache routine for
 * that case was a measurable part of the interpreter's cost on the SH-4. */
static SND_NOINLINE const uint8_t *cache_miss(rom_cache_t *c, uint32_t off)
{
    uint32_t page = off >> c->shift, k, old = 0;
    for (k = 0; k < c->slots; k++) {
        if (c->tag[k] == page) break;
        if (c->age[k] < c->age[old]) old = k;
    }
    if (k == c->slots) {
        k = old;
        c->tag[k] = page;
        g_snd_rom_misses++;
        c->misses++;
        if (g_rom_read(g_rom_ctx, page << c->shift, c->buf + (k << c->shift), c->page) != c->page)
            memset(c->buf + (k << c->shift), 0xFF, c->page);
    }
    c->age[k] = ++c->clock;
    c->last = k;
    return c->buf + (k << c->shift) + (off & (c->page - 1u));
}

static inline const uint8_t *cache_at(rom_cache_t *c, uint32_t off)
{
    uint32_t k = c->last;
    if (k < c->slots && c->tag[k] == off >> c->shift)
        return c->buf + (k << c->shift) + (off & (c->page - 1u));
    return cache_miss(c, off);
}

static unsigned rom8(uint32_t off)          /* program at 0, samples after */
{
    if (!g_rom_read)
        return off < g_prog_size ? g_prog[off] : g_samples[off - g_prog_size];
    return *(off < g_prog_size ? cache_at(&g_rc_prog, off) : cache_at(&g_rc_smp, off));
}

/* A program word (off even): what the 68000 fetches its instructions as. */
static unsigned prog16(uint32_t off)
{
    const uint8_t *p = g_rom_read ? cache_at(&g_rc_prog, off) : g_prog + off;
    return ((unsigned)p[0] << 8) | p[1];
}

static unsigned prog32(uint32_t off)
{
    const uint8_t *p;
    if (!g_rom_read) p = g_prog + off;
    else if ((off & (g_rc_prog.page - 1u)) <= g_rc_prog.page - 4u)
        p = cache_at(&g_rc_prog, off);
    else
        return (prog16(off) << 16) | prog16(off + 2u);
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16)
         | ((unsigned)p[2] << 8) | p[3];
}
static scsp_t *g_scsp1, *g_scsp2;

static struct {
    uint32_t bank;                  /* sample ROM offset of the window */
    uint32_t irq_lines;             /* 68000 levels SCSP 1 holds up */
    uint8_t  tx[QSIZE]; unsigned tx_r, tx_w;   /* PowerPC -> SCSP 1 */
    uint8_t  rx[QSIZE]; unsigned rx_r, rx_w;   /* SCSP 1 -> PowerPC */
    unsigned tx_clock;
    uint64_t sample_acc;            /* field -> sample count, exactly */
} g_s;

/* The PowerPC's side of the UART. It is touched only by the PowerPC's
 * thread, and trades bytes with the board's side (g_s.tx / g_s.rx) only at
 * a field boundary -- which is what lets the board run a field behind on
 * its own thread and still be deterministic: what the guest reads depends
 * on the field count and what it sent, never on how far the board got. */
static struct {
    uint8_t  tx[QSIZE]; unsigned tx_r, tx_w;   /* waiting to go to the board */
    uint8_t  rx[QSIZE]; unsigned rx_r, rx_w;   /* arrived from the board */
    int      ppc_irq;
} g_p;

int sound_present(void) { return g_scsp1 != NULL; }

/* Decoupled: the board only listens. The PowerPC sees exactly the board
 * that is not there (sound_null.c: always ready, never a reply), and the
 * bytes it sends are still played. Its signatures are then those of a run
 * without sound, on any host -- which is how a port whose sound is made
 * some other way (the Dreamcast's AICA) stays comparable with this one.
 * M3_SND_DECOUPLED=1 here. */
static int g_decoupled;

/* M3_SND_LOOPBACK=1: the chips' voices played by a second pair of chips,
 * through scsp_set_voices() -- the path the Dreamcast's AICA takes, with
 * this machine's synthesis on the far side of it. The first pair only
 * counts time. The audio should be the original's. */
static scsp_t *g_far1, *g_far2;
static void far_slot(void *ctx, int slot, int word, uint16_t v)
{
    scsp_write16((scsp_t *)ctx, (uint32_t)(slot * 0x20 + word * 2), v, 0xFFFF);
}
static void far_common(void *ctx, int reg, uint16_t v)
{
    /* The voices' side of the common registers: master volume, the DSP's
     * ring buffer. Timers, interrupts, MIDI and DMA stay with the first. */
    if (reg < 0x04) scsp_write16((scsp_t *)ctx, 0x400u + (uint32_t)reg, v, 0xFFFF);
}
static void far_dsp(void *ctx, uint32_t addr, uint16_t v)
{
    scsp_write16((scsp_t *)ctx, addr, v, 0xFFFF);
}
static uint16_t far_monitor(void *ctx, int slot) { return scsp_monitor((scsp_t *)ctx, slot); }
static scsp_voices_t g_far_ops1, g_far_ops2;

#ifdef M3_SND_STATS
/* A diagnostic build (-DM3_SND_STATS, with Musashi's instruction hook
 * pointed at snd_stat_pc): what the board does in a field, for sizing a
 * port of it onto another machine. Every 300 fields: 68000 instructions
 * run, the addresses most run, sample ROM bytes read, and what the voices
 * keyed on use (scsp.c). Run with M3_SOUND_THREAD=0. */
static uint64_t g_st_insns, g_st_rom;
static void stat_ram_report(void);
static uint32_t *g_st_hist;         /* per 68000 word address, 512 KB program */
void snd_stat_pc(unsigned pc)
{
    g_st_insns++;
    pc &= 0xFFFFFF;
    if (!g_st_hist) g_st_hist = (uint32_t *)calloc(0x100000, sizeof *g_st_hist);
    if (g_st_hist && pc >= 0x600000 && pc < 0x680000) g_st_hist[(pc - 0x600000) >> 1]++;
    else if (g_st_hist && pc < 0x80000) g_st_hist[0x40000 + (pc >> 1)]++;
}
static void stat_report(void)
{
    static uint64_t fields;
    unsigned i, k, c;
    if (++fields % 300) return;
    fprintf(stderr, "[sndstat] fields %llu: 68000 %.0f insns/field, ROM %.0f bytes/field\n",
            (unsigned long long)model3recomp_frame_count(), g_st_insns / 300.0, g_st_rom / 300.0);
    for (k = 0; k < 8 && g_st_hist; k++) {
        unsigned best = 0;
        for (i = 1; i < 0x80000; i++) if (g_st_hist[i] > g_st_hist[best]) best = i;
        if (!g_st_hist[best]) break;
        fprintf(stderr, "[sndstat]   pc %06X %.0f/field\n",
                best < 0x40000 ? 0x600000 + best * 2 : (best - 0x40000) * 2,
                g_st_hist[best] / 300.0);
        g_st_hist[best] = 0;
    }
    for (c = 0; c < 2; c++) {
        const scsp_stats_t *t = &scsp_stats[c];
        unsigned pages = 0;
        for (i = 0; i < 256; i++) pages += t->pages[i];
        fprintf(stderr, "[sndstat]   scsp%u: %u key-ons since boot (pcm8 %u, fm %u, lfo %u, to dsp %u,"
                        " noise %u, looped %u); voices reach %u KB, play %u KB of it;"
                        " dsp program %u words\n", c + 1, t->keyons, t->pcm8, t->fm,
                t->lfo, t->dsp_send, t->noise, t->looped, t->max_end >> 10, pages * 4,
                scsp_dsp_steps(c ? g_scsp2 : g_scsp1));
        fprintf(stderr, "[sndstat]   scsp%u low pages played:", c + 1);
        for (i = 0; i < 16; i++) if (t->pages[i]) fprintf(stderr, " %u", i);
        fprintf(stderr, "\n");
        fprintf(stderr, "[sndstat]   scsp%u DISDL at key-on: %u %u %u %u %u %u %u %u\n", c + 1,
                t->disdl[0], t->disdl[1], t->disdl[2], t->disdl[3], t->disdl[4], t->disdl[5],
                t->disdl[6], t->disdl[7]);
    }
    fprintf(stderr, "[sndstat]   one-shots that ran out keyed on: %u; paged ROM misses %llu\n",
            scsp_stats_ends, (unsigned long long)g_snd_rom_misses);
    fprintf(stderr, "[sndstat]   misses: program %llu, samples %llu\n",
            (unsigned long long)g_rc_prog.misses, (unsigned long long)g_rc_smp.misses);
    stat_ram_report();
    g_st_insns = g_st_rom = 0;
    if (g_st_hist) memset(g_st_hist, 0, 0x100000 * sizeof *g_st_hist);
}
#define STAT_ROM() (g_st_rom++)
/* The 68000's own reads and writes of the two RAMs, per 4 KB page, since
 * boot: which of them it only fills (samples) and which it works in. */
static uint32_t g_st_rd[2][128], g_st_wr[2][128];
static uint8_t g_st_prog[128], *g_st_srom;   /* 4 KB pages touched, since boot */
#define STAT_RAM(chip, a, w) ((w) ? g_st_wr[chip][(a) >> 12]++ : g_st_rd[chip][(a) >> 12]++)
static void stat_ram_report(void)
{
    unsigned c, i;
    for (c = 0; c < 2; c++) {
        unsigned rd = 0, wr = 0;
        fprintf(stderr, "[sndstat]   ram%u pages read by the 68000:", c + 1);
        for (i = 0; i < 128; i++) {
            if (g_st_rd[c][i]) { rd++; fprintf(stderr, " %02X", i); }
            if (g_st_wr[c][i]) wr++;
        }
        fprintf(stderr, " (%u read, %u written)\n", rd, wr);
    }
    {
        unsigned np = 0, ns = 0;
        for (i = 0; i < 128; i++) np += g_st_prog[i];
        for (i = 0; g_st_srom && i < 4096; i++) ns += g_st_srom[i];
        fprintf(stderr, "[sndstat]   program pages read %u of 128; sample ROM pages read %u (%u KB)\n",
                np, ns, ns * 4);
    }
}
#else
#define STAT_ROM() ((void)0)
#define STAT_RAM(chip, a, w) ((void)0)
#endif

/* ---- 68000 memory ----------------------------------------------------- */
static unsigned rd8(unsigned a)
{
    a &= 0xFFFFFF;
    if (a < 0x080000) {
        STAT_RAM(0, a, 0);
        if (a < g_ram_local) return g_ram1[a];
        return g_voices ? g_voices->ram_read8(0, a) : 0;
    }
    if (a >= 0x200000 && a < 0x280000) {
        a -= 0x200000;
        STAT_RAM(1, a, 0);
        if (a < g_ram_local) return g_ram2[a];
        return g_voices ? g_voices->ram_read8(1, a) : 0;
    }
    if ((a & 0xFFF000) == 0x100000) {
        uint16_t w = scsp_read16(g_scsp1, a & 0xFFF);
        return (a & 1) ? (w & 0xFF) : (w >> 8);
    }
    if ((a & 0xFFF000) == 0x300000) {
        uint16_t w = scsp_read16(g_scsp2, a & 0xFFF);
        return (a & 1) ? (w & 0xFF) : (w >> 8);
    }
    if (a >= 0x600000 && a < 0x680000) {
        a -= 0x600000;
#ifdef M3_SND_STATS
        g_st_prog[(a >> 12) & 127] = 1;
#endif
        return a < g_prog_size ? rom8(a) : 0xFF;
    }
    if (a >= 0x800000 && g_samples_size) {
        size_t o;
        STAT_ROM();
        o = g_s.bank + (a - 0x800000);
        while (o >= g_samples_size) o -= g_samples_size;    /* no division */
#ifdef M3_SND_STATS
        {   /* M3_SND_PAGETRACE=<file>: each change of 1 KB sample block, with the field */
            static FILE *tf; static int probed; static uint32_t lastb = ~0u;
            if (!probed) { const char *e = getenv("M3_SND_PAGETRACE"); probed = 1; if (e) tf = fopen(e, "w"); }
            if (tf && (o >> 10) != lastb) {
                lastb = (uint32_t)(o >> 10);
                fprintf(tf, "%llu %u\n", (unsigned long long)model3recomp_frame_count(), lastb);
            }
        }
        if (!g_st_srom) g_st_srom = (uint8_t *)calloc(4096, 1);
        if (g_st_srom) g_st_srom[(o >> 12) & 4095] = 1;
#endif
        return rom8((uint32_t)(g_prog_size + o));
    }
    return 0xFF;
}

static unsigned rd16(unsigned a)
{
    a &= 0xFFFFFE;
    if ((a & 0xFFF000) == 0x100000) return scsp_read16(g_scsp1, a & 0xFFF);
    if ((a & 0xFFF000) == 0x300000) return scsp_read16(g_scsp2, a & 0xFFF);
    return (rd8(a) << 8) | rd8(a + 1);
}

static void wr8(unsigned a, unsigned v)
{
    a &= 0xFFFFFF;
    if (a < 0x080000) {
        STAT_RAM(0, a, 1);
        if (a < g_ram_local) g_ram1[a] = (uint8_t)v;
        if (g_voices && a >= g_mirror_from) g_voices->ram_write8(0, a, (uint8_t)v);
        return;
    }
    if (a >= 0x200000 && a < 0x280000) {
        a -= 0x200000;
        STAT_RAM(1, a, 1);
        if (a < g_ram_local) g_ram2[a] = (uint8_t)v;
        if (g_voices && a >= g_mirror_from) g_voices->ram_write8(1, a, (uint8_t)v);
        return;
    }
    if ((a & 0xFFF000) == 0x100000 || (a & 0xFFF000) == 0x300000) {
        scsp_t *c = (a & 0xFFF000) == 0x100000 ? g_scsp1 : g_scsp2;
        if (a & 1) scsp_write16(c, a & 0xFFE, (uint16_t)(v & 0xFF), 0x00FF);
        else       scsp_write16(c, a & 0xFFE, (uint16_t)((v & 0xFF) << 8), 0xFF00);
        return;
    }
    if (a == 0x400001) {
        g_s.bank = (v & 0x10) ? 0x800000u : 0u;
        if (g_samples_size) g_s.bank %= (uint32_t)g_samples_size;
        return;
    }
}

static void wr16(unsigned a, unsigned v)
{
    a &= 0xFFFFFE;
    if ((a & 0xFFF000) == 0x100000) { scsp_write16(g_scsp1, a & 0xFFF, (uint16_t)v, 0xFFFF); return; }
    if ((a & 0xFFF000) == 0x300000) { scsp_write16(g_scsp2, a & 0xFFF, (uint16_t)v, 0xFFFF); return; }
    wr8(a, v >> 8);
    wr8(a + 1, v & 0xFF);
}

#ifndef M3_SND_STATS
/* n contiguous bytes of the sample ROM at 68000 address a (0x800000 up,
 * through the bank), or NULL when they wrap round the ROM or cross a page
 * of the cache -- then rd8 takes them a byte at a time. In a race the
 * driver copies ~5 KB a field from here into its chips' RAMs, a long word
 * at a time (movem.l), and each byte went through rd16, rd8 and rom8. */
static const uint8_t *smp_at(uint32_t a, uint32_t n)
{
    size_t o = g_s.bank + (a - 0x800000u);
    uint32_t off;
    while (o >= g_samples_size) o -= g_samples_size;    /* as rd8 */
    if (g_samples_size - o < n) return NULL;
    if (!g_rom_read) return g_samples + o;
    off = (uint32_t)(g_prog_size + o);
    if ((off & (g_rc_smp.page - 1u)) > g_rc_smp.page - n) return NULL;
    return cache_at(&g_rc_smp, off);
}

/* A long word into a chip's RAM above what is kept here: samples, which
 * only the voices hold. In wr8's byte order. */
static void voices_w32(int chip, uint32_t a, uint32_t v)
{
    if (g_voices->ram_write32) { g_voices->ram_write32(chip, a, v); return; }
    g_voices->ram_write8(chip, a, (uint8_t)(v >> 24));
    g_voices->ram_write8(chip, a + 1u, (uint8_t)(v >> 16));
    g_voices->ram_write8(chip, a + 2u, (uint8_t)(v >> 8));
    g_voices->ram_write8(chip, a + 3u, (uint8_t)v);
}
#endif

/* Musashi's callbacks. The interpreter calls these for every operand and
 * instruction fetch. Keep its RAM and program -- overwhelmingly the common
 * cases -- out of the complete board decoder above. M3_SND_STATS deliberately
 * takes the slow path because it counts every access. */
unsigned int m68k_read_memory_8(unsigned int a)
{
#ifndef M3_SND_STATS
    a &= 0xFFFFFFu;
    if (a < g_ram_local) return g_ram1[a];
    if (a - 0x200000u < g_ram_local) return g_ram2[a - 0x200000u];
    if (a - 0x600000u < g_prog_size) return rom8(a - 0x600000u);
    if (a >= 0x800000u && g_samples_size) return *smp_at(a, 1);
#endif
    return rd8(a);
}

unsigned int m68k_read_memory_16(unsigned int a)
{
    a &= 0xFFFFFE;
#ifndef M3_SND_STATS                        /* which counts every access */
    if (a + 1u < g_ram_local) return ((unsigned)g_ram1[a] << 8) | g_ram1[a + 1];
    {
        uint32_t o = a - 0x200000u;
        if (o < g_ram_local && g_ram_local - o >= 2u)
            return ((unsigned)g_ram2[o] << 8) | g_ram2[o + 1];
    }
    if (a - 0x600000u < g_prog_size) return prog16(a - 0x600000u);
    if (a >= 0x800000u && g_samples_size) {
        const uint8_t *p = smp_at(a, 2);
        if (p) return ((unsigned)p[0] << 8) | p[1];
    }
#endif
    return rd16(a);
}
unsigned int m68k_read_memory_32(unsigned int a)
{
#ifndef M3_SND_STATS
    uint32_t o;
    a &= 0xFFFFFEu;
    if (a + 3u < g_ram_local)
        return ((unsigned)g_ram1[a] << 24) | ((unsigned)g_ram1[a + 1] << 16)
             | ((unsigned)g_ram1[a + 2] << 8) | g_ram1[a + 3];
    o = a - 0x200000u;
    if (o < g_ram_local && g_ram_local - o >= 4u)
        return ((unsigned)g_ram2[o] << 24) | ((unsigned)g_ram2[o + 1] << 16)
             | ((unsigned)g_ram2[o + 2] << 8) | g_ram2[o + 3];
    o = a - 0x600000u;
    if (o < g_prog_size && g_prog_size - o >= 4u) return prog32(o);
    if (a >= 0x800000u && a <= 0xFFFFFCu && g_samples_size) {
        const uint8_t *p = smp_at(a, 4);
        if (p) return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16)
                    | ((unsigned)p[2] << 8) | p[3];
    }
#endif
    return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2);
}
void m68k_write_memory_8(unsigned int a, unsigned int v)
{
#ifndef M3_SND_STATS
    a &= 0xFFFFFFu;
    if (a < g_ram_local && (!g_voices || a < g_mirror_from)) {
        g_ram1[a] = (uint8_t)v;
        return;
    }
    if (a - 0x200000u < g_ram_local
        && (!g_voices || a - 0x200000u < g_mirror_from)) {
        g_ram2[a - 0x200000u] = (uint8_t)v;
        return;
    }
    if (g_voices) {                     /* above what is kept: samples */
        if (a >= g_ram_local && a < 0x080000u) {
            g_voices->ram_write8(0, a, (uint8_t)v); return;
        }
        if (a - 0x200000u >= g_ram_local && a - 0x200000u < 0x080000u) {
            g_voices->ram_write8(1, a - 0x200000u, (uint8_t)v); return;
        }
    }
#endif
    wr8(a, v);
}
void m68k_write_memory_16(unsigned int a, unsigned int v)
{
#ifndef M3_SND_STATS
    uint32_t o;
    a &= 0xFFFFFEu;
    if (a + 1u < g_ram_local && (!g_voices || a + 1u < g_mirror_from)) {
        g_ram1[a] = (uint8_t)(v >> 8); g_ram1[a + 1] = (uint8_t)v;
        return;
    }
    o = a - 0x200000u;
    if (o < g_ram_local && g_ram_local - o >= 2u
        && (!g_voices || (o < g_mirror_from && g_mirror_from - o >= 2u))) {
        g_ram2[o] = (uint8_t)(v >> 8); g_ram2[o + 1] = (uint8_t)v;
        return;
    }
    if (g_voices) {
        int chip = -1;
        if (a >= g_ram_local && a + 1u < 0x080000u) { chip = 0; o = a; }
        else if (o >= g_ram_local && o + 1u < 0x080000u) chip = 1;
        if (chip >= 0) {
            g_voices->ram_write8(chip, o, (uint8_t)(v >> 8));
            g_voices->ram_write8(chip, o + 1u, (uint8_t)v);
            return;
        }
    }
#endif
    wr16(a, v);
}
void m68k_write_memory_32(unsigned int a, unsigned int v)
{
#ifndef M3_SND_STATS
    uint32_t o;
    a &= 0xFFFFFEu;
    if (a + 3u < g_ram_local && (!g_voices || a + 3u < g_mirror_from)) {
        g_ram1[a] = (uint8_t)(v >> 24); g_ram1[a + 1] = (uint8_t)(v >> 16);
        g_ram1[a + 2] = (uint8_t)(v >> 8); g_ram1[a + 3] = (uint8_t)v;
        return;
    }
    o = a - 0x200000u;
    if (o < g_ram_local && g_ram_local - o >= 4u
        && (!g_voices || (o < g_mirror_from && g_mirror_from - o >= 4u))) {
        g_ram2[o] = (uint8_t)(v >> 24); g_ram2[o + 1] = (uint8_t)(v >> 16);
        g_ram2[o + 2] = (uint8_t)(v >> 8); g_ram2[o + 3] = (uint8_t)v;
        return;
    }
    if (g_voices) {
        if (a >= g_ram_local && a + 3u < 0x080000u) { voices_w32(0, a, v); return; }
        if (o >= g_ram_local && o + 3u < 0x080000u) { voices_w32(1, o, v); return; }
    }
#endif
    wr16(a, v >> 16); wr16(a + 2, v & 0xFFFF);
}
unsigned int m68k_read_disassembler_8(unsigned int a)  { return rd8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return rd16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

/* ---- interrupts and MIDI ---------------------------------------------- */
/* Musashi asks which vector an interrupt wants: the 68000's autovector.
 * Having a handler at all is the point -- see m68kconf.h. */
int sound_int_ack(int level)
{
    (void)level;
    return M68K_INT_ACK_AUTOVECTOR;
}

/* The SCSP holds a line per level until the driver clears it; the 68000
 * sees the highest one held. */
static void set_68k_irq(void)
{
    int level = 7;
    while (level > 0 && !(g_s.irq_lines & (1u << level))) level--;
    m68k_set_irq((unsigned)level);
}

static void scsp1_irq(int level, int state)
{
    if (level <= 0 || level > 7) return;
    if (state) g_s.irq_lines |= 1u << level;
    else       g_s.irq_lines &= ~(1u << level);
    set_68k_irq();
}

static void scsp1_midi_out(uint8_t b)
{
    if (((g_s.rx_w + 1) % QSIZE) != g_s.rx_r) {
        g_s.rx[g_s.rx_w] = b;
        g_s.rx_w = (g_s.rx_w + 1) % QSIZE;
    }
}

/* ---- the PowerPC's side ----------------------------------------------- */
uint8_t sound_uart_read(unsigned reg)
{
    if (!g_scsp1 || g_decoupled) return reg ? 0x05 : 0;   /* no board: always ready */
    if (reg == 0) {
        uint8_t b = 0;
        if (g_p.rx_r != g_p.rx_w) {
            b = g_p.rx[g_p.rx_r];
            g_p.rx_r = (g_p.rx_r + 1) % QSIZE;
        }
        return b;
    }
    /* i8251 status: TxRDY, RxRDY, TxEMPTY. */
    return (uint8_t)((((g_p.tx_w + 1) % QSIZE) != g_p.tx_r ? 0x01 : 0)
                   | (g_p.rx_r != g_p.rx_w ? 0x02 : 0)
                   | (g_p.tx_r == g_p.tx_w ? 0x04 : 0));
}

void sound_uart_write(unsigned reg, uint8_t v)
{
    if (getenv("M3_SND_TRACE"))
        fprintf(stderr, "[snd] field %llu ppc writes %s %02X\n",
                (unsigned long long)model3recomp_frame_count(), reg ? "ctl" : "data", v);
    if (!g_scsp1 || g_decoupled) {
        /* No board: as sound_null.c (DC-BUG-015). */
        if (reg == 0)
            irq_ack(M3_IRQ_SOUND);
        else
            irq_set_level(M3_IRQ_SOUND, (v & 0x20) != 0);
        /* A decoupled board still hears it; a byte with no room is lost. */
        if (g_scsp1 && reg == 0 && ((g_p.tx_w + 1) % QSIZE) != g_p.tx_r) {
            g_p.tx[g_p.tx_w] = v;
            g_p.tx_w = (g_p.tx_w + 1) % QSIZE;
        }
        return;
    }
    if (reg == 0) {
        /* Sending a byte clears the sound interrupt (MAME). */
        irq_ack(M3_IRQ_SOUND);
        if (g_scsp1 && ((g_p.tx_w + 1) % QSIZE) != g_p.tx_r) {
            g_p.tx[g_p.tx_w] = v;
            g_p.tx_w = (g_p.tx_w + 1) % QSIZE;
        }
        return;
    }
    /* A UART command with bit 5 set turns on the PowerPC's sound interrupt,
     * which MAME raises every millisecond; here it is raised once a field.
     * That is how the guest paces its command stream. */
    g_p.ppc_irq = (v & 0x20) != 0;
    if (!g_p.ppc_irq) irq_ack(M3_IRQ_SOUND);
}

/* ---- running ---------------------------------------------------------- */
void sound_set_voices(const sound_voices_t *v, uint32_t local_ram, uint32_t mirror_from)
{
    g_voices = v;
    g_ram_local = local_ram && local_ram < SND_RAM ? local_ram : SND_RAM;
    g_mirror_from = mirror_from < g_ram_local ? mirror_from : g_ram_local;
}

static void board_init(void)
{
    unsigned i;
    {
        const char *e = getenv("M3_SND_CYCLES");
        if (e && atoi(e) > 0) g_cycles = (unsigned)atoi(e);
    }
    g_ram1 = (uint8_t *)calloc(1, g_ram_local);
    g_ram2 = (uint8_t *)calloc(1, g_ram_local);
    if (!g_ram1 || !g_ram2) return;
    /* The reset vectors are read from RAM at 0, so the board copies them
     * there first (MAME does the same). */
    for (i = 0; i < 16; i++) {
        g_ram1[i] = (uint8_t)rom8(i);
        if (g_voices) g_voices->ram_write8(0, i, g_ram1[i]);
    }
    g_scsp1 = scsp_create(g_ram1, g_ram_local, scsp1_irq, scsp1_midi_out);
    g_scsp2 = scsp_create(g_ram2, g_ram_local, NULL, NULL);
    if (g_voices) {
        scsp_set_voices(g_scsp1, (const scsp_voices_t *)g_voices->chip[0], 1);
        scsp_set_voices(g_scsp2, (const scsp_voices_t *)g_voices->chip[1], 1);
    }
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    g_decoupled = g_voices != NULL || getenv("M3_SND_DECOUPLED") != NULL;
}

void sound_init_paged(size_t (*read)(void *ctx, uint32_t off, void *dst, size_t len),
                      void *ctx, size_t prog_size, size_t samples_size)
{
    rom_cache_t *cs[2] = { &g_rc_prog, &g_rc_smp };
    unsigned i;
    if (!read || prog_size < 16) return;
    for (i = 0; i < 2; i++) {
#ifdef M3_DREAMCAST
        cs[i]->buf = (uint8_t *)memalign(32, cs[i]->slots * cs[i]->page);   /* for the disc's DMA */
#else
        cs[i]->buf = (uint8_t *)malloc(cs[i]->slots * cs[i]->page);
#endif
        if (!cs[i]->buf) return;
        memset(cs[i]->tag, 0xFF, sizeof cs[i]->tag);
    }
    g_rom_read = read; g_rom_ctx = ctx;
    g_prog_size = prog_size; g_samples_size = samples_size;
    board_init();
}

/* M3_SND_PAGED=1: the Dreamcast's arrangement on this machine -- ROMs
 * through the paged reader, 32 KB of each RAM kept here, and the voices
 * played by a far pair of chips that hold the full RAMs and get every byte
 * written to them. If the driver reads back only what is kept, the audio is
 * the original's. */
static const uint8_t *g_mem_prog, *g_mem_samples;
static uint8_t *g_far_ram[2];
static size_t mem_read(void *ctx, uint32_t off, void *dst, size_t len)
{
    size_t i;
    (void)ctx;
    for (i = 0; i < len; i++) {
        size_t o = off + i;
        ((uint8_t *)dst)[i] = o < g_prog_size ? g_mem_prog[o]
                            : o - g_prog_size < g_samples_size ? g_mem_samples[o - g_prog_size] : 0xFF;
    }
    return len;
}
static void far_ram_w8(int chip, uint32_t a, uint8_t v) { g_far_ram[chip][a & (SND_RAM - 1)] = v; }
static uint8_t far_ram_r8(int chip, uint32_t a) { return g_far_ram[chip][a & (SND_RAM - 1)]; }
static void far_dma1(void *ctx, uint32_t a, uint16_t v) { (void)ctx; far_ram_w8(0, a, (uint8_t)(v >> 8)); far_ram_w8(0, a + 1, (uint8_t)v); }
static void far_dma2(void *ctx, uint32_t a, uint16_t v) { (void)ctx; far_ram_w8(1, a, (uint8_t)(v >> 8)); far_ram_w8(1, a + 1, (uint8_t)v); }
static void far_field(void) { }
static void far_ram_w32(int chip, uint32_t a, uint32_t v)
{
    far_ram_w8(chip, a, (uint8_t)(v >> 24)); far_ram_w8(chip, a + 1, (uint8_t)(v >> 16));
    far_ram_w8(chip, a + 2, (uint8_t)(v >> 8)); far_ram_w8(chip, a + 3, (uint8_t)v);
}
static void paged_selftest(const uint8_t *prog, size_t prog_size,
                           const uint8_t *samples, size_t samples_size)
{
    static sound_voices_t v;
    g_mem_prog = prog; g_mem_samples = samples;
    g_far_ram[0] = (uint8_t *)calloc(1, SND_RAM);
    g_far_ram[1] = (uint8_t *)calloc(1, SND_RAM);
    g_far1 = scsp_create(g_far_ram[0], SND_RAM, NULL, NULL);
    g_far2 = scsp_create(g_far_ram[1], SND_RAM, NULL, NULL);
    g_far_ops1 = (scsp_voices_t){ g_far1, far_slot, far_common, far_dsp, far_dma1, far_monitor };
    g_far_ops2 = (scsp_voices_t){ g_far2, far_slot, far_common, far_dsp, far_dma2, far_monitor };
    v.chip[0] = &g_far_ops1; v.chip[1] = &g_far_ops2;
    v.ram_write8 = far_ram_w8; v.ram_read8 = far_ram_r8; v.field = far_field;
    v.ram_write32 = far_ram_w32;
    sound_set_voices(&v, 0x8000, 0x4000);
    g_prog_size = prog_size; g_samples_size = samples ? samples_size : 0;
    sound_init_paged(mem_read, NULL, prog_size, g_samples_size);
}

void sound_init(const uint8_t *prog, size_t prog_size,
                const uint8_t *samples, size_t samples_size)
{
    if (!prog || prog_size < 16 || getenv("M3_NO_SOUND")) return;
    if (getenv("M3_SND_PAGED")) { paged_selftest(prog, prog_size, samples, samples_size); return; }
    g_prog = prog; g_prog_size = prog_size;
    g_samples = samples; g_samples_size = samples ? samples_size : 0;
    board_init();
    if (!g_scsp1) return;
    if (getenv("M3_SND_LOOPBACK")) {
        static const scsp_voices_t ops = { NULL, far_slot, far_common, far_dsp, NULL, far_monitor };
        g_far1 = scsp_create(g_ram1, g_ram_local, NULL, NULL);
        g_far2 = scsp_create(g_ram2, g_ram_local, NULL, NULL);
        g_far_ops1 = ops; g_far_ops1.ctx = g_far1;
        g_far_ops2 = ops; g_far_ops2.ctx = g_far2;
        scsp_set_voices(g_scsp1, &g_far_ops1, 1);
        scsp_set_voices(g_scsp2, &g_far_ops2, 1);
    }
}

/* M3_WAV=<file>: also write the audio to a WAV file, for listening to a
 * scripted run or checking it by eye. The header's sizes are patched in
 * as it grows. */
static void wav_out(const int16_t *lr, unsigned frames)
{
    static FILE *f;
    static int probed;
    static uint32_t bytes;
    if (!probed) {
        const char *e = getenv("M3_WAV");
        probed = 1;
        if (e && (f = fopen(e, "wb")) != NULL) {
            static const uint8_t h[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
                16,0,0,0, 1,0, 2,0, 0x44,0xAC,0,0, 0x10,0xB1,2,0, 4,0, 16,0, 'd','a','t','a',0,0,0,0 };
            fwrite(h, 1, 44, f);
        }
    }
    if (!f) return;
    fwrite(lr, 4, frames, f);        /* little-endian host */
    bytes += frames * 4u;
    {
        uint32_t riff = bytes + 36;
        long here = ftell(f);
        fseek(f, 4, SEEK_SET);  fwrite(&riff, 4, 1, f);
        fseek(f, 40, SEEK_SET); fwrite(&bytes, 4, 1, f);
        fseek(f, here, SEEK_SET);
    }
}

static int16_t clip16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* One field of the board: the samples, the 68000 between them, and the
 * UART's line delivering a byte every 14 samples. */
static void board_field(void)
{
    /* 44100 Hz over the 57.524 Hz field (17385 us): 766.68 samples, kept
     * exact by carrying the remainder. */
    static int16_t buf[2 * 1024];
    unsigned n, i;

    g_s.sample_acc += (uint64_t)RATE * 17385u;
    n = (unsigned)(g_s.sample_acc / 1000000u);
    g_s.sample_acc %= 1000000u;
    if (n > 1024) n = 1024;

    for (i = 0; i < n; i++) {
        int32_t l = 0, r = 0;
        if (++g_s.tx_clock >= BYTE_SAMPLES) {
            g_s.tx_clock = 0;
            if (g_s.tx_r != g_s.tx_w && scsp_midi_in(g_scsp1, g_s.tx[g_s.tx_r]))
                g_s.tx_r = (g_s.tx_r + 1) % QSIZE;
        }
        m68k_execute((int)g_cycles);
        scsp_sample(g_scsp1, &l, &r);
        scsp_sample(g_scsp2, &l, &r);
        if (g_far1) { scsp_sample(g_far1, &l, &r); scsp_sample(g_far2, &l, &r); }
        buf[2 * i] = clip16(l);
        buf[2 * i + 1] = clip16(r);
    }
    platform_audio(buf, (int)n);
    wav_out(buf, n);
    if (g_voices && g_voices->field) g_voices->field();
#ifdef M3_SND_STATS
    stat_report();
#endif
}

/* ---- the board's thread ------------------------------------------------
 * The board costs a few milliseconds a field -- a third of the budget on
 * one core -- so it runs beside the guest: field N's sound is made while
 * the PowerPC runs field N+1. M3_SOUND_THREAD=0 runs it inline instead,
 * which gives the same result, only slower. */
#ifdef _WIN32
static SRWLOCK g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
#  define LOCK()   AcquireSRWLockExclusive(&g_lock)
#  define UNLOCK() ReleaseSRWLockExclusive(&g_lock)
#  define WAIT()   SleepConditionVariableSRW(&g_cv, &g_lock, INFINITE, 0)
#  define WAKE()   WakeAllConditionVariable(&g_cv)
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
#  define LOCK()   pthread_mutex_lock(&g_lock)
#  define UNLOCK() pthread_mutex_unlock(&g_lock)
#  define WAIT()   pthread_cond_wait(&g_cv, &g_lock)
#  define WAKE()   pthread_cond_broadcast(&g_cv)
#endif
static int g_threaded = -1, g_busy;
uint64_t g_board_us, g_wait_us;   /* M3_PROFILE: the board's cost, and the guest's wait for it */

#ifdef _WIN32
static DWORD WINAPI board_thread(LPVOID arg)
#else
static void *board_thread(void *arg)
#endif
{
    (void)arg;
    for (;;) {
        LOCK();
        while (!g_busy) WAIT();
        UNLOCK();
        {
            uint64_t t = platform_ticks_us();
            board_field();
            g_board_us += platform_ticks_us() - t;
        }
        LOCK();
        g_busy = 0;
        WAKE();
        UNLOCK();
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* Wait for the board to finish the field it is making. */
static void board_sync(void)
{
    if (g_threaded <= 0) return;
    LOCK();
    while (g_busy) WAIT();
    UNLOCK();
}

void sound_run_field(void)
{
    uint64_t t0;
    if (!g_scsp1) return;
    if (g_threaded < 0) {
        const char *e = getenv("M3_SOUND_THREAD");
        g_threaded = !(e && *e == '0');
#ifdef M3_DREAMCAST
        g_threaded = 0;             /* one core: a thread only adds switching */
#endif
        if (g_threaded) {
#ifdef _WIN32
            HANDLE h = CreateThread(NULL, 0, board_thread, NULL, 0, NULL);
            if (h) CloseHandle(h); else g_threaded = 0;
#else
            pthread_t t;
            if (pthread_create(&t, NULL, board_thread, NULL)) g_threaded = 0;
            else pthread_detach(t);
#endif
        }
    }
    t0 = platform_ticks_us();
    board_sync();
    g_wait_us += platform_ticks_us() - t0;

    /* The field boundary: bytes the guest sent go down the line, bytes the
     * board sent back become readable. */
    while (g_p.tx_r != g_p.tx_w && ((g_s.tx_w + 1) % QSIZE) != g_s.tx_r) {
        g_s.tx[g_s.tx_w] = g_p.tx[g_p.tx_r];
        g_s.tx_w = (g_s.tx_w + 1) % QSIZE;
        g_p.tx_r = (g_p.tx_r + 1) % QSIZE;
    }
    if (g_decoupled) g_s.rx_r = g_s.rx_w;      /* nobody reads its replies */
    while (g_s.rx_r != g_s.rx_w && ((g_p.rx_w + 1) % QSIZE) != g_p.rx_r) {
        g_p.rx[g_p.rx_w] = g_s.rx[g_s.rx_r];
        g_p.rx_w = (g_p.rx_w + 1) % QSIZE;
        g_s.rx_r = (g_s.rx_r + 1) % QSIZE;
    }
    if (g_p.ppc_irq && !g_decoupled) irq_raise(M3_IRQ_SOUND);

    if (g_threaded) {
        LOCK();
        g_busy = 1;
        WAKE();
        UNLOCK();
    } else {
        board_field();
    }
}

void sound_state(m3_state_t *st)
{
    size_t n = scsp_state_size();
    uint8_t *tmp;
    if (!g_scsp1) return;
    board_sync();                   /* the board must be between fields */
    m3_state_io(st, g_ram1, g_ram_local);
    m3_state_io(st, g_ram2, g_ram_local);
    M3_STATE_VAR(st, g_s);
    M3_STATE_VAR(st, g_p);
    tmp = (uint8_t *)malloc(n);
    if (tmp) {
        scsp_state_save(g_scsp1, tmp); m3_state_io(st, tmp, n); scsp_state_load(g_scsp1, tmp);
        scsp_state_save(g_scsp2, tmp); m3_state_io(st, tmp, n); scsp_state_load(g_scsp2, tmp);
        free(tmp);
    }
    {
        unsigned cs = m68k_context_size();
        uint8_t *ctx = (uint8_t *)malloc(cs);
        if (ctx) {
            m68k_get_context(ctx);
            m3_state_io(st, ctx, cs);
            m68k_set_context(ctx);
            free(ctx);
        }
    }
}
