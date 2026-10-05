/* model3recomp -- the cabinet's inputs, one record per field.
 *
 * The board reads its buttons and light guns many times in a field. They are
 * latched once, at the field boundary, and every read in the next field sees
 * the same record. That is what a lockstep netplay session needs -- both
 * machines must hand the guest identical inputs on identical fields -- and it
 * also makes a scripted run independent of how often the guest happens to
 * look.
 */
#ifndef MODEL3RECOMP_INPUT_H
#define MODEL3RECOMP_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cabinet buttons, both players. A set bit means pressed; the board's
 * registers are active low and bus.c inverts. */
#define M3_BTN_COIN1    0x0001u
#define M3_BTN_COIN2    0x0002u
#define M3_BTN_TEST     0x0004u
#define M3_BTN_SERVICE  0x0008u
#define M3_BTN_START1   0x0010u
#define M3_BTN_START2   0x0020u
#define M3_BTN_TRIG1    0x0040u   /* light gun trigger                    */
#define M3_BTN_TRIG2    0x0080u
#define M3_BTN_OFFSCR1  0x0100u   /* pointed off screen, which is reload  */
#define M3_BTN_OFFSCR2  0x0200u

/* The light gun board reports ten bits a coordinate. Across the visible
 * screen it runs X 150..651 and Y 80..465 -- Supermodel's calibration for
 * the Model 3 gun games, and The Lost World's stage select agrees. */
#define M3_GUN_X0 150
#define M3_GUN_X1 651
#define M3_GUN_Y0 80
#define M3_GUN_Y1 465

/* A host pad as the platform saw it, for a game's input_map: buttons
 * (M3_PAD_*), the analog stick (-128..127, centre 0) and the triggers
 * (0..255). */
#define M3_PAD_A      0x0001u
#define M3_PAD_B      0x0002u
#define M3_PAD_X      0x0004u
#define M3_PAD_Y      0x0008u
#define M3_PAD_START  0x0010u
#define M3_PAD_UP     0x0020u
#define M3_PAD_DOWN   0x0040u
#define M3_PAD_LEFT   0x0080u
#define M3_PAD_RIGHT  0x0100u
typedef struct m3_pad {
    uint16_t buttons;
    int8_t   sx, sy;
    uint8_t  lt, rt;
    uint8_t  present;
} m3_pad_t;

typedef struct m3_input {
    uint32_t buttons;           /* M3_BTN_* */
    uint8_t  adc[8];            /* analog channels: wheel, pedals ... */
    uint8_t  in08, in0C;        /* game inputs, registers 0x08 / 0x0C, set = pressed */
    m3_pad_t pad[2];            /* the host's pads, before input_map */
    uint16_t gun_x[2], gun_y[2];/* board units, players 1 and 2 */
    uint32_t cheats;            /* game-defined bits, see m3_cheat_* */
    uint32_t options;           /* game options, four bits each, see m3_option_* */
} m3_input_t;

/* The record the guest sees this field. */
const m3_input_t *m3_input(void);

/* Latch the next field's record: sample the host, add scripted input, and
 * trade with a netplay peer if there is one. model3recomp_end_frame() calls
 * this; nothing else should. */
void m3_input_latch(void);

/* Screen pixel (0..w-1, 0..h-1) to gun coordinates. */
void m3_gun_from_screen(int px, int py, int w, int h,
                        uint16_t *gx, uint16_t *gy);

/* Cheats a game offers. The host side of a netplay session decides them, so
 * they travel in the input record and both machines apply the same ones. A
 * game registers its cheats with m3_cheat_add() and reads them back from
 * m3_input()->cheats. */
void        m3_cheat_add(unsigned bit, const char *label);
const char *m3_cheat_label(unsigned bit);   /* NULL if not registered */
uint32_t    m3_cheats_local(void);
void        m3_cheats_set_local(uint32_t bits);

/* A one-shot cheat -- "add credits" rather than "infinite health": the
 * menu item sets its bit for a single field. */
void        m3_cheat_add_action(unsigned bit, const char *label);
int         m3_cheat_is_action(unsigned bit);
void        m3_cheat_pulse(unsigned bit);

/* Game options: a choice per slot (0..7), four bits each in the record,
 * so on a netplay session the host's choice holds on both machines. The
 * window saves them with its other settings. A game reads its choice with
 * m3_option(slot). */
void        m3_option_add(unsigned slot, const char *label,
                          const char *const *choices, unsigned n, unsigned dflt);
const char *m3_option_label(unsigned slot);  /* NULL if not registered */
const char *m3_option_choice(unsigned slot, unsigned i);
unsigned    m3_option_count(unsigned slot);
unsigned    m3_option_local(unsigned slot);
void        m3_option_set_local(unsigned slot, unsigned v);
unsigned    m3_option(unsigned slot);         /* as the guest sees it this field */
uint32_t    m3_options_local_all(void);
void        m3_options_set_local_all(uint32_t v); /* from saved settings */

#ifdef __cplusplus
}
#endif
#endif
