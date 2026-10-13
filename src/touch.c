/* Touch controls: layout, fingers and the pad they make (touch.h). */
#include "touch.h"

#include <math.h>
#include <string.h>

static float minf(float a, float b) { return a < b ? a : b; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static int inside(const m3_touch_box *b, float x, float y)
{
    return x >= b->x && x <= b->x + b->w && y >= b->y && y <= b->y + b->h;
}

static void update(m3_touch *t)
{
    int k;
    memset(t->held, 0, sizeof t->held);
    t->steering = 0;
    t->steer = 0;
    for (k = 0; k < t->fingers; k++) {
        const int b = t->finger[k].box;
        if (b == M3_TOUCH_STEER) {
            const float travel = t->range * t->unit, x = t->finger[k].x;
            float v;
            t->steering = 1;
            t->origin_x = clampf(t->origin_x, x - travel, x + travel); /* the centre follows past full lock */
            t->finger_x = x;
            v = (x - t->origin_x) / travel;
            t->steer = v < 0 ? -fmaxf(0.f, (-v - .06f) / .94f) : fmaxf(0.f, (v - .06f) / .94f);
        } else if (inside(&t->box[b], t->finger[k].x, t->finger[k].y)) {
            t->held[b] = 1;
        }
    }
}

void m3_touch_layout(m3_touch *t, float x, float y, float w, float h, float dp, float range_dp,
                     float side)
{
    /* The columns need about 420 x 360 dp; smaller areas scale them down. */
    const float auto_dp = minf(w / 800.f, h / 360.f) * 2.f;
    float u, m, g, cw, half, right, bottom, gas_w, brake_w, pedal, gear_h, cells, gas_x, brake_x, pedal_y, gear_y;
    m3_touch_clear(t);
    u = minf(minf(dp > 0 ? dp : auto_dp, w / 420.f), h / 360.f);
    t->unit = u;
    t->range = clampf(range_dp, 30.f, 200.f);
    m = 12 * u; g = 8 * u; right = x + w; bottom = y + h;
    /* A column fills its bar: 12 dp from the screen's edge, 6 from the picture. */
    cw = side >= 0 ? clampf(side - 18 * u, 136 * u, 176 * u) : 164 * u;
    half = (cw - g) / 2;
    t->box[M3_TOUCH_COIN]  = (m3_touch_box){ x + m, y + m, half, 44 * u };
    t->box[M3_TOUCH_START] = (m3_touch_box){ x + m + half + g, y + m, half, 44 * u };
    t->box[M3_TOUCH_VIEW]  = (m3_touch_box){ right - m - half, y + m, half, 44 * u };
    brake_w = (cw - g) * .47f; gas_w = cw - g - brake_w; pedal = 120 * u; gear_h = 46 * u; cells = 24 * u;
    gas_x = right - m - gas_w; brake_x = gas_x - g - brake_w;
    pedal_y = bottom - m - pedal; gear_y = pedal_y - g - gear_h;
    t->box[M3_TOUCH_BRAKE]     = (m3_touch_box){ brake_x, pedal_y, brake_w, pedal };
    t->box[M3_TOUCH_GAS]       = (m3_touch_box){ gas_x, pedal_y, gas_w, pedal };
    t->box[M3_TOUCH_GEAR_DOWN] = (m3_touch_box){ brake_x, gear_y, brake_w, gear_h };
    t->box[M3_TOUCH_GEAR_UP]   = (m3_touch_box){ gas_x, gear_y, gas_w, gear_h };
    t->box[M3_TOUCH_STEER]     = (m3_touch_box){ x, y, w * .5f, h };
    t->gears = (m3_touch_box){ brake_x, gear_y - g - cells, cw, cells };
    t->rest_x = x + m + cw / 2;      /* the wheel at rest: in the left column, near the bottom */
    t->rest_y = bottom - m - 22 * u;
}

void m3_touch_clear(m3_touch *t)
{
    t->fingers = 0;
    memset(t->held, 0, sizeof t->held);
    memset(t->tapped, 0, sizeof t->tapped);
    t->steering = 0;
    t->steer = 0;
}

void m3_touch_up(m3_touch *t, int64_t id)
{
    int k, n = 0;
    for (k = 0; k < t->fingers; k++)
        if (t->finger[k].id != id) t->finger[n++] = t->finger[k];
    t->fingers = n;
    update(t);
}

int m3_touch_down(m3_touch *t, int64_t id, float x, float y)
{
    int b;
    m3_touch_up(t, id);
    for (b = 0; b < M3_TOUCH_COUNT; b++) {
        if (!inside(&t->box[b], x, y)) continue;
        if (b == M3_TOUCH_STEER && t->steering) return -1; /* one steering finger */
        if (t->fingers < (int)(sizeof t->finger / sizeof t->finger[0])) {
            t->finger[t->fingers].id = id;
            t->finger[t->fingers].box = b;
            t->finger[t->fingers].x = x;
            t->finger[t->fingers].y = y;
            t->fingers++;
            if (b == M3_TOUCH_STEER) t->origin_x = t->finger_x = x, t->origin_y = y;
            t->tapped[b] = 1;
        }
        update(t);
        return b;
    }
    return -1;
}

void m3_touch_move(m3_touch *t, int64_t id, float x, float y)
{
    int k;
    for (k = 0; k < t->fingers; k++)
        if (t->finger[k].id == id) t->finger[k].x = x, t->finger[k].y = y;
    update(t);
}

void m3_touch_pad(m3_touch *t, m3_pad_t *p)
{
    int on[M3_TOUCH_COUNT], b;
    for (b = 0; b < M3_TOUCH_COUNT; b++) {
        on[b] = t->held[b] || t->tapped[b];
        t->tapped[b] = 0;
    }
    p->present = 1;
    if (t->steering) p->sx = (int8_t)lrintf(clampf(t->steer, -1.f, 1.f) * 127.f);
    if (on[M3_TOUCH_GAS])       p->buttons |= M3_PAD_A;
    if (on[M3_TOUCH_BRAKE])     p->buttons |= M3_PAD_B;
    if (on[M3_TOUCH_GEAR_UP])   p->buttons |= M3_PAD_UP;
    if (on[M3_TOUCH_GEAR_DOWN]) p->buttons |= M3_PAD_DOWN;
    if (on[M3_TOUCH_VIEW])      p->buttons |= M3_PAD_X;
    if (on[M3_TOUCH_COIN])      p->buttons |= M3_PAD_Y;
    if (on[M3_TOUCH_START])     p->buttons |= M3_PAD_START;
}

int m3_touch_gear(uint8_t in08)
{
    switch (in08 & 0x70u) {
    case 0x50: return 1;
    case 0x60: return 2;
    case 0x10: return 3;
    case 0x20: return 4;
    default:   return 0;
    }
}
