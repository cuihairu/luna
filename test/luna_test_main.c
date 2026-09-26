/* luna_test_main.c — the launcher's last-resort path (src/luna_main.c).
 *
 * Every CLI mode protects its own work (a bad script, a bad -e, a
 * failing plugin), so the one caller of run_chunk's error branch — the
 * embedded entry chunk itself failing — cannot be reached from a
 * command line: by then everything else is already proven working. The
 * branch is therefore driven white-box, like the linedit tests, with
 * the translation unit compiled in and main() renamed out of the way.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* main() is the launch itself; this harness only needs the helpers. */
#define main luna_entry_main_unused
#include "luna_main.c"
#undef main

/* The reporter: name the entry chunk, repeat the message, answer 1.
 * The message stays on the stack for its caller (main discards it with
 * the state). */
static void test_entry_failure_is_named_and_returns_one(void **state)
{
    (void)state;
    lua_State *L = luaL_newstate();
    assert_non_null(L);
    luaL_openlibs(L); /* the entry runs in a full state, so does this */

    assert_int_equal(run_chunk(L, "error('entry boom')", "=(luna)"), 1);
    assert_non_null(strstr(lua_tostring(L, -1), "entry boom"));
    lua_settop(L, 0);

    /* and the success it falls back to when the chunk does run */
    assert_int_equal(run_chunk(L, "return 40 + 2", "=(luna)"), 0);
    assert_int_equal((int)lua_tointeger(L, -1), 42);
    lua_close(L);
}

/* setup_module_paths walks package.path/cpath: a state without a
 * package table has to fall out through the guard at the top rather
 * than read past the missing table. */
static void test_module_paths_tolerates_a_missing_package(void **state)
{
    (void)state;
    lua_State *L = luaL_newstate();
    assert_non_null(L);
    lua_pushnil(L);
    lua_setglobal(L, "package");
    setup_module_paths(L);
    /* the lookup it could not use is still what is on top */
    assert_true(lua_isnil(L, -1));
    lua_close(L);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_entry_failure_is_named_and_returns_one),
        cmocka_unit_test(test_module_paths_tolerates_a_missing_package),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
