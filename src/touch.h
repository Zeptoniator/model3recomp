/* Touch controls for a driving game on a phone: where the buttons are, which
 * finger holds what, and the pad (m3_pad_t) they make for the game's
 * input_map. No SDL here: the platform layer feeds fingers in and draws.
 *
 * Layout, in dp (one dp is `unit` touch units): a column in each bar beside
 * the picture, as wide as the bar allows (136 to 176 dp; a 20:9 phone's bars
 * are about 165), so nothing covers the game. Top left COIN and START; top
 * right VIEW (the next view); bottom right the gear lever's four positions
 * over the gear buttons over BRAKE and GAS; bottom left, at rest, the wheel.
 * Steering floats: a finger landing anywhere in the left half is straight
 * ahead, and sliding `range` dp to a side is full lock; past it the centre
 * follows the finger, so turning back answers at once. A finger owns what it
 * landed on until it lifts, so steering and pedals work together. A tap
 * shorter than a field still reaches the game: it is held until the pad has
 * been read once. */
#ifndef MODEL3RECOMP_TOUCH_H
#define MODEL3RECOMP_TOUCH_H

#include "model3recomp/input.h"

#include <stdint.h>

enum {
    M3_TOUCH_COIN, M3_TOUCH_START, M3_TOUCH_VIEW, M3_TOUCH_GEAR_DOWN, M3_TOUCH_GEAR_UP,
    M3_TOUCH_BRAKE, M3_TOUCH_GAS, M3_TOUCH_STEER, /* the steering zone: last, under the buttons */
    M3_TOUCH_COUNT
};

typedef struct m3_touch_box {
    float x, y, w, h;
} m3_touch_box;

typedef struct m3_touch {
    m3_touch_box box[M3_TOUCH_COUNT];
    m3_touch_box gears;               /* the gear lever's positions, shown over the gear buttons */
    float unit;                       /* one dp in touch units */
    float range;                      /* dp of travel to full lock */
    float rest_x, rest_y;             /* the steering track's centre while no finger steers */
    int held[M3_TOUCH_COUNT];         /* a finger is on it now */
    int tapped[M3_TOUCH_COUNT];       /* pressed since the pad was last read */
    int steering;                     /* a finger is steering */
    float origin_x, origin_y, finger_x;
    float steer;                      /* -1 (left) .. 1 (right) */
    struct { int64_t id; int box; float x, y; } finger[10];
    int fingers;
} m3_touch;

/* area: the window (x, y, w, h); dp: one dp in its units (0: from its size);
 * side: the width of each bar beside the picture (negative: not known). */
void m3_touch_layout(m3_touch *t, float x, float y, float w, float h, float dp, float range_dp,
                     float side);
/* Returns the control the finger landed on (M3_TOUCH_*), or -1. */
int  m3_touch_down(m3_touch *t, int64_t id, float x, float y);
void m3_touch_move(m3_touch *t, int64_t id, float x, float y);
void m3_touch_up(m3_touch *t, int64_t id);
void m3_touch_clear(m3_touch *t);
/* Adds the touch controls to a pad (the stick's x, the triggers, buttons) and
 * forgets the taps it has now passed on. The pad is present even with no
 * finger down -- the touch controls are always there -- so the game's
 * input_map keeps the gear lever where it was put; a real pad's buttons and
 * stick are kept, the touches only add to them. */
void m3_touch_pad(m3_touch *t, m3_pad_t *p);
/* The gear lever's position (1-4) in register 0x08 as a shift4 input_map
 * writes it (Supermodel's wiring: 1st 0x50, 2nd 0x60, 3rd 0x10, 4th 0x20),
 * 0 for none. */
int  m3_touch_gear(uint8_t in08);

#endif
