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
#include "luna_cov.h"
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
    luna_cov_setup(L);
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
    lua_pushlstring(L, LUNA_LUA_PLUGINS, sizeof(LUNA_LUA_PLUGINS) - 1);
    lua_setglobal(L, "__LUNA_PLUGINS_SRC");
    assert_int_equal(luaL_dostring(L,
                       "package.preload['luna.magic'] = assert(load(__LUNA_MAGIC_SRC, luna_chunkname('magic')))\n"
                       "package.preload['luna.complete'] = assert(load(__LUNA_COMPLETE_SRC, luna_chunkname('complete')))\n"
                       "package.preload['luna.introspect'] = assert(load(__LUNA_INTROSPECT_SRC, luna_chunkname('introspect')))\n"
                       "package.preload['luna.highlight'] = assert(load(__LUNA_HIGHLIGHT_SRC, luna_chunkname('highlight')))\n"
                       "package.preload['luna.plugins'] = assert(load(__LUNA_PLUGINS_SRC, luna_chunkname('plugins')))\n"
                       "M = require 'luna.magic'\n"
                       "local repl = assert(load(__LUNA_REPL_SRC, luna_chunkname('repl')))()\n"
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
    luna_cov_teardown(L);
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

static void test_time_runs_statement_code(void **state)
{
    (void)state;
    /* statements cannot compile as `return <code>`: they still run */
    assert_string_equal(feed("%time for i = 1, 100 do end"), "ok");
    assert_non_null(strstr(outbuf, "Wall time:"));
    assert_null(strstr(outbuf, "Out[")); /* statements echo no result */
    /* an error inside statement code still surfaces */
    assert_string_equal(feed("%time if true then error('kaboom-statement') end"), "ok");
    assert_non_null(strstr(outbuf, "kaboom-statement"));
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

static void test_hist_range_selection(void **state)
{
    (void)state;
    feed("a1 = 1");
    feed("a2 = 2");
    feed("a3 = 3");
    assert_string_equal(feed("%hist 2-3"), "ok");
    assert_null(strstr(outbuf, "In [1]: a1 = 1"));
    assert_non_null(strstr(outbuf, "In [2]: a2 = 2"));
    assert_non_null(strstr(outbuf, "In [3]: a3 = 3"));
    /* a single number selects one entry */
    assert_string_equal(feed("%hist 1"), "ok");
    assert_non_null(strstr(outbuf, "In [1]: a1 = 1"));
    assert_null(strstr(outbuf, "In [2]"));
    /* a bad range spec is a usage error, not a crash */
    assert_string_equal(feed("%hist banana"), "ok");
    assert_non_null(strstr(outbuf, "usage"));
}

static void test_plugins_lists_overridden(void **state)
{
    (void)state;
    /* no plugin directories in this VM: nothing loaded, nothing failed */
    assert_string_equal(feed("%plugins"), "ok");
    assert_non_null(strstr(outbuf, "no plugins loaded"));
    /* a shadowed discovery is reported with its directory */
    if (luaL_dostring(L,
                      "require('luna.plugins').overridden['dup'] = '/tmp/dup-copy'") != LUA_OK) {
        fail_msg("cannot seed overridden: %s", lua_tostring(L, -1));
    }
    assert_string_equal(feed("%plugins"), "ok");
    assert_non_null(strstr(outbuf, "overridden"));
    assert_non_null(strstr(outbuf, "dup"));
    assert_non_null(strstr(outbuf, "/tmp/dup-copy"));
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

/* -- coverage gap group --------------------------------------------------- */

static void test_echo_result_early_return_when_no_values(void **state)
{
    (void)state;
    /* echo_result returns early when the result carries no non-nil
     * values; %time of a call returning no values still prints its
     * timing, but no Out line is echoed. */
    outlen = 0;
    outbuf[0] = '\0';
    assert_string_equal(feed("%time (function() end)()"), "ok");
    assert_non_null(strstr(outbuf, "Wall time:"));
    assert_null(strstr(outbuf, "Out[")); /* echo_result saw no values, returned early */
}

static void test_exit_magic_calls_os_exit(void **state)
{
    (void)state;
    /* The %exit magic calls os.exit(0). In the test harness we cannot
     * actually exit, so we mock os.exit to capture the call. */
    outlen = 0;
    outbuf[0] = '\0';
    assert_int_equal(luaL_dostring(L,
                       "local orig_exit = os.exit\n"
                       "local captured = nil\n"
                       "os.exit = function(code) captured = code end\n"
                       "M.commands.exit.run(S, '')\n"
                       "os.exit = orig_exit\n"
                       "return captured") , LUA_OK);
    assert_int_equal(lua_tointeger(L, -1), 0);
    lua_pop(L, 1);
}

static void test_timeit_usage_error(void **state)
{
    (void)state;
    /* %timeit with empty arg hits the usage error branch */
    assert_string_equal(feed("%timeit"), "ok");
    assert_non_null(strstr(outbuf, "usage: %timeit <expression>"));
}

static void test_time_execution_error(void **state)
{
    (void)state;
    /* %time with code that errors — the error path in the time magic
     * calls error(tostring(res[2])) which is a distinct branch. */
    assert_string_equal(feed("%time error('boom-time')"), "ok");
    assert_non_null(strstr(outbuf, "boom-time"));
}

static void test_timeit_execution_error(void **state)
{
    (void)state;
    /* %timeit with code that errors — same error branch in timeit */
    assert_string_equal(feed("%timeit error('boom-timeit')"), "ok");
    assert_non_null(strstr(outbuf, "boom-timeit"));
}

static void test_clear_on_tty(void **state)
{
    (void)state;
    /* %clear writes the ANSI clear sequence when kernel.tty() is true.
     * The test harness runs without a TTY, so we force the branch by
     * temporarily patching kernel.tty in the session. */
    outlen = 0;
    outbuf[0] = '\0';
    assert_int_equal(luaL_dostring(L,
                       "local k = require 'kernel'\n"
                       "local orig = k.tty\n"
                       "k.tty = function() return true end\n"
                       "M.commands.clear.run(S, '')\n"
                       "k.tty = orig") , LUA_OK);
    assert_non_null(strstr(outbuf, "\33[2J\33[H"));
}

static void test_plugins_loader_unavailable(void **state)
{
    (void)state;
    /* %plugins when require('luna.plugins') fails — simulate by breaking
     * the preload and calling the magic directly. */
    assert_int_equal(luaL_dostring(L,
                       "package.preload['luna.plugins'] = nil\n"
                       "package.loaded['luna.plugins'] = nil\n"
                       "M.commands.plugins.run(S, '')") , LUA_OK);
    assert_non_null(strstr(outbuf, "plugins: loader unavailable"));
}

static void test_plugins_with_loaded_and_failed(void **state)
{
    (void)state;
    /* Seed the plugins module with loaded and failed entries to exercise
     * the listing loops and the failed counter. */
    assert_int_equal(luaL_dostring(L,
                       "local plugs = require 'luna.plugins'\n"
                       "plugs.loaded['demo'] = { version = '1.0', description = 'demo plugin' }\n"
                       "plugs.failed['badone'] = 'load error details'\n"
                       "return true") , LUA_OK);
    assert_string_equal(feed("%plugins"), "ok");
    assert_non_null(strstr(outbuf, "loaded plugins:"));
    assert_non_null(strstr(outbuf, "demo"));
    assert_non_null(strstr(outbuf, "1.0"));
    assert_non_null(strstr(outbuf, "demo plugin"));
    assert_non_null(strstr(outbuf, "1 plugin(s) failed to load"));
}

static void test_plugins_with_shadowed(void **state)
{
    (void)state;
    /* Seed overridden entries to exercise the shadowed listing and its
     * alphabetical sort (a.name < b.name). */
    assert_int_equal(luaL_dostring(L,
                       "local plugs = require 'luna.plugins'\n"
                       "plugs.overridden = { zulu = '/tmp/z', alpha = '/tmp/a' }\n"
                       "return true") , LUA_OK);
    assert_string_equal(feed("%plugins"), "ok");
    assert_non_null(strstr(outbuf, "overridden by an earlier same-name plugin:"));
    /* alpha should appear before zulu due to sort */
    const char *local_a = strstr(outbuf, "alpha");
    const char *local_z = strstr(outbuf, "zulu");
    assert_true(local_a < local_z);
}

/* -- runner --------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_time_reports_wall_time_and_value, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_time_usage_error, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_timeit_runs_repeats, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_time_runs_statement_code, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_hist_prints_session_inputs, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_hist_range_selection, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_plugins_lists_overridden, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_whos_magic_lists_globals, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_reset_clears_user_globals, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_unknown_magic_reports, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_magic_help_lists_commands, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_register_extends_and_dispatch, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_register_validates_arguments, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_bare_percent_still_evaluates, setup_magic, teardown_magic),
        /* coverage gap tests */
        cmocka_unit_test_setup_teardown(test_echo_result_early_return_when_no_values, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_timeit_usage_error, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_time_execution_error, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_timeit_execution_error, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_clear_on_tty, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_plugins_loader_unavailable, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_plugins_with_loaded_and_failed, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_plugins_with_shadowed, setup_magic, teardown_magic),
        cmocka_unit_test_setup_teardown(test_exit_magic_calls_os_exit, setup_magic, teardown_magic),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
