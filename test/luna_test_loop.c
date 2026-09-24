/* luna_test_loop.c — the opt-in event loop: timers, immediates, keep-
 * alive semantics, the prepare hook's serve poll, and ^C interruption.
 *
 * The uv loop is process-global, so every test leaves it empty: each
 * case clears what it scheduled, then runs the loop until the close
 * callbacks drain. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "luna_loop.h"
#include "luna_kernel.h"

static lua_State *L;

/* eval(code) -> one string from the stack */
static const char *eval_string(const char *code)
{
    static char buf[512];
    if (luaL_dostring(L, code) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        lua_pop(L, 1);
        snprintf(buf, sizeof(buf), "ERR: %s", msg ? msg : "?");
        return buf;
    }
    const char *s = luaL_tolstring(L, -1, NULL); /* booleans too */
    lua_pop(L, 2);
    snprintf(buf, sizeof(buf), "%s", s ? s : "(nil)");
    return buf;
}

static int setup_loop(void **state)
{
    (void)state;
    L = luaL_newstate();
    assert_non_null(L);
    luaL_openlibs(L); /* the cases use assert/table/tostring */
    /* glb=1 only for the test harness: real scripts reach the module
     * through require "loop", which glb=0 already serves */
    luaL_requiref(L, "loop", luaopen_luna_loop, 1);
    lua_pop(L, 1);
    /* a previous case must not leave anything ticking */
    assert_string_equal(eval_string("return loop.run()"), "true");
    return 0;
}

static int teardown_loop(void **state)
{
    (void)state;
    lua_close(L);
    L = NULL;
    return 0;
}

static void test_run_with_nothing_scheduled_returns(void **state)
{
    (void)state;
    assert_string_equal(eval_string("return loop.run()"), "true");
}

static void test_settimeout_fires_in_order_with_args(void **state)
{
    (void)state;
    /* swapped deadlines: the shorter fires first; trailing args reach
     * the callback as ... */
    assert_string_equal(eval_string(
        "t = {}\n"
        "loop.setTimeout(function(...) t[#t+1] = {...} end, 30, 'late')\n"
        "loop.setTimeout(function(...) t[#t+1] = {...} end, 5, 'early', 42)\n"
        "assert(loop.run())\n"
        "return t[1][1] .. t[1][2] .. ',' .. t[2][1]"), "early42,late");
}

static void test_interval_runs_until_cleared(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "n = 0\n"
        "h = loop.setInterval(function() n = n + 1 end, 5)\n"
        "loop.setTimeout(function() loop.clearInterval(h) end, 40)\n"
        "assert(loop.run())\n"
        "return (n >= 2 and n <= 8) and 'bounded' or n"), "bounded");
}

static void test_immediate_runs_within_the_run(void **state)
{
    (void)state;
    /* both fire in one pass; libuv's timers phase precedes check, so
     * only membership is asserted (Node makes no order promise either) */
    assert_string_equal(eval_string(
        "t = {}\n"
        "loop.setImmediate(function() t[#t+1] = 'imm' end)\n"
        "loop.setTimeout(function() t[#t+1] = 'time' end, 10)\n"
        "assert(loop.run())\n"
        "table.sort(t)\n"
        "return table.concat(t, ',')"), "imm,time");
}

static void test_clear_of_a_fired_oneshot_is_harmless(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "fired = false\n"
        "h = loop.setTimeout(function() fired = true end, 5)\n"
        "assert(loop.run())\n"
        "loop.clearTimeout(h)  -- already closed: no-op, must not crash\n"
        "return tostring(fired)"), "true");
}

static void test_stop_ends_the_run_handles_stay_scheduled(void **state)
{
    (void)state;
    /* stop() breaks out in the first iteration: the 5ms interval has
     * not fired yet and stays scheduled; the test clears and drains */
    assert_string_equal(eval_string(
        "n = 0\n"
        "h = loop.setInterval(function() n = n + 1 end, 5)\n"
        "loop.setImmediate(function() loop.stop() end)\n"
        "assert(loop.run())\n"
        "local after = n\n"
        "loop.clearInterval(h)\n"
        "assert(loop.run())  -- drain the closing interval\n"
        "return 'stopped at ' .. after"), "stopped at 0");
}

static void test_prepare_hook_steps_serve(void **state)
{
    (void)state;
    /* the loop's keep-alive hook gives the attach socket its poll */
    assert_string_equal(eval_string(
        "served = 0\n"
        "__LUNA_SERVE_STEP = function() served = served + 1 end\n"
        "loop.setTimeout(function() end, 30)\n"
        "assert(loop.run())\n"
        "__LUNA_SERVE_STEP = nil\n"
        "return served > 0 and 'polled' or 'silent'"), "polled");
}

static void test_interrupt_stops_the_run(void **state)
{
    (void)state;
    /* a ^C pending while uv_run blocks must surface as the same
     * "interrupted" error a busy script produces (exit 130 upstream) */
    luna_kernel_request_interrupt();
    assert_string_equal(eval_string(
        "h = loop.setTimeout(function() end, 10000)\n"
        "local ok, err = pcall(loop.run)\n"
        "loop.clearTimeout(h)\n"
        "assert(loop.run())  -- drain the cleared handle\n"
        "return tostring(ok) .. ',' .. tostring(tostring(err):find('interrupted') ~= nil)"),
        "false,true");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_run_with_nothing_scheduled_returns, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_settimeout_fires_in_order_with_args, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_interval_runs_until_cleared, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_immediate_runs_within_the_run, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_clear_of_a_fired_oneshot_is_harmless, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_stop_ends_the_run_handles_stay_scheduled, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_prepare_hook_steps_serve, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_interrupt_stops_the_run, setup_loop, teardown_loop),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
