/* model3recomp -- Real3D scene renderer.
 *
 * Walks the scene the game builds every field -- viewports, a tree of culling
 * nodes, models -- and rasterises it with a depth buffer.
 *
 * Every structure below was read off a live scene dump and then checked
 * against something that could only come out right if the decode was right.
 * That matters more here than anywhere else in the emulator, because a 3D
 * pipeline decoded wrongly does not fail: it draws. It draws confident,
 * plausible, wrong geometry, and there is no screen to check it against --
 * MAME does not render Model 3 3D at all, so the oracle that settled the
 * tilemaps is not available for any of this.
 *
 * The checks, per structure:
 *
 *   viewport      word 0x14 decodes to 496x384, the exact display size, and
 *                 words 0x0C..0x0F are sin/cos pairs for 19.38 and 15
 *                 degrees -- whose tangent ratio, 1.3125, is the aspect of a
 *                 496x384 screen.
 *   culling node  ten words; a run of them at stride 0x0A has [00] rising by
 *                 0x400 and the matrix index in [03] by 1, every time.
 *   pointer list  entries are addresses of valid nodes, and the unused ones
 *                 are all the same dummy, 0x800800.
 *   matrix        twelve floats, translation first, then a 3x3 by rows.
 *                 Matrix 1 is [0,0,0, 1,0,0, 0,1,0, 0,0,1], which is exactly
 *                 identity under this reading and nothing under any other.
 *   polygon       the whole of polygon RAM tiles into models with zero slack
 *                 -- the last model ends on the last word -- under the rule
 *                 that a header is seven words followed by four words per
 *                 vertex not reused from the previous polygon.
 *   normals       every one of the 1260 header normals in that block comes
 *                 out exactly unit length. This is the strongest check here,
 *                 and it is what pinned the 24-bit sign extension and the
 *                 2^22 scale.
 *   VROM          a 16-lane interleave read big-endian is the only
 *                 arrangement in which VROM contains any models at all:
 *                 31265 of them, against none for the 8-lane image the ROM
 *                 builder used to produce. See tools/rom_loader.py.
 *
 * What is NOT settled is the vertex scale -- see VERTEX_SCALE_DEFAULT.
 */
#include "model3recomp/real3d.h"
#include "model3recomp/bus.h"
#include "model3recomp/platform.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TRIS   40000u
#define MAX_DEPTH  24

/* The vertex fixed-point scale.
 *
 * This one is a calibration, not a derivation, and it is the weakest number
 * in the file. Two independent matrices in the same scene imply about
 * 1/3000: one is a pure rotation placing an object some 600 units away whose
 * raw half-extent is 655360, and another carries a scale of 45 with a
 * translation of 28000. Both want the object to come out a sensible size for
 * the field of view, and both land near 1/3000.
 *
 * 1/1024 is what is used, because it is a power of two in that region and
 * because it is the value that puts this scene's geometry on screen. It
 * wants checking against a scene with something recognisable in it -- a
 * dinosaur is either the right size or it is not -- and attract mode does
 * not reach one before a run times out. M3_VERTEX_SCALE overrides it.
 */
#define VERTEX_SCALE_DEFAULT 1024.0f

static float g_vertex_scale = VERTEX_SCALE_DEFAULT;
static unsigned g_tri_count;

unsigned real3d_last_tri_count(void) { return g_tri_count; }

/* ---- memory ------------------------------------------------------------
 * Culling and polygon RAM are little-endian, like everything else the
 * graphics side reads. VROM is the exception: see the note above.
 */
static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* A culling address is a word index, not a byte offset: viewport 0 links to
 * 0x800030 and the next viewport node begins exactly at word 0x30. */
static const uint8_t *cull_at(uint32_t addr, unsigned words)
{
    size_t n = 0;
    const uint8_t *m;
    uint32_t base;

    addr &= 0x00FFFFFFu;
    if (addr >= 0x800000u && addr < 0x840000u) {
        m = bus_cull_hi(&n);
        base = (addr & 0x3FFFFu) * 4u;
    } else if (addr < 0x100000u) {
        m = bus_cull_lo(&n);
        base = addr * 4u;
    } else {
        return NULL;
    }
    if (!m || (size_t)base + (size_t)words * 4u > n)
        return NULL;
    return m + base;
}

static float f32_of(uint32_t w)
{
    float f;
    memcpy(&f, &w, sizeof f);
    return f;
}

/* Sign-extend the top 24 bits of a word. */
static int32_t sx24(uint32_t w)
{
    uint32_t v = w >> 8;
    return (v & 0x800000u) ? (int32_t)(v | 0xFF000000u) : (int32_t)v;
}

/* ---- matrices ---------------------------------------------------------- */
typedef struct { float m[3][4]; } mat_t;

static const mat_t IDENT = {{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};

static int mat_load(uint32_t base, uint32_t off, mat_t *out)
{
    const uint8_t *p = cull_at(base + off * 12u, 12);
    float v[12];
    int i;
    if (!p) return 0;
    for (i = 0; i < 12; i++)
        v[i] = f32_of(rd32le(p + i * 4));
    out->m[0][0] = v[3];  out->m[0][1] = v[4];  out->m[0][2] = v[5];
    out->m[1][0] = v[6];  out->m[1][1] = v[7];  out->m[1][2] = v[8];
    out->m[2][0] = v[9];  out->m[2][1] = v[10]; out->m[2][2] = v[11];
    out->m[0][3] = v[0];  out->m[1][3] = v[1];  out->m[2][3] = v[2];
    return 1;
}

static mat_t mat_mul(const mat_t *a, const mat_t *b)
{
    mat_t o;
    int r, c;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++)
            o.m[r][c] = a->m[r][0]*b->m[0][c] + a->m[r][1]*b->m[1][c]
                      + a->m[r][2]*b->m[2][c];
        o.m[r][3] = a->m[r][0]*b->m[0][3] + a->m[r][1]*b->m[1][3]
                  + a->m[r][2]*b->m[2][3] + a->m[r][3];
    }
    return o;
}

static void mat_point(const mat_t *m, const float *p, float *o)
{
    int r;
    for (r = 0; r < 3; r++)
        o[r] = m->m[r][0]*p[0] + m->m[r][1]*p[1] + m->m[r][2]*p[2] + m->m[r][3];
}

static void mat_dir(const mat_t *m, const float *p, float *o)
{
    int r;
    for (r = 0; r < 3; r++)
        o[r] = m->m[r][0]*p[0] + m->m[r][1]*p[1] + m->m[r][2]*p[2];
}

/* ---- the triangle queue ------------------------------------------------ */
typedef struct {
    float v[3][3];          /* view space */
    float n[3];
    uint8_t r, g, b;
} tri_t;

static tri_t *g_tris;
static unsigned g_ntris;

static void emit(const float a[3], const float b[3], const float c[3],
                 const float n[3], uint32_t rgb)
{
    tri_t *t;
    if (g_ntris >= MAX_TRIS) return;
    t = &g_tris[g_ntris++];
    memcpy(t->v[0], a, sizeof t->v[0]);
    memcpy(t->v[1], b, sizeof t->v[1]);
    memcpy(t->v[2], c, sizeof t->v[2]);
    memcpy(t->n, n, sizeof t->n);
    t->r = (uint8_t)((rgb >> 16) & 0xFF);
    t->g = (uint8_t)((rgb >> 8) & 0xFF);
    t->b = (uint8_t)(rgb & 0xFF);
}

/* ---- models ------------------------------------------------------------ */
static void draw_model(uint32_t addr, const mat_t *m)
{
    const uint8_t *mem;
    size_t n = 0;
    uint32_t (*rd)(const uint8_t *);
    size_t i;
    float prev[4][3];
    unsigned nprev = 0;
    unsigned guard;

    if (addr & 0x800000u) {
        mem = bus_vrom(&n);
        rd = rd32be;                    /* VROM alone is read big-endian */
        i = (size_t)(addr & 0x7FFFFFu) * 4u;
    } else {
        mem = bus_poly(&n);
        rd = rd32le;
        i = (size_t)addr * 4u;
    }
    if (!mem) return;

    for (guard = 0; guard < 20000u; guard++) {
        uint32_t ph[7];
        float nrm[3], vs[4][3];
        unsigned nv, nnew, k, j, slot;
        uint32_t rgb;
        float len;

        if (i + 7u * 4u > n) return;
        for (k = 0; k < 7; k++) ph[k] = rd(mem + i + k * 4);

        /* Header words 1..3 are the polygon normal, 24-bit fixed over 2^22.
         * A real header's normal is a unit vector; this is the check that
         * catches a model pointer that is not a model, which otherwise
         * produces triangles rather than an error. */
        for (k = 0; k < 3; k++)
            nrm[k] = (float)sx24(ph[k + 1]) / 4194304.0f;
        len = nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2];
        if (len < 0.98f || len > 1.02f) return;

        nv = (ph[0] & 0x40u) ? 4u : 3u;
        slot = 0;
        for (k = 0; k < 4; k++)             /* bits 0..3: reuse from previous */
            if (((ph[0] >> k) & 1u) && k < nprev && slot < 4) {
                memcpy(vs[slot], prev[k], sizeof vs[slot]);
                slot++;
            }
        if (slot > nv) return;
        nnew = nv - slot;
        if (i + (7u + 4u * nnew) * 4u > n) return;

        for (k = 0; k < nnew && slot < 4; k++, slot++) {
            size_t o = i + (7u + 4u * k) * 4u;
            vs[slot][0] = (float)sx24(rd(mem + o))      / g_vertex_scale;
            vs[slot][1] = (float)sx24(rd(mem + o + 4))  / g_vertex_scale;
            vs[slot][2] = (float)sx24(rd(mem + o + 8))  / g_vertex_scale;
        }

        rgb = (((ph[4] >> 24) & 0xFFu) << 16) | (((ph[4] >> 16) & 0xFFu) << 8)
            | ((ph[4] >> 8) & 0xFFu);

        if (slot >= 3) {
            float wv[4][3], wn[3];
            for (j = 0; j < slot; j++) mat_point(m, vs[j], wv[j]);
            mat_dir(m, nrm, wn);
            for (j = 1; j + 1 < slot; j++)
                emit(wv[0], wv[j], wv[j+1], wn, rgb);
        }

        for (j = 0; j < slot; j++) memcpy(prev[j], vs[j], sizeof prev[j]);
        nprev = slot;

        i += (7u + 4u * nnew) * 4u;
        if (ph[1] & 4u) return;             /* last polygon of the model */
    }
}

/* ---- the node tree ----------------------------------------------------- */
static uint32_t g_matbase;

static void descend(uint32_t addr, const mat_t *m, int depth);

/* A pointer's top byte says what it points at: bit 24 a model, bit 26 a
 * pointer list, and otherwise another culling node. */
static void follow(uint32_t p, const mat_t *m, int depth)
{
    uint32_t a = p & 0x00FFFFFFu;

    if (a == 0 || a == 0x800800u)           /* 0x800800 is the dummy entry */
        return;

    if (p & 0x01000000u) {
        draw_model(a, m);
    } else if (p & 0x04000000u) {
        unsigned k;
        if (depth >= MAX_DEPTH) return;
        for (k = 0; k < 256u; k++) {
            const uint8_t *e = cull_at(a + k, 1);
            uint32_t v;
            if (!e) break;
            v = rd32le(e);
            /* Both flag bits together is a skip -- not an entry, and not an
             * end either. Treating it as an end stops the list at its first
             * element, which is exactly what an attract-mode list starts
             * with, and loses every model in it. */
            if ((v & 0x03000000u) == 0x03000000u)
                continue;
            follow(v, m, depth + 1);
            if (v & 0x02000000u)            /* last entry: after, not before */
                break;
        }
    } else {
        descend(a, m, depth + 1);
    }
}

static void descend(uint32_t addr, const mat_t *m, int depth)
{
    const uint8_t *node = cull_at(addr, 10);
    mat_t cur, mm;
    uint32_t mo;
    float tx, ty, tz;

    if (!node || depth >= MAX_DEPTH) return;
    cur = *m;

    mo = rd32le(node + 3 * 4) & 0xFFFu;
    if (mo && mat_load(g_matbase, mo, &mm))
        cur = mat_mul(m, &mm);

    tx = f32_of(rd32le(node + 4 * 4));
    ty = f32_of(rd32le(node + 5 * 4));
    tz = f32_of(rd32le(node + 6 * 4));
    if (tx != 0.0f || ty != 0.0f || tz != 0.0f) {
        mat_t t = IDENT;
        t.m[0][3] = tx; t.m[1][3] = ty; t.m[2][3] = tz;
        cur = mat_mul(&cur, &t);
    }

    follow(rd32le(node + 7 * 4), &cur, depth);
    follow(rd32le(node + 8 * 4), &cur, depth);
}

/* ---- rasteriser -------------------------------------------------------- */
static float *g_zbuf;
static int g_zw, g_zh;

static void raster(uint32_t *fb, int fw, int fh, float sx, float sy)
{
    /* A fixed key light. The hardware takes its light from the viewport,
     * which is not decoded yet; until it is, a constant direction at least
     * makes the shape of a surface readable instead of flat. */
    static const float L[3] = { 0.3f, 0.5f, -0.81f };
    unsigned t;

    for (t = 0; t < g_ntris; t++) {
        const tri_t *tr = &g_tris[t];
        float p[3][3];
        float d, lit, sh;
        int x0, x1, y0, y1, x, y, k;
        int r, g, b;
        uint32_t argb;

        if (tr->v[0][2] <= 0.001f || tr->v[1][2] <= 0.001f ||
            tr->v[2][2] <= 0.001f)
            continue;                        /* behind or on the eye plane */

        for (k = 0; k < 3; k++) {
            p[k][0] = fw * 0.5f + tr->v[k][0] / tr->v[k][2] * sx;
            p[k][1] = fh * 0.5f - tr->v[k][1] / tr->v[k][2] * sy;
            p[k][2] = tr->v[k][2];
        }

        d = (p[1][1]-p[2][1])*(p[0][0]-p[2][0])
          + (p[2][0]-p[1][0])*(p[0][1]-p[2][1]);
        if (d > -1e-6f && d < 1e-6f) continue;

        lit = tr->n[0]*L[0] + tr->n[1]*L[1] + tr->n[2]*L[2];
        if (lit < 0) lit = -lit;
        if (lit > 1.0f) lit = 1.0f;
        sh = 0.25f + 0.75f * lit;
        r = (int)(tr->r * sh); g = (int)(tr->g * sh); b = (int)(tr->b * sh);
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8)
             | (uint32_t)b;

        x0 = (int)floorf(fminf(fminf(p[0][0], p[1][0]), p[2][0]));
        x1 = (int)ceilf (fmaxf(fmaxf(p[0][0], p[1][0]), p[2][0]));
        y0 = (int)floorf(fminf(fminf(p[0][1], p[1][1]), p[2][1]));
        y1 = (int)ceilf (fmaxf(fmaxf(p[0][1], p[1][1]), p[2][1]));
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > fw - 1) x1 = fw - 1;
        if (y1 > fh - 1) y1 = fh - 1;

        for (y = y0; y <= y1; y++) {
            for (x = x0; x <= x1; x++) {
                float l0, l1, l2, z;
                int o;
                l0 = ((p[1][1]-p[2][1])*(x-p[2][0])
                    + (p[2][0]-p[1][0])*(y-p[2][1])) / d;
                l1 = ((p[2][1]-p[0][1])*(x-p[2][0])
                    + (p[0][0]-p[2][0])*(y-p[2][1])) / d;
                l2 = 1.0f - l0 - l1;
                if (l0 < 0 || l1 < 0 || l2 < 0) continue;
                z = l0*p[0][2] + l1*p[1][2] + l2*p[2][2];
                o = y * fw + x;
                if (z < g_zbuf[o]) { g_zbuf[o] = z; fb[o] = argb; }
            }
        }
    }
}

/* ---- entry point ------------------------------------------------------- */
void real3d_render(void)
{
    int fw = 0, fh = 0;
    uint32_t *fb = platform_framebuffer(&fw, &fh);
    uint32_t vpaddr;
    unsigned guard, total = 0;
    static int inited;

    if (!fb) return;

    if (!inited) {
        const char *e = getenv("M3_VERTEX_SCALE");
        if (e && atof(e) > 0.0) g_vertex_scale = (float)atof(e);
        g_tris = (tri_t *)malloc(sizeof(tri_t) * MAX_TRIS);
        inited = 1;
    }
    if (!g_tris) return;

    memset(fb, 0, (size_t)fw * (size_t)fh * sizeof(uint32_t));

    if (g_zw != fw || g_zh != fh) {
        free(g_zbuf);
        g_zbuf = (float *)malloc(sizeof(float) * (size_t)fw * (size_t)fh);
        g_zw = fw; g_zh = fh;
    }
    if (!g_zbuf) return;
    {
        int i, nn = fw * fh;
        for (i = 0; i < nn; i++) g_zbuf[i] = 1e30f;
    }

    vpaddr = 0x800000u;
    for (guard = 0; guard < 16u && vpaddr; guard++) {
        const uint8_t *vp = cull_at(vpaddr, 0x20);
        mat_t cam;
        float hf, vf, sx, sy;
        uint32_t next;

        if (!vp) break;
        g_matbase = rd32le(vp + 0x16 * 4) & 0x00FFFFFFu;

        /* Words 0x0C and 0x0E are the sines of the horizontal and vertical
         * half angles. For this game they are 19.38 and 15 degrees, whose
         * tangents are in the ratio 1.3125 -- the aspect of a 496x384
         * screen, which is how the pair was identified. */
        hf = f32_of(rd32le(vp + 0x0C * 4));
        vf = f32_of(rd32le(vp + 0x0E * 4));
        if (hf < -1.0f) hf = -1.0f;
        if (hf > 1.0f) hf = 1.0f;
        if (vf < -1.0f) vf = -1.0f;
        if (vf > 1.0f) vf = 1.0f;
        hf = asinf(hf); vf = asinf(vf);
        if (hf < 1e-3f || vf < 1e-3f) { hf = 0.338f; vf = 0.262f; }
        sx = (fw * 0.5f) / tanf(hf);
        sy = (fh * 0.5f) / tanf(vf);

        /* Matrix 0 is the camera. It is the only one of the table's first
         * four that puts any of this scene in front of the eye, and it is a
         * plain rotation. */
        if (!mat_load(g_matbase, 0, &cam)) cam = IDENT;

        g_ntris = 0;
        descend(rd32le(vp + 0x02 * 4) & 0x00FFFFFFu, &cam, 0);
        raster(fb, fw, fh, sx, sy);
        total += g_ntris;

        next = rd32le(vp + 0x01 * 4) & 0x00FFFFFFu;
        if (next == vpaddr) break;
        vpaddr = next;
    }
    g_tri_count = total;
}
