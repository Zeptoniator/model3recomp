/* dreamcast_boot: the board runtime on a Dreamcast, with no game.
 *
 * Milestone DC-01: KallistiOS boots, model3recomp_init() brings the board
 * up through the Dreamcast platform layer, and the field loop runs a fixed
 * number of fields without the guest -- the loop model3recomp_run() falls
 * back to when a guest returns. Everything is reported on the serial
 * console, so it can be read without a picture.
 */
#include "model3recomp/model3recomp.h"

#include <kos.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#ifndef M3_GIT_REV
#define M3_GIT_REV "unknown"
#endif

#define FIELDS 120

static void heap(const char *when)
{
    struct mallinfo mi = mallinfo();
    printf("  heap %-14s: %7lu KB in use, %7lu KB arena\n", when,
           (unsigned long)mi.uordblks >> 10, (unsigned long)mi.arena >> 10);
}

int main(void)
{
    m3_config_t cfg;
    uint64_t t0, t1;
    int i;

    printf("================================\n");
    printf(" MODEL3RECOMP DREAMCAST\n");
    printf("================================\n");
    printf("build           : %s %s, model3recomp %s\n", __DATE__, __TIME__,
           M3_GIT_REV);
    printf("KallistiOS      : OK\n");
    printf("SH-4 backend    : OK (sizeof double %u, endianness %s)\n",
           (unsigned)sizeof(double),
           (*(const uint16_t *)"\x01\x00" == 1) ? "little" : "big");
    heap("before init");

    memset(&cfg, 0, sizeof cfg);
    cfg.step  = M3_STEP_1_5;
    cfg.title = "dreamcast_boot";
    if (!model3recomp_init(&cfg)) {
        printf("Platform layer  : FAILED\n");
        return 1;
    }
    printf("Platform layer  : OK\n");
    printf("PVR             : not used yet (no renderer/pvr)\n");
    heap("after init");

    t0 = timer_us_gettime64();
    for (i = 0; i < FIELDS; i++)
        model3recomp_end_frame();
    t1 = timer_us_gettime64();
    printf("Runtime         : READY (%llu fields in %llu us)\n",
           (unsigned long long)model3recomp_frame_count(),
           (unsigned long long)(t1 - t0));
    heap("after loop");
    printf("================================\n");
    printf("DC-01 DONE\n");
    return 0;
}
