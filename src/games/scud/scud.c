/* Scud Race -- title data shared by every host. */
#include "scud.h"

#include "scudrom_funcs.h"
#include "scudram_funcs.h"

#include "model3recomp/bus.h"

#include <string.h>

/* A pad as a Scud Race cabinet (Supermodel's Games.xml: vehicle, shift4,
 * vr4). The ADC: channel 0 the wheel (0x80 centred), 1 the accelerator, 2
 * the brake. Register 0x08: the four view buttons in bits 0..3, and the
 * four-position gear lever as Supermodel wires it -- 1st 0x50, 2nd 0x60,
 * 3rd 0x10, 4th 0x20 (bits pulled low; set here, the bus inverts).
 *
 *   stick / d-pad left-right  wheel        R / A   accelerator
 *   L / B                     brake        up / down  gear up / down
 *   X                         next view    Y  coin, Start  start */
static const uint8_t k_gear_bits[5] = { 0x00, 0x50, 0x60, 0x10, 0x20 };

static void scud_input_map(m3_input_t *in)
{
    static unsigned gear = 1, view, prev;
    const m3_pad_t *p = &in->pad[0];
    unsigned b, edge;
    int steer;

    if (!p->present) return;
    b = p->buttons;
    edge = b & ~prev;
    prev = b;

    steer = 0x80 + p->sx;
    if (b & M3_PAD_LEFT)  steer = 0x00;
    if (b & M3_PAD_RIGHT) steer = 0xFF;
    in->adc[0] = (uint8_t)(steer < 0 ? 0 : steer > 255 ? 255 : steer);
    in->adc[1] = (b & M3_PAD_A) ? 0xFF : p->rt;
    in->adc[2] = (b & M3_PAD_B) ? 0xFF : p->lt;

    if ((edge & M3_PAD_UP) && gear < 4) gear++;
    if ((edge & M3_PAD_DOWN) && gear > 1) gear--;
    in->in08 |= k_gear_bits[gear];
    if (edge & M3_PAD_X) view = (view + 1) & 3u;
    if (b & M3_PAD_X) in->in08 |= (uint8_t)(1u << view);

    if (b & M3_PAD_Y)     in->buttons |= M3_BTN_COIN1;
    if (b & M3_PAD_START) in->buttons |= M3_BTN_START1;
}

void scud_board_config(m3_config_t *cfg)
{
    cfg->step      = M3_STEP_1_5;
    cfg->title     = "Scud Race";
    cfg->scsi_base = SCUD_SCSI_BASE;
    cfg->scsi_irq  = 1;    /* its 0x500 handler services the 53C810 first */
    cfg->adc       = 1;    /* wheel and pedals */
    cfg->adc_rest[0] = 0x80;
    cfg->input_map = scud_input_map;
}

void scud_operator_settings(void)
{
    uint8_t *ram = bus_ram();
    if (ram && ram[SCUD_LINK_ID] == 1u)      /* MASTER, the blank-EEPROM default */
        ram[SCUD_LINK_ID] = SCUD_LINK_SINGLE;
}

void scud_register_lifted(void)
{
    scudrom_register_all();
    scudram_register_all();
}
