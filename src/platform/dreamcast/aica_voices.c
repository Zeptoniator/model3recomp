/* model3recomp -- the sound board's voices on the Dreamcast's AICA.
 *
 * The AICA is the SCSP's descendant: the same slot architecture, the same
 * envelope, pitch and LFO fields, mostly in the same bit positions, and 64
 * channels where the Model 3 has two chips of 32. So the board's 68000
 * driver runs on the SH-4 (sound.c, with chips that only keep time:
 * SCSP_NO_SYNTH), and every write it makes to a slot is re-encoded here
 * into the matching AICA channel: chip 1's slots are channels 0-31, chip
 * 2's 32-63. Each chip's 512 KB RAM is a window of the AICA's: what the
 * driver writes there (samples, mostly) goes straight over the G2 bus.
 *
 * Not carried over, yet: the DSP (reverb; voices' sends to it are left at
 * zero), FM (Scud Race keys no voice with it), 16-bit samples (the AICA
 * reads them little-endian; Scud plays only 8-bit ones, which are bytes
 * in the same order on both), and the SCSP's reverse and alternating loops
 * (the AICA loops forward only).
 *
 * KOS's spu_init() leaves the ARM7 in an idle loop at the bottom of sound
 * RAM, so the SH-4 has the channel registers to itself.
 */
#include "../../scsp.h"
#include "model3recomp/sound.h"
#include "aica_voices.h"

#include <dc/g2bus.h>
#include <dc/spu.h>
#include <stdio.h>
#include <stdint.h>

#define AICA_REG(o)      (0xA0700000u + (uint32_t)(o))
#define AICA_CH(ch, r)   AICA_REG((uint32_t)(ch) * 0x80u + (uint32_t)(r) * 4u)
#define AICA_RAM(a)      (SPU_RAM_UNCACHED_BASE + (uint32_t)(a))

/* Where each chip's RAM sits in the AICA's 2 MB: above KOS's idle loop. */
static const uint32_t k_base[2] = { 0x100000u, 0x180000u };

static int g_chip_id[2] = { 0, 1 };
static uint16_t g_slot[2][32][16];          /* the last value of each slot word */
static unsigned g_since_wait;
uint64_t g_aica_regs, g_aica_monitors, g_aica_ram_words, g_aica_ram_rmw;

/* The G2 bus takes eight writes before its FIFO must be let drain. */
static void g2w(uint32_t addr, uint32_t v)
{
    if (g_since_wait++ % 8u == 0) g2_fifo_wait();
    g2_write_32_raw(addr, v);
}

static uint32_t g2r(uint32_t addr)
{
    g2_fifo_wait();
    g_since_wait = 1;
    return g2_read_32_raw(addr);
}

static void ch_w(int ch, int r, uint32_t v) { g2w(AICA_CH(ch, r), v); g_aica_regs++; }

/* ---- sample RAM: bytes gathered into words ------------------------------
 * The driver copies samples a byte at a time, in order; the AICA's RAM is
 * written 32 bits at a time. A word is sent when its four bytes are in,
 * or merged with what is there when the driver moves on before that. */
static uint32_t g_pend_addr = ~0u, g_pend_val, g_pend_mask;

static void ram_flush(void)
{
    if (g_pend_addr == ~0u) return;
    if (g_pend_mask != 0xFu) {
        uint32_t keep = 0, old = g2r(AICA_RAM(g_pend_addr)), k;
        for (k = 0; k < 4; k++) if (!(g_pend_mask & (1u << k))) keep |= 0xFFu << (k * 8);
        g_pend_val = (g_pend_val & ~keep) | (old & keep);
        g_aica_ram_rmw++;
    }
    g2w(AICA_RAM(g_pend_addr), g_pend_val);
    g_aica_ram_words++;
    g_pend_addr = ~0u;
}

static void ram_w8(int chip, uint32_t a, uint8_t v)
{
    uint32_t at = k_base[chip & 1] + (a & 0x7FFFFu), w = at & ~3u, sh = (at & 3u) * 8u;
    if (w != g_pend_addr) {
        ram_flush();
        g_pend_addr = w; g_pend_val = 0; g_pend_mask = 0;
    }
    g_pend_val = (g_pend_val & ~(0xFFu << sh)) | ((uint32_t)v << sh);
    g_pend_mask |= 1u << (at & 3u);
    if (g_pend_mask == 0xFu) ram_flush();
}

static uint8_t ram_r8(int chip, uint32_t a)
{
    uint32_t at = k_base[chip & 1] + (a & 0x7FFFFu);
    ram_flush();
    return (uint8_t)(g2r(AICA_RAM(at & ~3u)) >> ((at & 3u) * 8u));
}

/* ---- slots -------------------------------------------------------------
 * SCSP slot words (scsp.c's macros) against AICA channel registers:
 *   0  KEYONEX 12 KEYONB 11 SBCTL 10:9 SSCTL 8:7 LPCTL 6:5 PCM8B 4 SA 3:0
 *      -> KEYONEX 15 KEYONB 14 SSCTL 10 LPCTL 9 PCMS 8:7 SA 6:0
 *   1  SA 15:0 -> 1      2  LSA -> 2      3  LEA -> 3
 *   4  D2R D1R EGHOLD AR -> 4 (no EGHOLD)
 *   5  LPSLNK KRS DL RR -> 5, same places
 *   6  STWINH SDIR TL 7:0 -> 10: TL 15:8, the filter off (LPOFF, bit 5)
 *   8  OCT 14:11 FNS 9:0 -> 6, same places
 *   9  LFO -> 7, same places
 *  11  DISDL 15:13 (6 dB steps) DIPAN 12:8 -> 9: DISDL 11:8 (3 dB) DIPAN 4:0
 */
static uint32_t monitor_raw(int ch, uint32_t *ca);

/* A one-shot that has played to its end. The SCSP clears its KEYONB then,
 * so the chip's next KEYONEX leaves it alone; the AICA keeps the bit, and
 * every KEYONEX after -- which acts on all 64 channels -- starts it again.
 * So before one is sent, any one-shot still keyed that the AICA has put in
 * release (SGC 3) is unkeyed, without KEYONEX. */
static uint64_t g_reaped;
static uint8_t g_started[2][32];            /* keyed by a KEYONEX since KEYONB rose */
static void reap_ended(void)
{
    int chip, s;
    for (chip = 0; chip < 2; chip++)
        for (s = 0; s < 32; s++) {
            uint16_t *w = g_slot[chip][s];
            if ((w[0] & 0x0800u) && !(w[0] & 0x0060u) && g_started[chip][s]) {
                uint32_t ca, eg = monitor_raw(chip * 32 + s, &ca);
                if (((eg >> 13) & 3u) == 3u) {
                    uint32_t sa = k_base[chip] + ((((uint32_t)w[0] & 0xFu) << 16 | w[1]) & 0x7FFFFu);
                    w[0] &= ~0x0800u;
                    g_started[chip][s] = 0;
                    ch_w(chip * 32 + s, 0, (w[0] & 0x0010u ? 0x0080u : 0u) | ((sa >> 16) & 0x7Fu));
                    g_reaped++;
                }
            }
        }
}

static void slot_word0(int chip, int s)
{
    const uint16_t *w = g_slot[chip][s];
    uint32_t sa = k_base[chip] + ((((uint32_t)w[0] & 0xFu) << 16 | w[1]) & 0x7FFFFu);
    uint32_t v = ((uint32_t)(w[0] & 0x1000u) << 3)          /* KEYONEX */
               | ((uint32_t)(w[0] & 0x0800u) << 3)          /* KEYONB */
               | ((w[0] & 0x0180u) ? 0x0400u : 0u)          /* SSCTL: noise */
               | ((w[0] & 0x0060u) ? 0x0200u : 0u)          /* LPCTL: loop */
               | ((w[0] & 0x0010u) ? 0x0080u : 0u)          /* PCM8B: PCMS 1 */
               | ((sa >> 16) & 0x7Fu);
    int ch = chip * 32 + s;
    if (w[0] & 0x1000u) reap_ended();
    ch_w(ch, 1, sa & 0xFFFFu);
    ch_w(ch, 0, v);
    g_slot[chip][s][0] &= ~0x1000u;          /* KEYONEX acts once */
    if (v & 0x8000u) {                      /* every keyed channel starts now */
        int c2, s2;
        for (c2 = 0; c2 < 2; c2++)
            for (s2 = 0; s2 < 32; s2++)
                if (g_slot[c2][s2][0] & 0x0800u) g_started[c2][s2] = 1;
    }
}

static void slot_write(void *ctx, int s, int word, uint16_t v)
{
    int chip = *(const int *)ctx, ch = chip * 32 + (s & 31);
    s &= 31;
    if ((word & 15) == 0 && ((g_slot[chip][s][0] ^ v) & 0x0800u))
        g_started[chip][s] = 0;             /* KEYONB rose or fell */
    g_slot[chip][s][word & 15] = v;
    switch (word & 15) {
    case 0: case 1: slot_word0(chip, s); break;
    case 2: ch_w(ch, 2, v); break;
    case 3: ch_w(ch, 3, v); break;
    case 4: ch_w(ch, 4, v & ~0x20u); break;
    case 5: ch_w(ch, 5, v & 0x7FFFu); break;
    case 6: ch_w(ch, 10, ((uint32_t)(v & 0xFFu) << 8) | 0x20u); break;
    case 8: ch_w(ch, 6, v & 0x7BFFu); break;
    case 9: ch_w(ch, 7, v); break;
    case 11: {
        uint32_t d = (v >> 13) & 7u;
        ch_w(ch, 9, ((d ? 2u * d + 1u : 0u) << 8) | ((v >> 8) & 0x1Fu));
        break;
    }
    default: break;                         /* FM, DSP sends, effect returns */
    }
}

static void common_write(void *ctx, int reg, uint16_t v)
{
    /* One master volume for both chips: the first chip's. */
    if (*(const int *)ctx == 0 && reg == 0)
        g2w(AICA_REG(0x2800), v & 0xFu);
}

/* The slot monitor: the AICA's (0x280C selects, 0x2810 is LP SGC EG,
 * 0x2814 the play position) in the SCSP's word, CA << 7 | SGC << 5 | EG. */
static uint32_t monitor_raw(int ch, uint32_t *ca)
{
    uint32_t eg;
    g2w(AICA_REG(0x280C), (uint32_t)ch << 8);
    eg = g2r(AICA_REG(0x2810));
    *ca = g2r(AICA_REG(0x2814));
    g_aica_monitors++;
    return eg;
}

static uint16_t monitor(void *ctx, int s)
{
    int ch = *(const int *)ctx * 32 + (s & 31);
    uint32_t ca, eg = monitor_raw(ch, &ca);
    return (uint16_t)((((ca >> 12) & 0xFu) << 7) | (((eg >> 13) & 3u) << 5) | ((eg >> 8) & 0x1Fu));
}

static void dma_write(void *ctx, uint32_t a, uint16_t v)
{
    int chip = *(const int *)ctx;
    ram_w8(chip, a, (uint8_t)(v >> 8));
    ram_w8(chip, a + 1, (uint8_t)v);
}

static void field(void) { ram_flush(); }

static const scsp_voices_t k_chip[2] = {
    { &g_chip_id[0], slot_write, common_write, NULL, dma_write, monitor },
    { &g_chip_id[1], slot_write, common_write, NULL, dma_write, monitor },
};
static const sound_voices_t k_voices = { { &k_chip[0], &k_chip[1] }, ram_w8, ram_r8, field };

const sound_voices_t *aica_voices(void)
{
    int ch;
    /* Every channel off and silent, the filter open, full master volume.
     * Sound RAM was cleared by spu_init(). */
    for (ch = 0; ch < 64; ch++) {
        ch_w(ch, 0, 0x8000u);               /* KEYONEX with KEYONB clear: off */
        ch_w(ch, 5, 0x1Fu);                 /* fastest release */
        ch_w(ch, 8, 0);                     /* nothing to the DSP */
        ch_w(ch, 9, 0);
        ch_w(ch, 10, 0x20u);
    }
    g2w(AICA_REG(0x2800), 0xFu);
    g2_fifo_wait();
    return &k_voices;
}

void aica_voices_report(void)
{
    printf("[aica] %llu register writes, %llu monitor reads, %llu RAM words (%llu merged),"
           " %llu ended one-shots unkeyed\n",
           (unsigned long long)g_aica_regs, (unsigned long long)g_aica_monitors,
           (unsigned long long)g_aica_ram_words, (unsigned long long)g_aica_ram_rmw,
           (unsigned long long)g_reaped);
}
