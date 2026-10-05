/* model3recomp -- netplay for a host without sockets (the Dreamcast build).
 * Every call answers "not in a session", as netplay.c does when none was
 * asked for: netplay_exchange() returns 0 and input.c uses the local record. */
#include "model3recomp/netplay.h"

int  netplay_init_from_env(void) { return 1; }
void netplay_shutdown(void) { }
int  netplay_active(void) { return 0; }
int  netplay_role(void) { return 0; }
const char *netplay_status(void) { return ""; }

int netplay_exchange(const m3_input_t *local, m3_input_t *out, uint64_t f)
{
    (void)local; (void)out; (void)f;
    return 0;
}
