/* m3_touch: floating steering, one steering finger, pedals with steering, a
 * tap that lasts until the pad is read, gear taps as edges, a pad present
 * with no finger down (the gear lever stays put), columns that fit the bars
 * beside the picture, layouts that fit and do not overlap, and the gear
 * lever read back from register 0x08. */
#include "../src/touch.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static m3_pad_t read_pad(m3_touch *t)
{
    m3_pad_t p;
    memset(&p, 0, sizeof p);
    m3_touch_pad(t, &p);
    return p;
}

int main(void)
{
    m3_touch t;
    m3_pad_t p;
    const m3_touch_box *steer = &t.box[M3_TOUCH_STEER], *gas = &t.box[M3_TOUCH_GAS];
    float sx, sy, travel;
    int i, j, k;

    memset(&t, 0, sizeof t);
    m3_touch_layout(&t, 0, 0, 2400, 1080, 2.75f, 70, -1);
    sx = steer->x + 400; sy = steer->y + 900; travel = t.range * t.unit;

    p = read_pad(&t);
    CHECK(p.present && !p.buttons && p.sx == 0); /* nothing touched: present, idle */
    p.buttons = M3_PAD_B; p.sx = -40;      /* a real pad's buttons and stick are kept */
    m3_touch_pad(&t, &p);
    CHECK(p.buttons == M3_PAD_B && p.sx == -40);

    CHECK(m3_touch_down(&t, 1, sx, sy) == M3_TOUCH_STEER);
    p = read_pad(&t);
    CHECK(p.present && p.sx == 0); /* where the thumb lands is straight ahead */
    m3_touch_down(&t, 2, gas->x + 5, gas->y + 5);
    m3_touch_move(&t, 1, sx + travel, sy);
    p = read_pad(&t);
    CHECK(p.sx == 127 && (p.buttons & M3_PAD_A));
    m3_touch_move(&t, 1, sx + 2 * travel, sy); /* past full lock: the centre follows */
    m3_touch_move(&t, 1, sx + travel, sy);     /* back: straight at once */
    CHECK(read_pad(&t).sx == 0);
    m3_touch_move(&t, 1, sx, sy);
    CHECK(read_pad(&t).sx == -127);
    CHECK(m3_touch_down(&t, 3, steer->x + 30, sy) == -1); /* a second thumb on the left does not steer */
    CHECK(read_pad(&t).sx == -127);
    m3_touch_up(&t, 1);
    m3_touch_up(&t, 2);
    p = read_pad(&t);
    CHECK(p.sx == 0 && !(p.buttons & M3_PAD_A));

    /* a gear tap shorter than a field: pressed for one read, then released (an edge) */
    m3_touch_clear(&t);
    m3_touch_down(&t, 4, t.box[M3_TOUCH_GEAR_UP].x + 5, t.box[M3_TOUCH_GEAR_UP].y + 5);
    m3_touch_up(&t, 4);
    CHECK(read_pad(&t).buttons & M3_PAD_UP);
    CHECK(!(read_pad(&t).buttons & M3_PAD_UP));
    /* a finger that slides off its pedal lets go of it */
    m3_touch_down(&t, 5, gas->x + 5, gas->y + 5);
    (void)read_pad(&t);
    m3_touch_move(&t, 5, t.box[M3_TOUCH_BRAKE].x + 5, t.box[M3_TOUCH_BRAKE].y + 5);
    p = read_pad(&t);
    CHECK(!(p.buttons & (M3_PAD_A | M3_PAD_B)));
    m3_touch_up(&t, 5);
    m3_touch_down(&t, 6, t.box[M3_TOUCH_COIN].x + 5, t.box[M3_TOUCH_COIN].y + 5);
    CHECK(read_pad(&t).buttons & M3_PAD_Y);

    {   /* a handheld, phones (one at 2.75 pixels a dp), a 4:3 tablet */
        static const float dims[][3] = { {480, 272, 1}, {844, 390, 1}, {2400, 1080, 2.75f},
                                         {1024, 768, 1}, {2048, 1536, 2} };
        for (k = 0; k < 5; k++) {
            const float w = dims[k][0], h = dims[k][1];
            m3_touch_layout(&t, 20, 10, w, h, dims[k][2], 200, k == 2 ? 470 : -1);
            {   /* the gear lever's positions: inside, over the gear buttons, clear of VIEW */
                const m3_touch_box *gv = &t.gears, *up = &t.box[M3_TOUCH_GEAR_UP], *view = &t.box[M3_TOUCH_VIEW];
                CHECK(gv->x >= 20 && gv->x + gv->w <= 20 + w + 0.01f);
                CHECK(gv->y + gv->h <= up->y && gv->y >= view->y + view->h);
            }
            for (i = 0; i < M3_TOUCH_COUNT; i++) {
                const m3_touch_box *a = &t.box[i];
                CHECK(a->x >= 20 && a->y >= 10 && a->x + a->w <= 20 + w + 0.01f && a->y + a->h <= 10 + h + 0.01f);
                if (i == M3_TOUCH_STEER) continue;
                for (j = i + 1; j < M3_TOUCH_COUNT; j++) {
                    const m3_touch_box *b = &t.box[j];
                    if (j == M3_TOUCH_STEER) { /* only COIN and START lie over the steering zone */
                        CHECK(a->x >= b->x + b->w || i == M3_TOUCH_COIN || i == M3_TOUCH_START);
                        continue;
                    }
                    CHECK(a->x + a->w <= b->x || b->x + b->w <= a->x || a->y + a->h <= b->y || b->y + b->h <= a->y);
                }
            }
        }
    }
    {   /* a 20:9 phone's bars (470 px at 2.8 px a dp): each column stays in its bar */
        const float side = 470, w = 2340;
        m3_touch_layout(&t, 0, 0, w, 1080, 2.8125f, 70, side);
        for (i = 0; i < M3_TOUCH_STEER; i++) {
            const m3_touch_box *a = &t.box[i];
            CHECK(a->x + a->w <= side || a->x >= w - side);
        }
        CHECK(t.rest_x - t.range * t.unit >= 0 && t.rest_x + t.range * t.unit <= side);
    }
    CHECK(m3_touch_gear(0x50) == 1 && m3_touch_gear(0x60 | 0x02) == 2 && m3_touch_gear(0x10) == 3);
    CHECK(m3_touch_gear(0x20 | 0x0F) == 4 && m3_touch_gear(0x01) == 0);

    if (failures) return 1;
    printf("test_touch: ok\n");
    return 0;
}
