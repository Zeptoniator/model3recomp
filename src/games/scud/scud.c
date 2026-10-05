/* Scud Race -- title data shared by every host. */
#include "scud.h"

#include "scudrom_funcs.h"
#include "scudram_funcs.h"

#include "model3recomp/bus.h"

#include <string.h>

void scud_board_config(m3_config_t *cfg)
{
    cfg->step      = M3_STEP_1_5;
    cfg->title     = "Scud Race";
    cfg->scsi_base = SCUD_SCSI_BASE;
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
