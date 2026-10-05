/* SCUD.DAT reader (format: tools/scud/pack.py). Plugs into m3_roms_t.read. */
#ifndef SCUDPACK_H
#define SCUDPACK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE     *f;
    uint32_t  page, count, prog_bytes, bank_bytes, first, hot;
    uint16_t *slot[2];            /* page -> position in the file */
    uint32_t  pages[2];
    uint8_t  *ra;                 /* read-ahead window, ra_cap pages */
    uint32_t  ra_first, ra_n, ra_cap;
    long      pos;                /* file position, to skip seeks */
    uint64_t  reads;              /* fread calls on the file */
} scudpack_t;

/* 0 on failure (message on stderr). ra_pages: pages read per miss. */
int    scudpack_open(scudpack_t *p, const char *path, unsigned ra_pages);
size_t scudpack_read(void *ctx, int which, uint32_t off, void *dst, size_t len);

#endif
