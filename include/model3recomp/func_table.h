/* model3recomp -- guest address -> native function dispatch.
 *
 * Lifted code calls known targets directly. Everything the lifter could not
 * resolve statically -- bctr/blr through a jump table, a handler address
 * stored in RAM, a vector the game relocated -- lands here at runtime.
 */
#ifndef MODEL3RECOMP_FUNC_TABLE_H
#define MODEL3RECOMP_FUNC_TABLE_H

#include <stdint.h>
#include "model3recomp/ppc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*m3_func_t)(void);

void      func_table_init(void);
void      func_table_register(uint32_t guest_addr, m3_func_t fn);
m3_func_t func_table_lookup(uint32_t guest_addr);

/* Returns 0 and logs if the address is not registered -- that is a lifter
 * coverage hole, not a game bug, and it is the single most useful thing to
 * see when a port stops making progress. */
int       func_table_call(uint32_t guest_addr);

/* Where the last lifted `blr` was going, until a call site claims it. See
 * M3_RETURNED in lift.h. */
extern uint32_t m3_ret_to;

/* Every lifted translation unit emits one of these; the generated
 * func_table_gen.c calls them all. */
void      func_table_register_all(void);

#ifdef __cplusplus
}
#endif
#endif /* MODEL3RECOMP_FUNC_TABLE_H */
