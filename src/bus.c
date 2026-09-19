/* model3recomp -- Model 3 memory bus.
 *
 * Every guest load and store lands here. Two routings in this file are
 * load-bearing enough to call out, because getting either wrong hangs the
 * game rather than glitching it:
 *
 *   read  0xF0100018  ->  irq_status_read()   the frame boundary
 *   write 0xF1180010  ->  irq_ack()           the acknowledge the guest spins on
 *
 * See docs/technical/execution-model.md.
 */
#include "model3recomp/bus.h"
#define M3_NL "\n"
#include "model3recomp/irq.h"
#include "model3recomp/scsi.h"
#include "model3recomp/model3recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t  *g_ram;
uint8_t *m3_ram_base;           /* mirrors g_ram, for lift.h's fast path */
static m3_roms_t g_roms;
static uint8_t  *g_backup;          /* 128 KB battery-backed SRAM */
static uint32_t  g_crom_bank;       /* byte offset of the window into the image */
static uint32_t  g_crom_bank_reg;   /* what was written, which reads back verbatim */

/* I/O board control register at 0xF0040000. It is a latch: the boot strobes a
 * bit, reads the register back, and waits for the bit to mirror -- first for
 * it to set, then for it to clear:
 *
 *     stw r10, 0(r11)   ; r11 = 0xF0040000, r10 = 0x01000000
 *     lwz r0,  0(r11)
 *     andis. r9, r0, 0x100
 *     beq  -0xC                    ; spin until it SETS
 *     ...
 *     stw r10, 0(r11)   ; r10 = 0
 *     lwz r0,  0(r11)
 *     andis. r9, r0, 0x100
 *     bne  -0xC                    ; spin until it CLEARS
 *
 * Return a constant here -- 0 or 0xFFFFFFFF -- and one of those two loops
 * never exits. */
static uint32_t  g_io_ctrl;
static uint32_t  g_io_ready;        /* see the serial-handshake note below */
static uint32_t  g_pci_addr;        /* PCI CONFIG_ADDRESS latch            */
static uint32_t  g_r3d_ping;        /* Real3D status, bit 1 is the ping    */

/* Tilegen VRAM is 0xF1000000..0xF111FFFF: 1 MB of tile/name data followed by
 * 128 KB of palette. One allocation, since the guest addresses it as one. */
#define TILEGEN_VRAM_SIZE 0x120000u
static uint8_t *g_vram;

/* The tilegen's own register file, 0xF1180000..0xF11800FF. Only the ack at
 * 0x10 has a side effect; the rest is state the renderer reads back -- layer
 * enables and the per-layer scroll. */
static uint32_t g_tilegen_regs[64];

const uint32_t *bus_tilegen_regs(void) { return g_tilegen_regs; }

/* The Real3D's upload FIFO at 0x94000000.
 *
 * The guest streams a thousand bytes a frame into this one address, and until
 * it was named the DMA guard threw every transfer away as out of range -- 464
 * of them in a two-thousand-field run, which is most of what the guest sends
 * the graphics processor. Kept in arrival order, because a FIFO has no
 * addresses. */
#define TEXFIFO_CAP 0x400000u
static uint8_t *g_texfifo;
static size_t   g_texfifo_used;

/* Real3D culling and polygon RAM. Not yet consumed by a renderer, but the
 * guest writes them constantly and reads back what it wrote. */
#define CULL_LO_SIZE 0x400000u
#define CULL_HI_SIZE 0x100000u
#define POLY_SIZE    0x400000u
static uint8_t *g_cull_lo, *g_cull_hi, *g_poly;

void bus_init(const m3_roms_t *roms)
{
    g_roms = *roms;
    g_ram     = calloc(1, M3_RAM_SIZE);
    g_backup  = calloc(1, 0x20000);
    /* Battery-backed settings. A machine that has been switched on before has
     * these; a freshly allocated block of zeros is a machine that never has,
     * and a game that checks them for its own signature behaves accordingly.
     * M3_BACKUP loads a saved image, which is what the battery is for. */
    if (g_backup) {
        const char *bk = getenv("M3_BACKUP");
        if (bk) {
            FILE *bf = fopen(bk, "rb");
            if (bf) {
                size_t got = fread(g_backup, 1, 0x20000, bf);
                fclose(bf);
                fprintf(stderr, "[model3recomp] backup RAM: %u bytes from %s\n",
                        (unsigned)got, bk);
            }
        }
    }
    g_vram    = calloc(1, TILEGEN_VRAM_SIZE);
    g_texfifo = calloc(1, TEXFIFO_CAP);
    g_cull_lo = calloc(1, CULL_LO_SIZE);
    g_cull_hi = calloc(1, CULL_HI_SIZE);
    g_poly    = calloc(1, POLY_SIZE);
    if (!g_ram || !g_backup || !g_vram || !g_cull_lo || !g_cull_hi || !g_poly) {
        fprintf(stderr, "[model3recomp] out of memory allocating the board\n");
        abort();
    }
    m3_ram_base = g_ram;
    g_crom_bank = 0;
    g_crom_bank_reg = 0;
    g_io_ctrl = 0;
    g_io_ready = 0;
    g_pci_addr = 0;
    g_r3d_ping = 0;
    scsi_init();
}

void bus_shutdown(void)
{
    free(g_ram); free(g_backup); free(g_vram); free(g_texfifo);
    free(g_cull_lo); free(g_cull_hi); free(g_poly);
    g_ram = g_backup = g_vram = g_cull_lo = g_cull_hi = g_poly = NULL;
    g_texfifo = NULL; g_texfifo_used = 0;
    m3_ram_base = NULL;
}

uint8_t *bus_ram(void) { return g_ram; }

uint8_t *bus_texfifo(size_t *used, size_t *capacity)
{
    if (used) *used = g_texfifo_used;
    if (capacity) *capacity = TEXFIFO_CAP;
    return g_texfifo;
}

void bus_texfifo_reset(void) { g_texfifo_used = 0; }

uint8_t *bus_vram(size_t *size)
{
    if (size) *size = TILEGEN_VRAM_SIZE;
    return g_vram;
}

uint8_t *bus_cull_lo(size_t *size)
{
    if (size) *size = CULL_LO_SIZE;
    return g_cull_lo;
}

uint8_t *bus_cull_hi(size_t *size)
{
    if (size) *size = CULL_HI_SIZE;
    return g_cull_hi;
}

uint8_t *bus_poly(size_t *size)
{
    if (size) *size = POLY_SIZE;
    return g_poly;
}

const uint8_t *bus_vrom(size_t *size)
{
    if (size) *size = g_roms.vrom_size;
    return g_roms.vrom;
}

/* ---- big-endian accessors over a host buffer --------------------------- */
static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static inline uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint32_t)p[0] << 8) | p[1]);
}
static inline void wbe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline void wbe16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

/* ---- region resolution --------------------------------------------------
 * Returns a host pointer for the plain-memory regions, or NULL when the
 * address belongs to a device and has to go through the switch below.
 */
static uint8_t *direct(uint32_t a, uint32_t size, int write)
{
    if (a < M3_RAM_SIZE)
        return g_ram + a;

    if (a >= M3_R3D_CULL_LO && a < M3_R3D_CULL_LO + CULL_LO_SIZE)
        return g_cull_lo + (a - M3_R3D_CULL_LO);
    if (a >= M3_R3D_CULL_HI && a < M3_R3D_CULL_HI + CULL_HI_SIZE)
        return g_cull_hi + (a - M3_R3D_CULL_HI);
    if (a >= M3_R3D_POLY && a < M3_R3D_POLY + POLY_SIZE)
        return g_poly + (a - M3_R3D_POLY);

    if (a >= M3_TILEGEN_VRAM && a < M3_TILEGEN_VRAM + TILEGEN_VRAM_SIZE)
        return g_vram + (a - M3_TILEGEN_VRAM);

    if (a >= M3_BACKUP_BASE && a < M3_BACKUP_BASE + 0x20000)
        return g_backup + (a - M3_BACKUP_BASE);

    if (!write) {
        /* Fixed program CROM occupies the top of the map. A ROM smaller than
         * the 8 MB window sits at the END of it, which is why the reset
         * vector at 0xFFF00100 lands where it does. */
        if (a >= M3_CROM_FIXED && g_roms.crom) {
            uint32_t top = 0xFFFFFFFFu - (uint32_t)g_roms.crom_size + 1u;
            if (a >= top)
                return (uint8_t *)g_roms.crom + (a - top);
            /* Below the program and above 0xFF800000 is the data ROM, and it
             * is not banked: the whole span is the front of the image, which
             * is where a game's tables and models actually live.
             *
             * This used to read as nothing at all, and a guest asking for its
             * own data got zeros -- The Lost World reads 0xFFA18DEC, where
             * the image has an "M3"-tagged record, and found none. Checked
             * against MAME: 0xFF800000 is offset 0 of this image, 0xFFC00000
             * is 0x400000 and so on, one for one to 0xFFDFFFFF, and driving
             * the bank register through all 256 values moves none of it. */
            if (g_roms.crom_bank) {
                uint32_t off = a - M3_CROM_FIXED;
                if (off < g_roms.crom_bank_size)
                    return (uint8_t *)g_roms.crom_bank + off;
            }
            return NULL;
        }
        /* Banked CROM window. */
        if (a >= M3_CROM_BANKED && a < M3_CROM_FIXED && g_roms.crom_bank) {
#ifdef M3_LOOP_GUARD
            /* Which window offsets the guest reads, under which bank value.
             * Pinning the bank mapping down needs the guest's own access
             * pattern, not a guess at what the register bits mean. */
            {
                static int probed, on;
                static uint32_t seen[64];
                static unsigned nseen;
                if (!probed) { probed = 1; on = getenv("M3_TRACE_BANK") != NULL; }
                if (on) {
                    uint32_t key = (g_crom_bank_reg << 24) |
                                   ((a - M3_CROM_BANKED) >> 16);
                    unsigned i;
                    for (i = 0; i < nseen; i++) if (seen[i] == key) break;
                    if (i == nseen && nseen < 64) {
                        seen[nseen++] = key;
                        fprintf(stderr, "[bank] reg=%02X window+0x%06X\n",
                                g_crom_bank_reg, (a - M3_CROM_BANKED) & ~0xFFFFu);
                    }
                }
            }
#endif
            uint32_t off = (a - M3_CROM_BANKED) + g_crom_bank;
            if (off < g_roms.crom_bank_size)
                return (uint8_t *)g_roms.crom_bank + off;
            return NULL;
        }
    }
    return NULL;
}

/* ---- spin detection -----------------------------------------------------
 * A recompiled game that stops making progress is almost always polling one
 * hardware register forever. There is no PC to inspect -- lifted code is
 * native code -- so the bus counts what it is asked for and names the culprit
 * itself once the traffic is obviously pathological.
 */
/* M3_TRACE_IO=<path> records every device access in order. The interpreter in
 * tools/ppc_interp.py writes the same format, so the two can be diffed: the
 * first line that differs is where the lifted code and the interpreter stop
 * agreeing. That is the whole point of keeping an interpreter around. */
static FILE *g_io_log;
static long  g_io_budget;

static void io_log_init(void)
{
    static int done;
    const char *path, *lim;
    if (done) return;
    done = 1;
    path = getenv("M3_TRACE_IO");
    if (!path) return;
    g_io_log = fopen(path, "w");
    lim = getenv("M3_TRACE_IO_MAX");
    g_io_budget = lim ? strtol(lim, NULL, 0) : 20000;
}

static void io_log(char rw, uint32_t a, unsigned size, uint32_t v)
{
    io_log_init();
    if (!g_io_log || g_io_budget <= 0) return;
    g_io_budget--;
    fprintf(g_io_log, "%c %08X %u %08X\n", rw, a, size, v);
    if (g_io_budget == 0) { fclose(g_io_log); g_io_log = NULL; }
}

enum { SPIN_SLOTS = 24, SPIN_REPORT = 20000000u };
static struct { uint32_t addr, n; } g_spin[SPIN_SLOTS];
static uint32_t g_dev_accesses;

static void spin_note(uint32_t a, int write)
{
    unsigned i, weakest = 0;
    m3_work++;                     /* device traffic counts as progress too */
    uint32_t key = (a & ~3u) | (write ? 1u : 0u);
    for (i = 0; i < SPIN_SLOTS; i++) {
        if (g_spin[i].addr == key && g_spin[i].n) {
            g_spin[i].n++;
            goto counted;
        }
        if (g_spin[i].n < g_spin[weakest].n)
            weakest = i;
    }
    /* Table full: evict the least-used slot rather than keeping whichever
     * addresses happened to come first. A boot writes dozens of registers
     * once each, and those would otherwise crowd out the one address the
     * guest is spinning on -- which is the only one worth seeing. */
    g_spin[weakest].addr = key;
    g_spin[weakest].n = 1;
counted:
    if (++g_dev_accesses < SPIN_REPORT)
        return;
    g_dev_accesses = 0;
    fprintf(stderr, "[model3recomp] %u device accesses with no field "
                    "advance -- the guest is probably spinning:\n", SPIN_REPORT);
    for (i = 0; i < SPIN_SLOTS && g_spin[i].n; i++)
        fprintf(stderr, "    %c %08X x%u\n",
                (g_spin[i].addr & 1) ? 'W' : 'R',
                g_spin[i].addr & ~3u, g_spin[i].n);
    for (i = 0; i < SPIN_SLOTS; i++) { g_spin[i].addr = 0; g_spin[i].n = 0; }
}

/* ---- devices ------------------------------------------------------------
 * Registers are defined as 32-bit words, but the guest reaches several of
 * them with lbz/stb -- the CROM bank register at 0xF0100008 is a byte
 * register the boot code writes and then reads back to confirm. So every
 * device access is funnelled through an aligned word accessor and the byte
 * lane is extracted here, once, rather than each device guessing.
 *
 * Getting this wrong does not glitch anything: the boot writes a bank, reads
 * back a value that never matches, and spins on 0xF0100008 forever.
 */
/* ---- PCI configuration space -------------------------------------------
 *
 * The game will not finish booting until it finds two devices by ID. It walks
 * them through the MPC105's CONFIG_ADDRESS/CONFIG_DATA port pair:
 *
 *     addr = 0x80000000 | (device << 11) | offset
 *     stwbrx addr -> 0xF0800CF8 ; lwbrx  data <- 0xF0C00CFC
 *
 * and compares dword 0 against a literal. Device 13 must answer as the Real3D
 * and device 14 as the 53C810; miss either and the guest spins on a readiness
 * flag that the matching arm is what sets. Both already live at fixed
 * addresses in this board's memory map, so the BAR writes that follow are
 * dropped -- there is nothing here for the guest to relocate.
 */
#define PCI_ID_REAL3D  0x16C311DBu   /* vendor 0x11DB Sega, device 0x16C3 */
#define PCI_ID_53C810  0x00011000u   /* vendor 0x1000 LSI,  device 0x0001 */

static uint32_t bswap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00u) |
           ((v << 8) & 0xFF0000u) | (v << 24);
}

static uint32_t pci_config(uint32_t addr)
{
    uint32_t dev;

    if (!(addr & 0x80000000u))
        return 0;
    if ((addr & 0xFCu) != 0)
        return 0;                    /* every other register reads zero */

    dev = (addr >> 11) & 0x1Fu;
    if (dev == 13) return PCI_ID_REAL3D;
    if (dev == 14) return PCI_ID_53C810;
    return 0;                        /* absent */
}

static uint32_t dev_read_word(uint32_t w, int side_effects);

static uint32_t dev_read_word(uint32_t w, int side_effects)
{
    /* System controller / interrupt controller. */
    if ((w & 0xFFFFFFC0u) == M3_SYSCTL_BASE) {
        switch (w & 0x3C) {
        case 0x14: return irq_enable_read();
        /* THE frame boundary. Answering this read is what makes a field
         * elapse; see docs/technical/execution-model.md. Skipped when this
         * is the read half of a read-modify-write on a byte store. */
        case 0x18: return side_effects ? irq_status_read() : irq_pending();
        /* Byte register in the most-significant lane of its word. */
        /* Reads back exactly what was written. The boot writes a bank and
         * spins here until it matches, so this cannot be derived from the
         * offset below -- the moment the mapping is anything but a shift,
         * the two stop agreeing and the guest never leaves the loop. */
        case 0x08: return g_crom_bank_reg << 24;
        default:   return 0;
        }
    }

    /* Real3D status block. The boot copies nine dwords of this into RAM with
     * stwbrx -- the chip is on the PCI side, so the cached copy is
     * byte-reversed -- and then waits for bit 0x02000000 of that copy to
     * flip. Byte-reversed, that is bit 1 of the register here.
     *
     * ponytail: the ping is toggled per read rather than driven by the
     * renderer, which is all the wait needs. Upgrade path: flip it when the
     * Real3D actually retires a frame, once there is a renderer to retire
     * one. */
    if ((w & 0xFF000000u) == M3_R3D_STATUS) {
        if (w != M3_R3D_STATUS)
            return 0;
        /* One flip per field, because a guest measures time with this.
         *
         * Toggling it per read satisfies a guest that only waits for the bit
         * to change, and that is what this did. But The Lost World times the
         * interval between two flips against the time base and divides to get
         * its decrementer period: flipping on demand makes that interval
         * nothing, and it computed a period of -26 where the hardware gives
         * 26,926. Its decrementer then never fired, its main loop waited
         * forever on a counter only that interrupt writes, and it never ran a
         * single one of its own state handlers.
         *
         * So the ping is what it is on hardware -- a frame flag -- and the
         * wait for it ends when a field does. */
        return (model3recomp_frame_count() & 1u) ? 0x00000002u : 0u;
    }

    /* PCI CONFIG_DATA. The guest reads it with lwbrx, so hand it back
     * byte-reversed. */
    if (w == M3_PCI_CONFIG_DATA)
        return bswap32(pci_config(g_pci_addr));

    /* Sound board handshake. The guest reads this with lwbrx and tests two
     * bits of the byte-reversed word: bit 0 is "ready for a command" and bit
     * 1 is "a reply byte is waiting". Byte-reversed those are 0x01000000 and
     * 0x02000000 here.
     *
     * The command loop at 0x00118854 spins until ready, and it runs inside
     * the VBlank handler -- so a board that is never ready does not merely
     * lose sound, it wedges the machine, because irq_tick() will not re-enter
     * while a dispatch is in progress and no further field ever falls due.
     *
     * ponytail: with no 68000 to be busy, always ready and never answering is
     * the honest model -- the guest's commands go nowhere and it never waits
     * on a reply. Upgrade path: a real 68000 and SCSPs, at which point both
     * bits come from the board instead. */
    if ((w & 0xFFFFFFFCu) == (M3_SOUND_BASE + 4u))
        return 0x01000000u;

    if ((w & 0xFFFFFFC0u) == M3_INPUTS_BASE) {
        if ((w & 0x3Cu) == 0x00u)
            return g_io_ctrl;        /* the strobe latch reads back */
        if ((w & 0x3Cu) == 0x04u) {
            /* ponytail: the I/O board's serial ready line is toggled rather
             * than driven by a real protocol. The boot bit-bangs a byte to
             * 0xF0040000 and then waits on bit 0x20000000 here -- first for it
             * to clear, then for it to set:
             *
             *     li r4, 0x51 ; bl <bit-bang>
             *     lwz r0, 0(r30)        ; r30 = 0xF0040004
             *     andis. r9, r0, 0x2000
             *     bne -0x14             ; wait for CLEAR
             *     ... then the same loop waiting for SET
             *
             * A constant hangs one loop or the other. Toggling satisfies both
             * and gets the boot past I/O init; the data the game reads back is
             * not meaningful. Upgrade path: model the 315-5649 I/O board's
             * serial protocol properly, which is also what buttons and coins
             * need. */
            g_io_ready ^= 0x20000000u;
            return 0xFFFFFFFFu ^ g_io_ready;
        }
        /* Inputs are active low, so all ones is "nothing pressed". A game
         * also reads cabinet straps and DIP switches through here, and for
         * those all ones is a value like any other -- M3_INPUTS overrides it
         * so what the guest does with a different one can be asked. */
        {
            static int probed;
            static uint32_t forced;
            static int have;
            if (!probed) {
                const char *e = getenv("M3_INPUTS");
                probed = 1;
                if (e) { forced = (uint32_t)strtoul(e, NULL, 0); have = 1; }
            }
            if (have)
                return forced;
        }
        return 0xFFFFFFFFu;          /* inputs are active low: nothing pressed */
    }

    return 0;
}

/* The Real3D "draw this" trigger.
 *
 * The scene in culling and polygon RAM is written by the guest over many
 * thousands of instructions, and the runtime's field boundary is synthetic
 * -- it comes off m3_work, not off a video clock -- so rendering on the
 * field boundary means rendering whatever happens to be in the buffers at
 * that moment, which can be half of the frame the guest is building and half
 * of the one before it. Counting the trigger lets the renderer tell a
 * finished scene from a torn one.
 */
static uint64_t g_r3d_triggers;
static uint64_t g_dropped_writes[256];

uint64_t bus_r3d_trigger_count(void) { return g_r3d_triggers; }

static void dev_write_word(uint32_t w, uint32_t v)
{
    if ((w & 0xFFFFFFC0u) == M3_SYSCTL_BASE) {
        switch (w & 0x3C) {
        case 0x14: irq_enable_write(v); return;
        case 0x08: {
            /* CROM bank select. The banked window is 8 MB of a 32 MB image,
             * so only a couple of bits of this can be the bank; the rest is
             * something else. M3_CROM_BANK picks the reading while it is
             * being pinned down against what the guest then finds there. */
            static int probed;
            static unsigned mode;
            uint32_t b = (v >> 24) & 0xFFu;
            g_crom_bank_reg = b;
            if (!probed) {
                const char *e = getenv("M3_CROM_BANK");
                probed = 1;
                mode = e ? (unsigned)strtoul(e, NULL, 0) : 0u;
            }
            switch (mode) {
            case 1:  g_crom_bank = (b & 3u) * 0x800000u; break;
            case 2:  g_crom_bank = ((~b) & 3u) * 0x800000u; break;
            case 3:  g_crom_bank = (b & 7u) * 0x400000u; break;
            case 4:  g_crom_bank = ((~b) & 7u) * 0x400000u; break;
            case 5:  g_crom_bank = ((b >> 4) & 3u) * 0x800000u; break;
            case 6:  g_crom_bank = (b & 7u) * 0x800000u; break;
            case 7:  g_crom_bank = ((~b) & 7u) * 0x800000u; break;
            case 8:  g_crom_bank = ((b >> 1) & 7u) * 0x800000u; break;
            case 9:  g_crom_bank = ((b >> 1) & 3u) * 0x800000u; break;
            case 10: g_crom_bank = (((~b) >> 1) & 7u) * 0x800000u; break;
            case 11: {   /* sweep: bank = ((b >> S) ^ X) & 7 */
                const char *es = getenv("M3_BANK_S"), *ex = getenv("M3_BANK_X");
                unsigned sh = es ? (unsigned)strtoul(es, NULL, 0) : 0u;
                unsigned xr = ex ? (unsigned)strtoul(ex, NULL, 0) : 0u;
                g_crom_bank = (((b >> sh) ^ xr) & 7u) * 0x800000u;
                break;
            }
            /* What this has always done, kept as the default until the
             * right reading is known: the value shifted into a byte offset.
             * For most of the values The Lost World writes -- 0xF7 above all
             * -- that lands far outside the image, every read through the
             * window returns zero, and the guest quietly gets nothing. */
            default: g_crom_bank = b << 20; break;
            }
            return;
        }
        default: return;
        }
    }

    if ((w & 0xFFFFFFC0u) == M3_INPUTS_BASE) {
        if ((w & 0x3Cu) == 0x00u) g_io_ctrl = v;
        return;
    }

    if ((w & 0xFF000000u) == M3_R3D_TRIGGER) {
        g_r3d_triggers++;
        if (getenv("M3_R3D_TRACE")) {
            static unsigned n;
            if (n++ < 400)
                fprintf(stderr, "[r3d] trigger %08X = %08X\n", w, v);
        }
        return;
    }

    /* PCI CONFIG_ADDRESS. Written with stwbrx, so un-reverse it. */
    if (w == M3_PCI_CONFIG_ADDR) {
        g_pci_addr = bswap32(v);
        return;
    }

    /* Interrupt acknowledge. The guest writes the bit here and then spins on
     * 0xF0100018 until it clears -- miss this and the game never leaves its
     * interrupt handler. */
    if ((w & 0xFFFF0000u) == (M3_TILEGEN_REGS & 0xFFFF0000u)) {
        g_tilegen_regs[(w >> 2) & 0x3Fu] = v;
        if ((w & 0xFC) == 0x10) irq_ack(v);
        return;
    }

    /* Anything that reaches here is a write the board does not model, and
     * it is thrown away. Count it by page, because a region the guest
     * writes megabytes to and we ignore looks exactly like a region the
     * guest never writes. */
    g_dropped_writes[(w >> 24) & 0xFFu]++;
    if (getenv("M3_DROP_TRACE")) {
        static unsigned n;
        if (n++ < 30)
            fprintf(stderr, "[drop] %08X = %08X" M3_NL, w, v);
    }
}

void bus_report_dropped_writes(void)
{
    unsigned i, any = 0;
    for (i = 0; i < 256; i++) {
        if (!g_dropped_writes[i]) continue;
        if (!any++) fprintf(stderr, "[model3recomp] writes the board "
                                    "discarded, by page:\n");
        fprintf(stderr, "    %02X000000: %llu\n", i,
                (unsigned long long)g_dropped_writes[i]);
    }
    if (!any)
        fprintf(stderr, "[model3recomp] no writes were discarded\n");
    fflush(stderr);
}

/* Byte-lane extraction, big-endian: the byte at address a sits in lane
 * (a & 3) counted from the most-significant end of its word. */
static uint32_t dev_read(uint32_t a, unsigned size)
{
    uint32_t word, mask;
    unsigned sh;
    spin_note(a, 0);

    /* The other place the runtime gets control. func_table_call() alone is
     * not enough: the guest spends long stretches polling a device without
     * dispatching through a pointer, and during those the field never
     * advances and no interrupt is ever delivered -- so it polls forever.
     *
     * Delivering an interrupt from inside a load is what the hardware does
     * anyway; the guest's handler saves and restores every register, and our
     * lifted code keeps its temporaries in C locals the guest cannot reach.
     * irq_tick() is re-entrancy guarded. */
    irq_tick();
    if ((a & 0xFE000000u) == M3_SCSI_BASE) {
        uint32_t sv = scsi_read(a, size);
        io_log('R', a, size, sv);
        return sv;
    }
    if (size == 4) {
        word = dev_read_word(a & ~3u, 1);
        io_log('R', a, size, word);
        return word;
    }
    word = dev_read_word(a & ~3u, 1);
    sh   = 8u * (4u - size - (a & 3u));
    mask = (1u << (8u * size)) - 1u;
    word = (word >> sh) & mask;
    io_log('R', a, size, word);
    return word;
}

#ifdef M3_LOOP_GUARD
/* Which guest function wrote a device register. The RAM watch in lift.h
 * cannot see these: a device write leaves the inline path and comes here. */
extern uint32_t m3_fn_now;
extern uint32_t m3_fn_prev;
static void dev_watch(uint32_t a, uint32_t v, unsigned size)
{
    static int probed;
    static uint32_t want;
    if (!probed) {
        const char *e = getenv("M3_WATCH_DEV");
        probed = 1;
        want = e ? (uint32_t)strtoul(e, NULL, 0) : 0u;
    }
    if (want && (a & 0xFFFFF000u) == (want & 0xFFFFF000u))
        fprintf(stderr, "[model3recomp] dev W %08X = %08X (%u) from guest "
                        "function %08X (called from %08X)\n", a, v, size, m3_fn_now, m3_fn_prev);
}
#define DEV_WATCH(a, v, n) dev_watch((a), (v), (n))
#else
#define DEV_WATCH(a, v, n) ((void)0)
#endif

static void dev_write(uint32_t a, uint32_t v, unsigned size)
{
    DEV_WATCH(a, v, size);

    /* The upload FIFO. One port, so the address does not advance and what
     * matters is the order the bytes arrived in. */
    if ((a & 0xFF000000u) == M3_R3D_TEXFIFO) {
        unsigned i;
        for (i = 0; i < size; i++) {
            if (g_texfifo_used >= TEXFIFO_CAP)
                break;
            g_texfifo[g_texfifo_used++] =
                (uint8_t)(v >> (8u * (size - 1u - i)));
        }
        return;
    }
    uint32_t w = a & ~3u, cur, mask;
    unsigned sh;
    spin_note(a, 1);
    io_log('W', a, size, v);
    if ((a & 0xFE000000u) == M3_SCSI_BASE) { scsi_write(a, v, size); return; }
    if (size == 4) { dev_write_word(w, v); return; }
    /* Read-modify-write without side effects: a byte store must not be able
     * to advance a field by accidentally reading the status register. */
    cur  = dev_read_word(w, 0);
    sh   = 8u * (4u - size - (a & 3u));
    mask = ((1u << (8u * size)) - 1u) << sh;
    dev_write_word(w, (cur & ~mask) | ((v << sh) & mask));
}

/* ---- public accessors ---------------------------------------------------- */
uint8_t bus_read8(uint32_t a)
{
    uint8_t *p = direct(a, 1, 0);
    if (p) return *p;
    return (uint8_t)dev_read(a, 1);
}

uint16_t bus_read16(uint32_t a)
{
    uint8_t *p = direct(a, 2, 0);
    if (p) return be16(p);
    return (uint16_t)dev_read(a, 2);
}

uint32_t bus_read32(uint32_t a)
{
    uint8_t *p = direct(a, 4, 0);
    if (p) return be32(p);
    return dev_read(a, 4);
}

uint64_t bus_read64(uint32_t a)
{
    return ((uint64_t)bus_read32(a) << 32) | bus_read32(a + 4);
}

void bus_write8(uint32_t a, uint8_t v)
{
    uint8_t *p = direct(a, 1, 1);
    if (p) { *p = v; return; }
    dev_write(a, v, 1);
}

void bus_write16(uint32_t a, uint16_t v)
{
    uint8_t *p = direct(a, 2, 1);
    if (p) { wbe16(p, v); return; }
    dev_write(a, v, 2);
}

void bus_write32(uint32_t a, uint32_t v)
{
    uint8_t *p = direct(a, 4, 1);
    if (p) { wbe32(p, v); return; }
    dev_write(a, v, 4);
}

void bus_write64(uint32_t a, uint64_t v)
{
    bus_write32(a, (uint32_t)(v >> 32));
    bus_write32(a + 4, (uint32_t)v);
}

void bus_dma_copy(uint32_t dst, uint32_t src, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++)
        bus_write8(dst + i, bus_read8(src + i));
}
