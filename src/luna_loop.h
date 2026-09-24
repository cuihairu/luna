#ifndef LUNA_LOOP_H
#define LUNA_LOOP_H

#include "lua.h"

/* Registers the "loop" Lua library: an opt-in libuv event loop.
 *
 * Node naming on Lua semantics: setTimeout/setInterval/setImmediate
 * schedule callbacks, loop.run() drives the loop until nothing is
 * scheduled. While it runs, a prepare hook keeps luna's two contracts
 * alive: the attach socket is stepped (like the REPL idle gap), and a
 * pending ^C stops the loop and surfaces as an "interrupted" error —
 * the same exit-130 convention as a busy script. */
int luaopen_luna_loop(lua_State *L);

#endif
