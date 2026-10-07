/* SCUD.DAT reader. The pack holds every 32 KB page of both CROMs and the
 * VROM (version 2; version 1 had the CROMs only), the ones
 * the game touches first laid out first, in that order (tools/scud/pack.py).
 * A miss reads a window of consecutive pages from the file -- which, by
 * construction, are the pages the game is about to want -- so the disc is
 * read in long sequential runs instead of one seek per page. */
#include "scudpack.h"

#include <stdlib.h>
#include <string.h>
#ifdef _arch_dreamcast
#include <malloc.h>
/* KOS's ISO9660 reads whole sectors by DMA only into a 32-byte-aligned
 * buffer; anything else goes a sector at a time through its cache. That
 * was the difference between ~120 KB/s and the drive's real rate. */
#define RA_ALLOC(n) memalign(32, (n))
#else
#define RA_ALLOC(n) malloc(n)
#endif

static uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

int scudpack_open(scudpack_t *p, const char *path, unsigned ra_pages)
{
    uint8_t h[0x40], *idx;
    uint32_t i, ver;

    memset(p, 0, sizeof *p);
    p->pos = -1;
    if (!(p->f = fopen(path, "rb")) || fread(h, 1, sizeof h, p->f) != sizeof h ||
        memcmp(h, "SCUDPACK", 8) || (ver = le32(h + 8)) < 1 || ver > 2) {
        fprintf(stderr, "%s: not a SCUD.DAT (tools/scud/pack.py)\n", path);
        return 0;
    }
    p->page = le32(h + 12);   p->count = le32(h + 16);
    p->prog_bytes = le32(h + 20); p->bank_bytes = le32(h + 24);
    p->first = le32(h + 28);  p->hot = le32(h + 32);
    p->vrom_bytes = ver >= 2 ? le32(h + 36) : 0;
    p->pages[0] = (p->prog_bytes + p->page - 1) / p->page;
    p->pages[1] = (p->bank_bytes + p->page - 1) / p->page;
    p->pages[2] = (p->vrom_bytes + p->page - 1) / p->page;
    for (i = 0; i < 3; i++)
        p->slot[i] = calloc(p->pages[i] + 1, sizeof(uint16_t));
    idx = malloc((size_t)p->count * 4u);
    p->ra_cap = ra_pages ? ra_pages : 1;
    p->ra = RA_ALLOC((size_t)p->ra_cap * p->page);
    /* No stdio buffer: fread then hands the aligned window to the file
     * system directly. */
    setvbuf(p->f, NULL, _IONBF, 0);
    if (!p->slot[0] || !p->slot[1] || !p->slot[2] || !idx || !p->ra ||
        fread(idx, 4, p->count, p->f) != p->count) {
        fprintf(stderr, "%s: short or out of memory\n", path);
        free(idx);
        return 0;
    }
    for (i = 0; i < p->count; i++) {
        uint32_t e = le32(idx + 4 * i);
        uint32_t w = ver >= 2 ? e >> 30 : e >> 31;
        uint32_t pg = e & (ver >= 2 ? 0x3FFFFFFFu : 0x7FFFFFFFu);
        if (w > 2) continue;
        if (pg < p->pages[w]) p->slot[w][pg] = (uint16_t)i;
    }
    free(idx);
    p->ra_n = 0;
    return 1;
}

size_t scudpack_read(void *ctx, int which, uint32_t off, void *dst, size_t len)
{
    scudpack_t *p = (scudpack_t *)ctx;
    uint8_t *out = (uint8_t *)dst;
    size_t done = 0;

    if (which < 0 || which > 2) return 0;
    while (done < len) {
        uint32_t pg = (off + done) / p->page, in = (off + done) % p->page;
        uint32_t s, n;
        size_t take;
        if (pg >= p->pages[which]) break;
        s = p->slot[which][pg];
        if (which == 2 || s >= p->hot) {
            /* Outside the hot pages -- the ones the game touched first, in
             * the order it touched them, laid out first -- reads are not in
             * file order: the VROM is read by the renderer a 4 KB piece at a
             * time, anywhere, and so is the banked CROM in a race. A
             * read-ahead window there is 256 KB of disc per miss for pages
             * nobody asked for (1214 s of a 2983 s race). Exactly the bytes,
             * straight into the caller's aligned slot. */
            long at = (long)p->first + (long)s * (long)p->page + (long)in;
            size_t got;
            take = p->page - in;
            if (take > len - done) take = len - done;
            if (p->pos != at && fseek(p->f, at, SEEK_SET)) break;
            got = fread(out + done, 1, take, p->f);
            p->reads++;
            p->pos = at + (long)got;
            done += got;
            if (got != take) break;
            continue;
        }
        if (!(p->ra_n && s >= p->ra_first && s < p->ra_first + p->ra_n)) {
            long at = (long)p->first + (long)s * (long)p->page;
            n = p->count - s < p->ra_cap ? p->count - s : p->ra_cap;
            if (p->pos != at && fseek(p->f, at, SEEK_SET)) break;
            p->ra_n = (uint32_t)(fread(p->ra, p->page, n, p->f));
            p->reads++;
            p->ra_first = s;
            p->pos = at + (long)p->ra_n * (long)p->page;
            if (!p->ra_n) break;
        }
        take = p->page - in;
        if (take > len - done) take = len - done;
        memcpy(out + done, p->ra + (size_t)(s - p->ra_first) * p->page + in, take);
        done += take;
    }
    return done;
}
