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
    lua_pushlstring(L, LUNA_LUA_MAGIC, sizeof(LUNA_LUA_MAGIC) - 1);
    lua_setglobal(L, "__LUNA_MAGIC_SRC");
    if (luaL_dostring(L,
                      "package.preload['luna.complete'] = assert(load(__LUNA_COMPLETE_SRC, '=(luna/complete)'))\n"
                      "package.preload['luna.introspect'] = assert(load(__LUNA_INTROSPECT_SRC, '=(luna/introspect)'))\n"
                      "package.preload['luna.highlight'] = assert(load(__LUNA_HIGHLIGHT_SRC, '=(luna/highlight)'))\n"
                      "package.preload['luna.magic'] = assert(load(__LUNA_MAGIC_SRC, '=(luna/magic)'))") != LUA_OK) {
        fail_msg("cannot preload modules: %s", lua_tostring(L, -1));
    }

    lua_pushlstring(L, LUNA_LUA_REPL, sizeof(LUNA_LUA_REPL) - 1);
    lua_setglobal(L, "__LUNA_REPL_SRC");
    const char *src = "local repl = assert(load(__LUNA_REPL_SRC, '=(luna/repl)'))()\n"
                      "S = repl.new()\n";
    assert_int_equal(luaL_dostring(L, src), LUA_OK);

    /* capture the base-globals snapshot the way luna_main does before
     * the entry runs (session S included, so %reset spares it), so
     * %reset keeps the runtime environment */
    if (luaL_dostring(L,
                      "local base = {}\n"
                      "for k in pairs(_G) do base[k] = true end\n"
                      "_G.__LUNA_BASE_GLOBALS = base") != LUA_OK) {
        fail_msg("cannot snapshot base globals: %s", lua_tostring(L, -1));
    }
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

/* evaluate code in the session VM and return the string it produced */
static const char *eval_in_vm(const char *code)
{
    static char buf[512];
    if (luaL_dostring(L, code) != LUA_OK) {
        fail_msg("vm eval failed: %s", lua_tostring(L, -1));
        return NULL;
    }
    if (lua_isboolean(L, -1)) {
        snprintf(buf, sizeof(buf), "%s", lua_toboolean(L, -1) ? "true" : "false");
    } else if (lua_isnil(L, -1)) {
        snprintf(buf, sizeof(buf), "(nil)");
    } else {
        const char *s = lua_tostring(L, -1);
        snprintf(buf, sizeof(buf), "%s", s ? s : "(other)");
    }
    lua_pop(L, 1);
    return buf;
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

/* Non-string error objects: the message handler first asks the object
 * for a __tostring, and only falls back to a canned sentence when it
 * gets nothing usable. */
static void test_error_object_with_tostring_is_the_message(void **state)
{
    (void)state;
    out_len_reset();
    assert_string_equal(
        feed("error(setmetatable({}, {__tostring = function() "
             "return 'object boom' end}))"),
        "error");
    assert_non_null(strstr(outbuf, "object boom"));
    assert_null(strstr(outbuf, "(error object is not a string)"));
}

static void test_error_object_without_tostring_is_summarized(void **state)
{
    (void)state;
    out_len_reset();
    assert_string_equal(feed("error({})"), "error");
    assert_non_null(strstr(outbuf, "(error object is not a string)"));
    assert_non_null(strstr(outbuf, "stack traceback"));
}

/* A line that ends on an operator continues — even when the user typed
 * trailing spaces after it (the incompleteness heuristic trims them
 * before looking at the tail). */
static void test_trailing_operator_with_spaces_continues(void **state)
{
    (void)state;
    out_len_reset();
    assert_string_equal(feed("1 +   "), "continue");
    assert_string_equal(feed("2"), "ok");
    assert_non_null(strstr(outbuf, "Out[1]: 3"));
}

/* A short string cannot span lines. Line one ends on an open quote
 * (the <eof> match keeps it open); when line two joins the pending
 * chunk the raw newline lands *inside* the quotes, and it is the
 * "unfinished string" classifier — not the <eof> match — that has to
 * catch it, or the block would be reported as a plain syntax error. */
static void test_short_string_wont_span_lines_stays_open(void **state)
{
    (void)state;
    out_len_reset();
    assert_string_equal(feed("s = \"abc"), "continue");
    assert_string_equal(feed("def\""), "continue");
    assert_int_equal(outlen, 0); /* nothing ran, nothing echoed */
}

/* Same for a trailing bare keyword: `not` alone leaves the expression
 * unfinished, so the next line is appended rather than reported —
 * `not false` evaluates as one expression across the two lines. */
static void test_trailing_keyword_continues(void **state)
{
    (void)state;
    out_len_reset();
    assert_string_equal(feed("not"), "continue");
    assert_string_equal(feed(" false"), "ok");
    assert_non_null(strstr(outbuf, "Out[1]: true"));
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

/* -- error hint group -------------------------------------------------- */

static void test_runtime_error_hints_did_you_mean(void **state)
{
    (void)state;
    out_len_reset();
    /* a typo'd global earns its nearest real name */
    assert_string_equal(feed("prnt('hi')"), "error");
    assert_non_null(strstr(outbuf, "(global 'prnt')"));
    assert_non_null(strstr(outbuf, "did you mean 'print'?"));
}

static void test_no_hint_when_nothing_is_close(void **state)
{
    (void)state;
    out_len_reset();
    assert_string_equal(feed("qqzzxx_wxyz('hi')"), "error");
    assert_null(strstr(outbuf, "did you mean"));
}

static void test_no_hint_when_the_global_exists(void **state)
{
    (void)state;
    out_len_reset();
    /* the name exists (just not callable): a suggestion would be noise */
    assert_string_equal(feed("t0 = 1; t0()"), "error");
    assert_null(strstr(outbuf, "did you mean"));
}

static void test_suggest_lists_two_tied_candidates(void **state)
{
    (void)state;
    assert_string_equal(feed("gamma = 1"), "ok");
    assert_string_equal(feed("gammy = 2"), "ok");
    out_len_reset();
    /* both real names are one edit away: list them together */
    assert_string_equal(feed("gammx(1)"), "error");
    assert_non_null(strstr(outbuf, "did you mean 'gamma' or 'gammy'?"));
}

/* -- In/Out register group -------------------------------------------- */

static void test_in_register_records_inputs(void **state)
{
    (void)state;
    out_len_reset();
    assert_string_equal(feed("alpha = 7"), "ok");
    assert_string_equal(feed("alpha * 2"), "ok");
    assert_string_equal(eval_in_vm("return In[1]"), "alpha = 7");
    assert_string_equal(eval_in_vm("return In[2]"), "alpha * 2");
    /* a magic line is an input too */
    assert_string_equal(feed("%whos"), "ok");
    assert_string_equal(eval_in_vm("return In[3]"), "%whos");
}

static void test_multiline_chunk_recorded_wholesale(void **state)
{
    (void)state;
    /* In[n] holds the whole accumulated chunk, not just the line that
     * happened to complete it */
    assert_string_equal(feed("function g(a)"), "continue");
    assert_string_equal(feed("  return a + 1"), "continue");
    assert_string_equal(feed("end"), "ok");
    assert_string_equal(eval_in_vm("return In[1]"),
                        "function g(a)\n  return a + 1\nend");
}

static void test_help_sugar_is_numbered_input(void **state)
{
    (void)state;
    out_len_reset();
    assert_string_equal(feed("?type"), "ok");
    assert_non_null(strstr(outbuf, "function"));
    assert_string_equal(eval_in_vm("return In[1]"), "?type");
}

static void test_reset_clears_underscore_registers(void **state)
{
    (void)state;
    assert_string_equal(feed("6*7"), "ok");
    assert_string_equal(eval_in_vm("return _ == 42 and __ == nil"), "true");
    assert_string_equal(feed("%reset"), "ok");
    /* _ / __ / Out are cleared; the In history survives (what %hist reads) */
    assert_string_equal(eval_in_vm("return _ == nil"), "true");
    assert_string_equal(eval_in_vm("return __ == nil"), "true");
    assert_string_equal(eval_in_vm("return next(Out) == nil"), "true");
    assert_string_equal(eval_in_vm("return In[1] == '6*7'"), "true");
    /* Out numbering starts over */
    out_len_reset();
    assert_string_equal(feed("8*8"), "ok");
    assert_non_null(strstr(outbuf, "Out[1]: 64"));
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
        cmocka_unit_test_setup_teardown(test_error_object_with_tostring_is_the_message, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_error_object_without_tostring_is_summarized, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_trailing_operator_with_spaces_continues, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_short_string_wont_span_lines_stays_open, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_trailing_keyword_continues, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_multiline_function_continues_then_runs, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_multiline_incomplete_string_waits, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_multiline_table_and_index, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_multiline_error_keeps_session, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_interrupt_aborts_running_chunk, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_interrupt_flag_cleared_after_abort, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_interrupt_hook_installed_only_during_exec, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_runtime_error_hints_did_you_mean, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_no_hint_when_nothing_is_close, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_no_hint_when_the_global_exists, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_suggest_lists_two_tied_candidates, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_in_register_records_inputs, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_multiline_chunk_recorded_wholesale, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_help_sugar_is_numbered_input, setup_session, teardown_session),
        cmocka_unit_test_setup_teardown(test_reset_clears_underscore_registers, setup_session, teardown_session),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
