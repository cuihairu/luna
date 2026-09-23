/* Magic commands: % interception in the session feed, the builtin
 * commands, and the plugin-facing register/dispatch extension point.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include <cmocka.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "luna_kernel.h"
#include "luna_lua.h"

int luaopen_lpeg(lua_State *L); /* highlight -> lexer -> lpeg */

static lua_State *L;
static char outbuf[65536];
static size_t outlen;

static int sink_append(lua_State *l)
{
    size_t n;
    const char *s = luaL_checklstring(l, 1, &n);
    assert_true(outlen + n < sizeof(outbuf));
    memcpy(outbuf + outlen, s, n);
    outlen += n;
    outbuf[outlen] = '\0';
    return 0;
}

static int setup_magic(void **state)
{
    (void)state;
    L = luaL_newstate();
    assert_non_null(L);
    luaL_openlibs(L);
    luaL_requiref(L, "kernel", luaopen_luna_kernel, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "lpeg", luaopen_lpeg, 0);
    lua_pop(L, 1);

    lua_getglobal(L, "kernel");
    lua_getfield(L, -1, "sink");
    lua_pushcfunction(L, sink_append);
    assert_int_equal(lua_pcall(L, 1, 0, 0), LUA_OK);
    lua_pop(L, 1);

    lua_pushlstring(L, LUNA_LUA_MAGIC, sizeof(LUNA_LUA_MAGIC) - 1);
    lua_setglobal(L, "__LUNA_MAGIC_SRC");
    lua_pushlstring(L, LUNA_LUA_COMPLETE, sizeof(LUNA_LUA_COMPLETE) - 1);
    lua_setglobal(L, "__LUNA_COMPLETE_SRC");
    lua_pushlstring(L, LUNA_LUA_INTROSPECT, sizeof(LUNA_LUA_INTROSPECT) - 1);
    lua_setglobal(L, "__LUNA_INTROSPECT_SRC");
    lua_pushlstring(L, LUNA_LUA_HIGHLIGHT, sizeof(LUNA_LUA_HIGHLIGHT) - 1);
    lua_setglobal(L, "__LUNA_HIGHLIGHT_SRC");
    lua_pushlstring(L, LUNA_LUA_REPL, sizeof(LUNA_LUA_REPL) - 1);
    lua_setglobal(L, "__LUNA_REPL_SRC");
    assert_int_equal(luaL_dostring(L,
                       "package.preload['luna.magic'] = assert(load(__LUNA_MAGIC_SRC, '=(luna/magic)'))\n"
                       "package.preload['luna.complete'] = assert(load(__LUNA_COMPLETE_SRC, '=(luna/complete)'))\n"
                       "package.preload['luna.introspect'] = assert(load(__LUNA_INTROSPECT_SRC, '=(luna/introspect)'))\n"
                       "package.preload['luna.highlight'] = assert(load(__LUNA_HIGHLIGHT_SRC, '=(luna/highlight)'))\n"
                       "M = require 'luna.magic'\n"
                       "local repl = assert(load(__LUNA_REPL_SRC, '=(luna/repl)'))()\n"
                       "S = repl.new()\n"
                       "local base = {}\n"
                       "for k in pairs(_G) do base[k] = true end\n"
                       "_G.__LUNA_BASE_GLOBALS = base"),
                     LUA_OK);
    return 0;
}

static int teardown_magic(void **state)
{
    (void)state;
    lua_close(L);
    L = NULL;
    return 0;
}

static const char *feed(const char *line)
{
    outlen = 0;
    outbuf[0] = '\0';
    lua_getglobal(L, "S");
    lua_getfield(L, -1, "feed");
    lua_insert(L, -2); /* keep the session: feed(S, line) */
    lua_pushstring(L, line);
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        fail_msg("feed failed: %s", lua_tostring(L, -1));
    }
    const char *status = lua_tostring(L, -1);
    lua_pop(L, 1);
    return status;
}

/* -- builtin magic group ------------------------------------------------- */

static void test_time_reports_wall_time_and_value(void **state)
{
    (void)state;
    assert_string_equal(feed("%time 6*7"), "ok");
    assert_non_null(strstr(outbuf, "Out[1]: 42"));
    assert_non_null(strstr(outbuf, "Wall time:"));
    assert_non_null(strstr(outbuf, "ms"));
}

static void test_time_usage_error(void **state)
{
    (void)state;
    assert_string_equal(feed("%time"), "ok"); /* magic errors are reported, not raised */
    assert_non_null(strstr(outbuf, "usage"));
}

static void test_timeit_runs_repeats(void **state)
{
    (void)state;
    assert_string_equal(feed("%timeit 1"), "ok");
    assert_non_null(strstr(outbuf, "loops"));
    assert_non_null(strstr(outbuf, "per loop"));
}

static void test_hist_prints_session_inputs(void **state)
{
    (void)state;
    assert_string_equal(feed("alpha = 1"), "ok");
    assert_string_equal(feed("%time 2+2"), "ok");
    assert_string_equal(feed("%hist"), "ok");
    assert_non_null(strstr(outbuf, "In [1]: alpha = 1"));
    assert_non_null(strstr(outbuf, "In [2]: %time 2+2"));
}

static void test_whos_magic_lists_globals(void **state)
{
    (void)state;
    feed("answer = 42");
    assert_string_equal(feed("%whos"), "ok");
    assert_non_null(strstr(outbuf, "answer"));
}

static void test_reset_clears_user_globals(void **state)
{
    (void)state;
    feed("doomed = 99");
    assert_string_equal(feed("%reset"), "ok");
    assert_non_null(strstr(outbuf, "session state reset"));
    /* the global is gone (nil results do not echo); kernel survives */
    assert_string_equal(feed("doomed == nil"), "ok");
    assert_non_null(strstr(outbuf, "Out[1]: true"));
    assert_string_equal(feed("6*7"), "ok");
    assert_non_null(strstr(outbuf, "Out[2]: 42"));
}

static void test_unknown_magic_reports(void **state)
{
    (void)state;
    assert_string_equal(feed("%definitely_not_a_magic"), "ok");
    assert_non_null(strstr(outbuf, "unknown magic"));
    assert_non_null(strstr(outbuf, "%help"));
}

static void test_magic_help_lists_commands(void **state)
{
    (void)state;
    assert_string_equal(feed("%help"), "ok");
    assert_non_null(strstr(outbuf, "%time"));
    assert_non_null(strstr(outbuf, "%hist"));
}

/* -- extension point group ------------------------------------------------ */

static void test_register_extends_and_dispatch(void **state)
{
    (void)state;
    assert_int_equal(luaL_dostring(L,
                       "M.register('greet', function(s, arg) kernel.write('hello ' .. arg .. '!\\n') end, 'greet someone')\n"
                       "return true") != LUA_OK ? 1 : LUA_OK, LUA_OK);
    assert_string_equal(feed("%greet world"), "ok");
    assert_non_null(strstr(outbuf, "hello world!"));
    /* %help now lists the plugin command */
    feed("%help");
    assert_non_null(strstr(outbuf, "greet someone"));
}

static void test_register_validates_arguments(void **state)
{
    (void)state;
    if (luaL_dostring(L,
                      "local ok, err = pcall(M.register, 'bad', 'not-a-function')\n"
                      "return ok == false") != LUA_OK) {
        fail_msg("register validation failed: %s", lua_tostring(L, -1));
    }
    assert_true(lua_toboolean(L, -1));
    lua_pop(L, 1);
}

static void test_bare_percent_still_evaluates(void **state)
{
    (void)state;
    /* a lone % is not a magic invocation: goes to normal eval and
     * reports a syntax error */
    assert_string_equal(feed("% 6"), "error");
}

/* -- runner --------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_time_reports_wall_time_and_value, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_time_usage_error, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_timeit_runs_repeats, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_hist_prints_session_inputs, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_whos_magic_lists_globals, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_reset_clears_user_globals, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_unknown_magic_reports, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_magic_help_lists_commands, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_register_extends_and_dispatch, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_register_validates_arguments, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_bare_percent_still_evaluates, setup_magic, teardown_magic),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
