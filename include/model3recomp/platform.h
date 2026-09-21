/* model3recomp -- host platform layer (window, input, timing, presentation). */
#ifndef MODEL3RECOMP_PLATFORM_H
#define MODEL3RECOMP_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int      platform_init(int w, int h, const char *title);
void     platform_shutdown(void);
int      platform_poll(void);          /* 0 when the user wants to quit */
void     platform_present(void);
uint64_t platform_ticks_us(void);

/* Cabinet buttons the host is holding down, as a bitmask. The board's I/O
 * registers are active low; this is not -- a set bit means pressed. */
#define M3_BTN_COIN1    0x0001u
#define M3_BTN_COIN2    0x0002u
#define M3_BTN_TEST     0x0004u
#define M3_BTN_SERVICE  0x0008u
#define M3_BTN_START1   0x0010u
#define M3_BTN_START2   0x0020u

unsigned platform_buttons(void);

/* The framebuffer renderers draw into: w*h pixels, 0xAARRGGBB. */
uint32_t *platform_framebuffer(int *w, int *h);

#ifdef __cplusplus
}
#endif
#endif
