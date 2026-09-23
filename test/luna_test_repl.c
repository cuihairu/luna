#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include <cmocka.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "luna_kernel.h"
#include "luna_lua.h" /* generated: embedded repl + complete sources */

int luaopen_lpeg(lua_State *L); /* no lpeg.h in deps/lpeg; repl needs highlight */

/* Output captured from kernel.write() (via kernel.sink). */
static char outbuf[65536];
static size_t outlen;

static lua_State *L;

static int out_len_reset(void)
{
    outlen = 0;
    outbuf[0] = '\0';
    return 0;
}

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

/* -- harness --------------------------------------------------------- */

static int setup_session(void **state)
{
    (void)state;
    L = luaL_newstate();
    assert_non_null(L);
    luaL_openlibs(L);

    luaL_requiref(L, "kernel", luaopen_luna_kernel, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "lpeg", luaopen_lpeg, 0);
    lua_pop(L, 1);

    /* route output into outbuf */
    lua_getglobal(L, "kernel");
    lua_getfield(L, -1, "sink");
    lua_pushcfunction(L, sink_append);
    assert_int_equal(lua_pcall(L, 1, 0, 0), LUA_OK);
    lua_pop(L, 1);

    /* embed the policy modules the entry preloads, then create a session */
    lua_pushlstring(L, LUNA_LUA_COMPLETE, sizeof(LUNA_LUA_COMPLETE) - 1);
    lua_setglobal(L, "__LUNA_COMPLETE_SRC");
    lua_pushlstring(L, LUNA_LUA_INTROSPECT, sizeof(LUNA_LUA_INTROSPECT) - 1);
    lua_setglobal(L, "__LUNA_INTROSPECT_SRC");
    lua_pushlstring(L, LUNA_LUA_HIGHLIGHT, sizeof(LUNA_LUA_HIGHLIGHT) - 1);
    lua_setglobal(L, "__LUNA_HIGHLIGHT_SRC");
    if (luaL_dostring(L,
                      "package.preload['luna.complete'] = assert(load(__LUNA_COMPLETE_SRC, '=(luna/complete)'))\n"
                      "package.preload['luna.introspect'] = assert(load(__LUNA_INTROSPECT_SRC, '=(luna/introspect)'))\n"
                      "package.preload['luna.highlight'] = assert(load(__LUNA_HIGHLIGHT_SRC, '=(luna/highlight)'))") != LUA_OK) {
        fail_msg("cannot preload modules: %s", lua_tostring(L, -1));
    }

    lua_pushlstring(L, LUNA_LUA_REPL, sizeof(LUNA_LUA_REPL) - 1);
    lua_setglobal(L, "__LUNA_REPL_SRC");
    const char *src = "local repl = assert(load(__LUNA_REPL_SRC, '=(luna/repl)'))()\n"
                      "S = repl.new()\n";
    assert_int_equal(luaL_dostring(L, src), LUA_OK);
    return 0;
}

static int teardown_session(void **state)
{
    (void)state;
    lua_close(L);
    L = NULL;
    return 0;
}

/* call S:feed(line) -> status string */
static const char *feed(const char *line)
{
    lua_getglobal(L, "S");
    lua_getfield(L, -1, "feed");
    lua_insert(L, -2); /* session below function */
    lua_pushstring(L, line);
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        fail_msg("feed failed: %s", lua_tostring(L, -1));
        return NULL;
    }
    const char *status = lua_tostring(L, -1);
    lua_pop(L, 1); /* pcall already consumed func+args; only the result is left */
    return status;
}

/* -- eval group ------------------------------------------------------ */

static void test_eval_expression_echo(void **state)
{
    (void)state;
    out_len_reset();
    assert_string_equal(feed("1+2"), "ok");
    assert_non_null(strstr(outbuf, "Out[1]: 3"));
}

static void test_eval_statement_no_echo(void **state)
{
    (void)state;
    out_len_reset();
    assert_string_equal(feed("x = 40"), "ok");
    assert_int_equal(outlen, 0); /* assignments echo nothing */
    assert_string_equal(feed("x + 2"), "ok");
    assert_non_null(strstr(outbuf, "Out[1]: 42"));
}

static void test_eval_print_captured(void **state)
{
    (void)state;
    out_len_reset();
    assert_string_equal(feed("print('hello', 123)"), "ok");
    assert_non_null(strstr(outbuf, "hello\t123\n"));
    assert_null(strstr(outbuf, "Out[")); /* print returns nothing */
}

static void test_eval_out_register(void **state)
{
    (void)state;
    out_len_reset();
    assert_string_equal(feed("'abc' .. 'd'"), "ok");
    assert_string_equal(feed("Out[1] .. '!'"), "ok");
    assert_non_null(strstr(outbuf, "Out[2]: 'abcd!'")); /* quoted by repr */
}

static void test_eval_syntax_error_reported(void **state)
{
    (void)state;
    out_len_reset();
    assert_string_equal(feed("1 +"), "continue"); /* still incomplete */
    assert_string_equal(feed("2 end"), "error");  /* now a real error */
    assert_non_null(strstr(outbuf, "[string \"repl\"]"));
    /* session survives */
    out_len_reset();
    assert_string_equal(feed("7*6"), "ok");
    assert_non_null(strstr(outbuf, "Out[1]: 42"));
}

static void test_eval_runtime_error_reported(void **state)
{
    (void)state;
    out_len_reset();
    assert_string_equal(feed("error('boom')"), "error");
    assert_non_null(strstr(outbuf, "boom"));
    /* traceback present */
    assert_non_null(strstr(outbuf, "stack traceback"));
}

/* -- multiline group ------------------------------------------------- */

static void test_multiline_function_continues_then_runs(void **state)
{
    (void)state;
    out_len_reset();
    assert_string_equal(feed("function f(a)"), "continue");
    assert_string_equal(feed("  return a * 2"), "continue");
    assert_string_equal(feed("end"), "ok"); /* no echo: it's a statement */
    assert_string_equal(feed("f(21)"), "ok");
    assert_non_null(strstr(outbuf, "Out[1]: 42"));
}

static void test_multiline_incomplete_string_waits(void **state)
{
    (void)state;
    /* long strings can span lines; short ones cannot */
    assert_string_equal(feed("s = [[abc"), "continue");
    assert_string_equal(feed("def]]"), "ok");
    out_len_reset();
    assert_string_equal(feed("#s"), "ok");
    assert_non_null(strstr(outbuf, "Out[1]: 7"));
}

static void test_multiline_table_and_index(void **state)
{
    (void)state;
    assert_string_equal(feed("t = {"), "continue");
    assert_string_equal(feed("  a = 10,"), "continue");
    assert_string_equal(feed("}"), "ok");
    out_len_reset();
    assert_string_equal(feed("t.a * 3"), "ok");
    assert_non_null(strstr(outbuf, "Out[1]: 30"));
}

static void test_multiline_error_keeps_session(void **state)
{
    (void)state;
    assert_string_equal(feed("if true then"), "continue");
    assert_string_equal(feed("  syntax ())"), "error"); /* closes input with a bad line */
    out_len_reset();
    assert_string_equal(feed("2^10"), "ok");
    assert_non_null(strstr(outbuf, "Out[1]: 1024"));
}

/* -- interrupt group -------------------------------------------------- */

static void test_interrupt_aborts_running_chunk(void **state)
{
    (void)state;
    out_len_reset();
    /* request SIGINT right before execution; the count hook fires
     * almost immediately inside the infinite loop */
    luna_kernel_request_interrupt();
    assert_string_equal(feed("while true do end"), "error");
    assert_non_null(strstr(outbuf, "interrupted"));

    /* the session and the state are still usable afterwards */
    out_len_reset();
    assert_string_equal(feed("1+1"), "ok");
    assert_non_null(strstr(outbuf, "Out[1]: 2"));
}

static void test_interrupt_flag_cleared_after_abort(void **state)
{
    (void)state;
    luna_kernel_request_interrupt();
    out_len_reset();
    assert_string_equal(feed("while true do end"), "error");
    /* a fresh chunk must not see a stale flag */
    out_len_reset();
    assert_string_equal(feed("t = 0"), "ok");
    assert_string_equal(feed("t + 1"), "ok");
    assert_non_null(strstr(outbuf, "Out[1]: 1"));
}

static void test_interrupt_hook_installed_only_during_exec(void **state)
{
    (void)state;
    /* interrupt requested while no chunk is running must not leak into
     * the next chunk: feed() itself runs fine because exec() resets the
     * flag only after a run — but check() never aborts. */
    luna_kernel_request_interrupt();
    out_len_reset();
    assert_string_equal(feed("('x'):rep(3)"), "ok");
    assert_non_null(strstr(outbuf, "Out[1]: 'xxx'"));
}

/* -- runner ----------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_eval_expression_echo, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_eval_statement_no_echo, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_eval_print_captured, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_eval_out_register, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_eval_syntax_error_reported, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_eval_runtime_error_reported, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_multiline_function_continues_then_runs, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_multiline_incomplete_string_waits, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_multiline_table_and_index, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_multiline_error_keeps_session, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_interrupt_aborts_running_chunk, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_interrupt_flag_cleared_after_abort, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_interrupt_hook_installed_only_during_exec, setup_session, teardown_session),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
