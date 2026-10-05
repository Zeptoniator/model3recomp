/* guest_smoke: run lifted PowerPC and check what it did.
 *
 * make_guest.py writes a small 603e program, tools/ppc_lifter.py turns it
 * into C at build time, and this runs it against the real runtime (bus,
 * context, function table). The same source is a ctest on a desktop and a
 * CDI on a Dreamcast, so the SH-4 build is checked against the same answers.
 *
 * The milestone this stands for is DC-02: generated PPC -> C -> sh-elf-gcc
 * -> SH-4 -> the right register values. */
#include "model3recomp/model3recomp.h"
#include "smoke_funcs.h"

#include <stdio.h>
#include <string.h>

static int g_fails;

static void check(const char *what, uint32_t want, uint32_t got)
{
    printf("  %-26s expected %08lX  actual %08lX  %s\n", what,
           (unsigned long)want, (unsigned long)got, want == got ? "ok" : "FAIL");
    if (want != got)
        g_fails++;
}

int main(void)
{
    m3_roms_t roms;
    const uint8_t *ram;

    memset(&roms, 0, sizeof roms);
    printf("================================\n");
    printf(" MODEL3RECOMP GUEST SMOKE\n");
    printf("================================\n");

    bus_init(&roms);
    func_table_init();
    ppc_reset();
    smoke_register_all();

    if (!func_table_call(0x00010000u)) {
        printf("entry 0x00010000 was not lifted\nGUEST SMOKE FAIL\n");
        return 1;
    }
    ram = bus_ram();
    printf("PPC LIFT TEST\n");
    check("r3  (loop sum)", 55u, PPC_R(3));
    check("r7  (stw bytes 0,3)", 0x1278u, PPC_R(7));
    check("r9  (rlwinm)", 0x1234u, PPC_R(9));
    check("r11 (fadd/fmul/fctiwz)", 18u, PPC_R(11));
    check("r12 (bl subroutine)", 0x77u, PPC_R(12));
    check("RAM 0x2000 bytes",
          0x12345678u,
          ((uint32_t)ram[0x2000] << 24) | ((uint32_t)ram[0x2001] << 16) |
          ((uint32_t)ram[0x2002] << 8) | ram[0x2003]);
    printf(g_fails ? "GUEST SMOKE FAIL (%d)\n" : "GUEST SMOKE PASS\n", g_fails);
    bus_shutdown();
    return g_fails ? 1 : 0;
}
