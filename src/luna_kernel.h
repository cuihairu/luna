#ifndef LUNA_KERNEL_H
#define LUNA_KERNEL_H

#include "lua.h"

/* Registers the "kernel" Lua library: the thin C side of the REPL.
 *
 * Everything the REPL does that must touch the process (chunk loading,
 * execution with SIGINT abort hooks, output capture, monotonic time,
 * terminal probes) lives here; all REPL policy lives in Lua on top. */
int luaopen_luna_kernel(lua_State *L);

/* Signal-safe: sets the pending-interrupt flag checked by the execution
 * count hook. Call from a SIGINT handler (or directly from tests). */
void luna_kernel_request_interrupt(void);

#endif
