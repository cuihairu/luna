#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "luna_kernel.h"
#include "luna_lua.h" /* generated: embedded Lua entry + repl sources */

#ifndef LUNA_MODULES_DIR
#define LUNA_MODULES_DIR "./luna_modules"
#endif

#ifndef LUNA_SO_EXT
#define LUNA_SO_EXT "so"
#endif

/* Prepend the bundled module tree to package.path/cpath so require
 * finds argparse and friends before any user configuration. */
static void setup_module_paths(lua_State *L)
{
    char item[512];

    lua_getglobal(L, "package");
    if (!lua_istable(L, -1))
        return;

    const char *fields[] = { "path", "cpath", NULL };
    for (int i = 0; fields[i]; i++) {
        lua_getfield(L, -1, fields[i]);
        const char *cur = lua_tostring(L, -1);
        char extra[2048];
        snprintf(extra, sizeof(extra),
                 "%s/?.lua;%s/?/init.lua;",
                 LUNA_MODULES_DIR, LUNA_MODULES_DIR);
        if (strcmp(fields[i], "cpath") == 0)
            snprintf(item, sizeof(item), "%s/?.%s;", LUNA_MODULES_DIR, LUNA_SO_EXT);
        else
            item[0] = '\0';
        char merged[4096];
        snprintf(merged, sizeof(merged), "%s%s%s", extra, item, cur ? cur : "");
        lua_pushstring(L, merged);
        lua_setfield(L, -3, fields[i]);
        lua_pop(L, 1); /* old value */
    }
    lua_pop(L, 1); /* package */
}

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

static int dbg_msgh(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);
    fprintf(stderr, "ERR: %s\n", msg ? msg : "(?)");
    if (luaL_dostring(L,
            "local info = debug.getinfo(2, 'l')\n"
            "print('line:', info and info.currentline)\n"
            "for i = 1, 10 do\n"
            "  local n, v = debug.getlocal(2, i)\n"
            "  if not n then break end\n"
            "  print(string.format('local %d: %s = %s', i, n, tostring(v)))\n"
            "end\n") != LUA_OK) {
        fprintf(stderr, "dbg failed: %s\n", lua_tostring(L, -1));
    }
    return 1;
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

    setup_module_paths(L);

    /* embedded sources for the entry chunk */
    lua_pushlstring(L, LUNA_LUA_REPL, sizeof(LUNA_LUA_REPL) - 1);
    lua_setglobal(L, "__LUNA_REPL_SRC");
    lua_pushlstring(L, LUNA_LUA_COMPLETE, sizeof(LUNA_LUA_COMPLETE) - 1);
    lua_setglobal(L, "__LUNA_COMPLETE_SRC");
    lua_pushlstring(L, LUNA_LUA_INTROSPECT, sizeof(LUNA_LUA_INTROSPECT) - 1);
    lua_setglobal(L, "__LUNA_INTROSPECT_SRC");

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
