/* luna_line.h — replxx bridge exposed to Lua as require "linedit". */
#ifndef LUNA_LINE_H
#define LUNA_LINE_H

#include "lua.h"

int luaopen_luna_line(lua_State *L);

/* Async-signal-safe: nudges a blocked replxx_input() so it returns an
 * empty line (the REPL loop's cue to poll the attach socket). Safe to
 * call from a signal handler even when no editor was ever created. */
void luna_line_notify_wake(void);

#endif
