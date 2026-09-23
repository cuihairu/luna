/* Syntax highlighting: scintillua/LPeg-backed ANSI rendering, the
 * highlight.set extension point, no-color degradation, and colored
 * session output through the REPL.
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
#include "luna_lua.h" /* generated: embedded repl + highlight sources */

int luaopen_lpeg(lua_State *L); /* no lpeg.h in deps/lpeg */

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

/* preload highlight (+repl for the session group); registers the lpeg
 * module exactly like luna_main does */
static int setup_highlight(void **state)
{
    (void)state;
    L = luaL_newstate();
    assert_non_null(L);
    luaL_openlibs(L);
    luaL_requiref(L, "kernel", luaopen_luna_kernel, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "lpeg", luaopen_lpeg, 0);
    lua_pop(L, 1);

    /* output capture */
    lua_getglobal(L, "kernel");
    lua_getfield(L, -1, "sink");
    lua_pushcfunction(L, sink_append);
    assert_int_equal(lua_pcall(L, 1, 0, 0), LUA_OK);
    lua_pop(L, 1);

    lua_pushlstring(L, LUNA_LUA_HIGHLIGHT, sizeof(LUNA_LUA_HIGHLIGHT) - 1);
    lua_setglobal(L, "__LUNA_HIGHLIGHT_SRC");
    lua_pushlstring(L, LUNA_LUA_COMPLETE, sizeof(LUNA_LUA_COMPLETE) - 1);
    lua_setglobal(L, "__LUNA_COMPLETE_SRC");
    lua_pushlstring(L, LUNA_LUA_INTROSPECT, sizeof(LUNA_LUA_INTROSPECT) - 1);
    lua_setglobal(L, "__LUNA_INTROSPECT_SRC");
    lua_pushlstring(L, LUNA_LUA_REPL, sizeof(LUNA_LUA_REPL) - 1);
    lua_setglobal(L, "__LUNA_REPL_SRC");
    lua_pushstring(L, LUNA_TEST_LEXERS_DIR);
    lua_setglobal(L, "__LUNA_LEXERS_DIR");
    assert_int_equal(luaL_dostring(L,
                       "package.preload['luna.highlight'] = assert(load(__LUNA_HIGHLIGHT_SRC, '=(luna/highlight)'))\n"
                       "package.preload['luna.complete'] = assert(load(__LUNA_COMPLETE_SRC, '=(luna/complete)'))\n"
                       "package.preload['luna.introspect'] = assert(load(__LUNA_INTROSPECT_SRC, '=(luna/introspect)'))\n"
                       "H = require 'luna.highlight'"),
                     LUA_OK);
    return 0;
}

static int teardown_highlight(void **state)
{
    (void)state;
    lua_close(L);
    L = NULL;
    return 0;
}

/* call H.render(code, colors) and copy the result out */
static void render(const char *code, int colors, char *out, size_t outsz)
{
    char chunk[256];
    snprintf(chunk, sizeof(chunk), "return H.render([[%s]], %s)",
             code, colors ? "true" : "false");
    out[0] = '\0';
    if (luaL_dostring(L, chunk) != LUA_OK) {
        fail_msg("render(%s) failed: %s", code, lua_tostring(L, -1));
    }
    const char *s = lua_tostring(L, -1);
    assert_non_null(s);
    strncpy(out, s, outsz - 1);
    out[outsz - 1] = '\0';
    lua_pop(L, 1);
}

/* -- render group ------------------------------------------------------- */

static void test_render_wraps_text_in_ansi(void **state)
{
    (void)state;
    char buf[1024];
    render("local x = 1", 1, buf, sizeof(buf));
    assert_non_null(strstr(buf, "\x1b["));    /* ANSI present */
    assert_non_null(strstr(buf, "local"));    /* text preserved (reset
                                                 sequences may split it) */
    assert_non_null(strstr(buf, "x = "));
}

static void test_render_distinct_styles(void **state)
{
    (void)state;
    char buf[2048];
    /* string vs number vs keyword vs comment: four different escapes */
    render("local s = 'str' .. 42 -- c", 1, buf, sizeof(buf));
    const char *p_string = strstr(buf, "\x1b[32m");
    const char *p_number = strstr(buf, "\x1b[36m");
    const char *p_keyword = strstr(buf, "\x1b[1;31m");
    const char *p_comment = strstr(buf, "\x1b[90m");
    assert_non_null(p_string);
    assert_non_null(p_number);
    assert_non_null(p_keyword);
    assert_non_null(p_comment);
    assert_true(p_string != p_number && p_number != p_keyword);
    /* each span resets before the next one starts */
    const char *reset = strstr(buf, "\x1b[0m");
    assert_non_null(reset);
}

static void test_render_function_name_styled(void **state)
{
    (void)state;
    char buf[1024];
    render("function greet() end", 1, buf, sizeof(buf));
    /* the lexer tags a call-position name as 'function' -> bold yellow */
    assert_non_null(strstr(buf, "\x1b[1;33mgreet"));
}

static void test_render_multi_line_no_crash(void **state)
{
    (void)state;
    char buf[2048];
    render("local a = 1\nreturn a + 2", 1, buf, sizeof(buf));
    assert_non_null(strstr(buf, "\x1b["));
}

static void test_render_empty_string(void **state)
{
    (void)state;
    char buf[64];
    render("", 1, buf, sizeof(buf));
    assert_string_equal(buf, "");
}

/* -- degradation group --------------------------------------------------- */

static void test_render_disabled_is_identity(void **state)
{
    (void)state;
    char buf[1024];
    render("local x = 'hi'", 0, buf, sizeof(buf));
    assert_string_equal(buf, "local x = 'hi'");
}

static void test_render_survives_broken_highlighter(void **state)
{
    (void)state;
    if (luaL_dostring(L,
                      "H.set(function() error('plugin exploded') end)\n"
                      "local r = H.render('local x = 1', true)\n"
                      "return r == 'local x = 1'") != LUA_OK) {
        fail_msg("broken highlighter not isolated: %s", lua_tostring(L, -1));
    }
    assert_true(lua_toboolean(L, -1));
    lua_pop(L, 1);
}

/* -- extension point group ------------------------------------------------ */

static void test_set_replaces_active_highlighter(void **state)
{
    (void)state;
    if (luaL_dostring(L,
                      "H.set(function(text) return '<' .. text .. '>' end)\n"
                      "local r = H.render('x', true)\n"
                      "H.reset()\n"
                      "return r") != LUA_OK) {
        fail_msg("set/replace failed: %s", lua_tostring(L, -1));
    }
    assert_string_equal(lua_tostring(L, -1), "<x>");
    lua_pop(L, 1);
    /* reset restored the scintillua-backed default */
    char buf[1024];
    render("local x = 1", 1, buf, sizeof(buf));
    assert_non_null(strstr(buf, "\x1b["));
}

static void test_set_rejects_non_function(void **state)
{
    (void)state;
    if (luaL_dostring(L,
                      "local ok, err = pcall(H.set, 'not a function')\n"
                      "return ok == false and err ~= nil") != LUA_OK) {
        fail_msg("set validation failed: %s", lua_tostring(L, -1));
    }
    assert_true(lua_toboolean(L, -1));
    lua_pop(L, 1);
}

/* -- session group (colors through the REPL) ------------------------------ */

static int setup_session(void **state)
{
    if (setup_highlight(state) != 0)
        return -1;
    if (luaL_dostring(L,
                      "local repl = assert(load(__LUNA_REPL_SRC, '=(luna/repl)'))()\n"
                      "S = repl.new({ color = true })") != LUA_OK) {
        fail_msg("session setup failed: %s", lua_tostring(L, -1));
    }
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

static void test_session_colored_out_label(void **state)
{
    (void)state;
    assert_string_equal(feed("40 + 2"), "ok");
    /* colored Out label and highlighted value */
    assert_non_null(strstr(outbuf, "\x1b[1;35mOut[1]: \x1b[0m"));
    assert_non_null(strstr(outbuf, "\x1b[36m42\x1b[0m"));
}

static void test_session_colored_error(void **state)
{
    (void)state;
    assert_string_equal(feed("error('boom')"), "error");
    assert_non_null(strstr(outbuf, "\x1b[31m")); /* red */
    assert_non_null(strstr(outbuf, "boom"));
}

static void test_session_plain_when_color_off(void **state)
{
    (void)state;
    /* build a plain session through the same module */
    if (luaL_dostring(L,
                      "local repl = assert(load(__LUNA_REPL_SRC, '=(luna/repl)'))()\n"
                      "S = repl.new({ color = false })") != LUA_OK) {
        fail_msg("plain session setup failed: %s", lua_tostring(L, -1));
    }
    assert_string_equal(feed("40 + 2"), "ok");
    assert_null(strstr(outbuf, "\x1b["));
    assert_non_null(strstr(outbuf, "Out[1]: 42"));
}

/* -- runner --------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_render_wraps_text_in_ansi, setup_highlight, teardown_highlight),
        cmocka_unit_test_setup_teardown(test_render_distinct_styles, setup_highlight, teardown_highlight),
        cmocka_unit_test_setup_teardown(test_render_function_name_styled, setup_highlight, teardown_highlight),
        cmocka_unit_test_setup_teardown(test_render_multi_line_no_crash, setup_highlight, teardown_highlight),
        cmocka_unit_test_setup_teardown(test_render_empty_string, setup_highlight, teardown_highlight),
        cmocka_unit_test_setup_teardown(test_render_disabled_is_identity, setup_highlight, teardown_highlight),
        cmocka_unit_test_setup_teardown(test_render_survives_broken_highlighter, setup_highlight, teardown_highlight),
        cmocka_unit_test_setup_teardown(test_set_replaces_active_highlighter, setup_highlight, teardown_highlight),
        cmocka_unit_test_setup_teardown(test_set_rejects_non_function, setup_highlight, teardown_highlight),
        cmocka_unit_test_setup_teardown(test_session_colored_out_label, setup_session, teardown_highlight),
        cmocka_unit_test_setup_teardown(test_session_colored_error, setup_session, teardown_highlight),
        cmocka_unit_test_setup_teardown(test_session_plain_when_color_off, setup_session, teardown_highlight),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
