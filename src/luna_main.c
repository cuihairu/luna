#include <signal.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "luna_kernel.h"
#include "luna_lua.h" /* generated: embedded Lua entry + repl sources */

static void luna_on_sigint(int sig)
{
    (void)sig;
    luna_kernel_request_interrupt();
}

/* arg table, same shape as the standalone interpreter:
 * arg[0] = program name, arg[1..n] = arguments. */
static void push_arg_table(lua_State *L, int argc, char **argv)
{
    lua_createtable(L, argc, 0);
    for (int i = 0; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i);
    }
    lua_setglobal(L, "arg");
}

static int run_chunk(lua_State *L, const char *src, const char *name)
{
    if (luaL_dostring(L, src) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "luna: %s: %s\n", name, msg ? msg : "(unknown error)");
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    signal(SIGINT, luna_on_sigint);

    lua_State *L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "luna: cannot create Lua state\n");
        return 1;
    }
    luaL_openlibs(L);

    luaL_requiref(L, "kernel", luaopen_luna_kernel, 1);
    lua_pop(L, 1);

    /* embedded sources for the entry chunk */
    lua_pushlstring(L, LUNA_LUA_REPL, sizeof(LUNA_LUA_REPL) - 1);
    lua_setglobal(L, "__LUNA_REPL_SRC");

    push_arg_table(L, argc, argv);

    int rc;
    if (run_chunk(L, LUNA_LUA_ENTRY, "=(luna)") != 0) {
        rc = 1;
    } else if (lua_isinteger(L, -1)) {
        rc = (int)lua_tointeger(L, -1);
    } else {
        rc = 0;
    }

    lua_close(L);
    return rc;
}
