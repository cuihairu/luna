/* Introspection: repr (quoted strings, limited tables, cycles,
 * signatures), help/whos text, and the "?expr" / "expr?" help sugar in
 * the session feed.
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
#include "luna_lua.h" /* generated: embedded repl + introspect sources */

int luaopen_lpeg(lua_State *L); /* repl requires luna.highlight -> lpeg */

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

static void preload_modules(void)
{
    luaL_requiref(L, "lpeg", luaopen_lpeg, 0);
    lua_pop(L, 1);
    lua_pushlstring(L, LUNA_LUA_COMPLETE, sizeof(LUNA_LUA_COMPLETE) - 1);
    lua_setglobal(L, "__LUNA_COMPLETE_SRC");
    lua_pushlstring(L, LUNA_LUA_INTROSPECT, sizeof(LUNA_LUA_INTROSPECT) - 1);
    lua_setglobal(L, "__LUNA_INTROSPECT_SRC");
    lua_pushlstring(L, LUNA_LUA_REPL, sizeof(LUNA_LUA_REPL) - 1);
    lua_setglobal(L, "__LUNA_REPL_SRC");
    lua_pushlstring(L, LUNA_LUA_HIGHLIGHT, sizeof(LUNA_LUA_HIGHLIGHT) - 1);
    lua_setglobal(L, "__LUNA_HIGHLIGHT_SRC");
    assert_int_equal(luaL_dostring(L,
                       "package.preload['luna.complete'] = assert(load(__LUNA_COMPLETE_SRC, luna_chunkname('complete')))\n"
                       "package.preload['luna.introspect'] = assert(load(__LUNA_INTROSPECT_SRC, luna_chunkname('introspect')))\n"
                       "package.preload['luna.highlight'] = assert(load(__LUNA_HIGHLIGHT_SRC, luna_chunkname('highlight')))"),
                     LUA_OK);
}

static int setup_introspect(void **state)
{
    (void)state;
    L = luaL_newstate();
    assert_non_null(L);
    luaL_openlibs(L);
    luaL_requiref(L, "kernel", luaopen_luna_kernel, 1);
    lua_pop(L, 1);
    luna_cov_setup(L);

    /* capture output */
    lua_getglobal(L, "kernel");
    lua_getfield(L, -1, "sink");
    lua_pushcfunction(L, sink_append);
    assert_int_equal(lua_pcall(L, 1, 0, 0), LUA_OK);
    lua_pop(L, 1);

    preload_modules();
    assert_int_equal(luaL_dostring(L, "I = require 'luna.introspect'"), LUA_OK);
    return 0;
}

static int teardown_introspect(void **state)
{
    (void)state;
    luna_cov_teardown(L);
    lua_close(L);
    L = NULL;
    return 0;
}

/* eval a Lua snippet, then call I.f(snippet_result...) — helper that
 * runs `code` and pushes I.fname(result) onto the out buffer */
static void call_repr(const char *code, char *out, size_t outsz)
{
    char chunk[512];
    snprintf(chunk, sizeof(chunk), "return I.repr(%s)", code);
    out[0] = '\0';
    if (luaL_dostring(L, chunk) != LUA_OK) {
        fail_msg("repr(%s) failed: %s", code, lua_tostring(L, -1));
    }
    const char *s = lua_tostring(L, -1);
    assert_non_null(s);
    strncpy(out, s, outsz - 1);
    out[outsz - 1] = '\0';
    lua_pop(L, 1);
}

/* -- repr group -------------------------------------------------------- */

static void test_repr_string_quoted(void **state)
{
    (void)state;
    char buf[256];
    call_repr("'abc'", buf, sizeof(buf));
    assert_string_equal(buf, "'abc'");
    call_repr("'a\\'b'", buf, sizeof(buf)); /* contains a quote */
    assert_non_null(strstr(buf, "\\'"));
}

static void test_repr_number_and_bool(void **state)
{
    (void)state;
    char buf[256];
    call_repr("42", buf, sizeof(buf));
    assert_string_equal(buf, "42");
    call_repr("true", buf, sizeof(buf));
    assert_string_equal(buf, "true");
    call_repr("nil", buf, sizeof(buf));
    assert_string_equal(buf, "nil");
}

static void test_repr_table_contents(void **state)
{
    (void)state;
    char buf[512];
    call_repr("{ alpha = 1, beta = 'x' }", buf, sizeof(buf));
    assert_non_null(strstr(buf, "alpha = 1"));
    assert_non_null(strstr(buf, "beta = 'x'"));
}

static void test_repr_table_length_limit(void **state)
{
    (void)state;
    char buf[512];
    if (luaL_dostring(L, "t = {}\nfor i = 1, 30 do t[i] = i end") != LUA_OK)
        fail_msg("setup failed");
    call_repr("t", buf, sizeof(buf));
    assert_non_null(strstr(buf, "more")); /* 20 shown, 10 elided */
}

static void test_repr_table_depth_limit(void **state)
{
    (void)state;
    char buf[256];
    call_repr("{ a = { b = { c = { d = 1 } } } }", buf, sizeof(buf));
    assert_non_null(strstr(buf, "{...}")); /* depth 3 exceeded */
}

static void test_repr_cycle_detected(void **state)
{
    (void)state;
    char buf[256];
    if (luaL_dostring(L, "t = {}\nt.self = t") != LUA_OK)
        fail_msg("setup failed");
    call_repr("t", buf, sizeof(buf));
    assert_non_null(strstr(buf, "<cycle>")); /* must not hang */
}

static void test_repr_function_signature(void **state)
{
    (void)state;
    char buf[256];
    call_repr("print", buf, sizeof(buf));
    assert_non_null(strstr(buf, "function(...)")); /* vararg builtin */
    call_repr("function(a, b) end", buf, sizeof(buf));
    assert_non_null(strstr(buf, "2 params"));
}

/* -- help / whos group -------------------------------------------------- */

static void test_help_function_signature(void **state)
{
    (void)state;
    if (luaL_dostring(L, "return I.help(print)") != LUA_OK)
        fail_msg("help failed");
    const char *s = lua_tostring(L, -1);
    assert_non_null(s);
    assert_non_null(strstr(s, "function("));
    assert_non_null(strstr(s, "builtin"));
}

static void test_help_docstring_seed(void **state)
{
    (void)state;
    if (luaL_dostring(L, "return I.help(print)") != LUA_OK)
        fail_msg("help failed");
    const char *s = lua_tostring(L, -1);
    assert_non_null(strstr(s, "output funnel")); /* seeded doc */
}

static void test_help_table_fields(void **state)
{
    (void)state;
    if (luaL_dostring(L, "return I.help({ alpha = 1, fn = function() end })") != LUA_OK)
        fail_msg("help failed");
    const char *s = lua_tostring(L, -1);
    assert_non_null(strstr(s, "alpha"));
    assert_non_null(strstr(s, "fn"));
    assert_non_null(strstr(s, "function("));
}

static void test_help_doc_extension(void **state)
{
    (void)state;
    if (luaL_dostring(L,
                      "mylib = { go = function() end }\n"
                      "I.docs['mylib'] = 'mylib — example extension'\n"
                      "return I.help(mylib)") != LUA_OK)
        fail_msg("doc extension failed");
    const char *s = lua_tostring(L, -1);
    assert_non_null(strstr(s, "example extension"));
}

static void test_whos_lists_globals(void **state)
{
    (void)state;
    if (luaL_dostring(L, "answer = 42\nreturn I.whos()") != LUA_OK)
        fail_msg("whos failed");
    const char *s = lua_tostring(L, -1);
    assert_non_null(strstr(s, "answer"));
    assert_non_null(strstr(s, "number"));
    assert_non_null(strstr(s, "42"));
}

/* -- help sugar group (through the session) ------------------------------ */

/* create a session S and feed one line, capturing output */
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

static int setup_session(void **state)
{
    if (setup_introspect(state) != 0)
        return -1;
    /* load the embedded repl module and create a session */
    if (luaL_dostring(L,
                      "local repl = assert(load(__LUNA_REPL_SRC, luna_chunkname('repl')))()\n"
                      "S = repl.new()") != LUA_OK) {
        fail_msg("session setup failed: %s", lua_tostring(L, -1));
    }
    return 0;
}

static void test_sugar_trailing_question(void **state)
{
    (void)state;
    assert_string_equal(feed("t = { width = 10 }"), "ok");
    assert_string_equal(feed("t?"), "ok");
    assert_non_null(strstr(outbuf, "width"));
    assert_null(strstr(outbuf, "Out[")); /* help does not echo a result */
}

static void test_sugar_leading_question(void **state)
{
    (void)state;
    assert_string_equal(feed("?print"), "ok");
    assert_non_null(strstr(outbuf, "function("));
    assert_non_null(strstr(outbuf, "output funnel"));
}

static void test_sugar_counts_as_input(void **state)
{
    (void)state;
    feed("?type");
    feed("?print");
    assert_string_equal(feed("40 + 2"), "ok");
    /* help inputs produce no Out echo; the session keeps working */
    assert_non_null(strstr(outbuf, "Out[1]: 42"));
}

static void test_help_global_injected_on_run(void **state)
{
    (void)state;
    /* help() renders primitive values as type: repr */
    if (luaL_dostring(L, "return I.help('a string')") != LUA_OK)
        fail_msg("help failed");
    const char *s = lua_tostring(L, -1);
    assert_non_null(strstr(s, "string: 'a string'"));
}

/* -- coverage gap group ------------------------------------------------- */

static void test_signature_rejects_non_functions(void **state)
{
    (void)state;
    /* signature(fn) returns nil for anything that is not a function */
    if (luaL_dostring(L,
                      "return I.signature('not a function') == nil and "
                      "I.signature(42) == nil") != LUA_OK) {
        fail_msg("signature non-function test failed: %s", lua_tostring(L, -1));
    }
    assert_true(lua_toboolean(L, -1));
    lua_pop(L, 1);
}

static void test_help_lua_function_shows_definition_site(void **state)
{
    (void)state;
    /* functions defined in Lua carry their source location, unlike
     * C builtins which report "builtin" */
    if (luaL_dostring(L,
                      "local r = I.help(function(luna_probe) end)\n"
                      "return type(r) == 'string' and "
                      "r:find('defined at', 1, true) ~= nil") != LUA_OK) {
        fail_msg("help lua function test failed: %s", lua_tostring(L, -1));
    }
    assert_true(lua_toboolean(L, -1));
    lua_pop(L, 1);
}

static void test_doc_for_paths_and_values(void **state)
{
    (void)state;
    /* doc_for resolves seeded dotted paths, live values by scanning _G
     * (string.rep is found as a field of string), and returns nil for
     * anything it cannot name */
    if (luaL_dostring(L,
                      "local a = I.doc_for('string.format')\n"
                      "local b = I.doc_for(string.rep)\n"
                      "local c = I.doc_for('no/such/doc/path')\n"
                      "return a ~= nil and b ~= nil and c == nil") != LUA_OK) {
        fail_msg("doc_for test failed: %s", lua_tostring(L, -1));
    }
    assert_true(lua_toboolean(L, -1));
    lua_pop(L, 1);
}

static void test_suggest_names_the_two_closest_candidates(void **state)
{
    (void)state;
    /* suggest parses the global name out of the error message and
     * returns the two closest matches by edit distance (print is always
     * at distance 1; princ is planted at distance 1 too). The queried
     * name itself must stay undefined — an existing global means there
     * is nothing to suggest. The alphabetical tie-breaks make the pick
     * deterministic, so the exact reply is assertable regardless of
     * pairs() order */
    if (luaL_dostring(L,
                      "princ = function() end\n"
                      "local r = I.suggest("
                      "\"attempt to call a nil value (global 'prinb')\")\n"
                      "return r == \"did you mean 'princ' or 'print'?\"") != LUA_OK) {
        fail_msg("suggest test failed: %s", lua_tostring(L, -1));
    }
    assert_true(lua_toboolean(L, -1));
    lua_pop(L, 1);
}

/* -- runner --------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_repr_string_quoted, setup_introspect, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_repr_number_and_bool, setup_introspect, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_repr_table_contents, setup_introspect, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_repr_table_length_limit, setup_introspect, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_repr_table_depth_limit, setup_introspect, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_repr_cycle_detected, setup_introspect, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_repr_function_signature, setup_introspect, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_help_function_signature, setup_introspect, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_help_docstring_seed, setup_introspect, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_help_table_fields, setup_introspect, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_help_doc_extension, setup_introspect, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_whos_lists_globals, setup_introspect, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_sugar_trailing_question, setup_session, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_sugar_leading_question, setup_session, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_sugar_counts_as_input, setup_session, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_help_global_injected_on_run, setup_session, teardown_introspect),
        /* coverage gap tests */
        cmocka_unit_test_setup_teardown(test_signature_rejects_non_functions, setup_introspect, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_help_lua_function_shows_definition_site, setup_introspect, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_doc_for_paths_and_values, setup_introspect, teardown_introspect),
        cmocka_unit_test_setup_teardown(test_suggest_names_the_two_closest_candidates, setup_introspect, teardown_introspect),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
