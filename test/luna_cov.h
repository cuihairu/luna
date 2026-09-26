/* luna_cov.h — luacov wiring for the in-process test harnesses.
 *
 * Coverage builds (see CMakeLists.txt / test/CMakeLists.txt) compile in
 * the LUNA_SOURCE_ROOT / LUNA_LUACOV_SRC / LUNA_COV_CONFIG paths and
 * export LUNA_COVERAGE=1 to the tests; every harness then calls
 * luna_cov_setup(L) right after creating its state — before any luna.*
 * source is loaded, so chunknames and the line hook are in place — and
 * luna_cov_teardown(L) before lua_close, which merges the state's stats
 * into the shared build-tree stats file. Plain builds: everything here
 * is a no-op apart from defining the chunkname helper, so error
 * messages keep their '=(luna/x)' names.
 */
#ifndef luna_cov_h
#define luna_cov_h

#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"

#ifndef LUNA_SOURCE_ROOT
#define LUNA_SOURCE_ROOT "nil"
#endif
#ifndef LUNA_LUACOV_SRC
#define LUNA_LUACOV_SRC "nil"
#endif
#ifndef LUNA_COV_CONFIG
#define LUNA_COV_CONFIG ""
#endif

static void luna_cov_setup(lua_State *L)
{
    /* chunkname helper: '@'-form (traced by luacov) when coverage is
     * on, the historical '=(luna/x)' marker otherwise */
    if (luaL_dostring(L,
            "function luna_chunkname(name)\n"
            "  local root = __LUNA_COV_ROOT\n"
            "  if root then return '@' .. root .. '/lua/luna/' .. name .. '.lua' end\n"
            "  return '=(luna/' .. name .. ')'\n"
            "end\n") != LUA_OK) {
        lua_pop(L, 1);
    }
    if (!getenv("LUNA_COVERAGE"))
        return;
    /* the paths go in as real C strings — the LUNA_* macros are C string
     * literals, and splicing them into Lua source text would lose the
     * quotes at concatenation (a bare path is not Lua code) */
    lua_pushstring(L, LUNA_SOURCE_ROOT);
    lua_setglobal(L, "__LUNA_COV_ROOT");
    lua_pushstring(L, LUNA_COV_CONFIG);
    lua_setglobal(L, "__LUNA_COV_CONFIG");
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_pushliteral(L, LUNA_LUACOV_SRC "/?.lua;");
        lua_getfield(L, -2, "path");
        lua_concat(L, 2);
        lua_setfield(L, -2, "path");
    }
    lua_pop(L, 1);
    if (luaL_dostring(L,
            "local ok, runner = pcall(require, 'luacov.runner')\n"
            "if not ok then error('no luacov: ' .. tostring(runner)) end\n"
            "runner.init(dofile(__LUNA_COV_CONFIG))\n"
            "local covhook = runner.debug_hook\n"
            "debug.sethook(function(ev, line)\n"
            "  if ev == 'line' then covhook(nil, line, 3) end -- level 3: skip hook and wrapper\n"
            "  if ev == 'count' then\n"
            "    local k = package.loaded.kernel\n"
            "    if k then k.count_hook() end\n"
            "  end\n"
            "end, 'l', 100000)\n") != LUA_OK) {
        lua_pop(L, 1); /* report below, but never fail the test for it */
        if (luaL_dostring(L, "io.stderr:write('luna_cov: instrumentation unavailable\\n')") != LUA_OK)
            lua_pop(L, 1);
    }
}

static void luna_cov_teardown(lua_State *L)
{
    if (!getenv("LUNA_COVERAGE"))
        return;
    /* merges this state's stats into the shared stats file */
    if (luaL_dostring(L, "require('luacov.runner').shutdown()") != LUA_OK)
        lua_pop(L, 1);
}

#endif
