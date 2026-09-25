/* Tab completion engine: global/keyword/field candidates, nested chain
 * resolution, callable "(" suffix, metatable-aware lookup, and the
 * plugin completion-source extension point with failure isolation.
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
#include "luna_lua.h" /* generated: embedded complete module */

int luaopen_lfs(lua_State *L); /* directory scans for require completion */

static lua_State *L;

static int setup_complete(void **state)
{
    (void)state;
    L = luaL_newstate();
    assert_non_null(L);
    luaL_openlibs(L);

    luaL_requiref(L, "kernel", luaopen_luna_kernel, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "lfs", luaopen_lfs, 0);
    lua_pop(L, 1);

    /* a fixture package tree reachable through package.path, the way
     * the bundled luna_modules dir is in real runs */
    if (luaL_dostring(L, "package.path = '" COMPLETE_FIXTURES
                          "/?.lua;" COMPLETE_FIXTURES "/?/init.lua;' .. package.path") != LUA_OK) {
        fail_msg("cannot set package.path: %s", lua_tostring(L, -1));
    }

    /* preload the embedded completion module, then grab a handle */
    lua_pushlstring(L, LUNA_LUA_COMPLETE, sizeof(LUNA_LUA_COMPLETE) - 1);
    lua_setglobal(L, "__LUNA_COMPLETE_SRC");
    if (luaL_dostring(L, "package.preload['luna.complete'] = assert(load(__LUNA_COMPLETE_SRC, '=(luna/complete)'))") != LUA_OK) {
        fail_msg("cannot preload complete: %s", lua_tostring(L, -1));
    }

    if (luaL_dostring(L, "C = require 'luna.complete'") != LUA_OK) {
        fail_msg("cannot require luna.complete: %s", lua_tostring(L, -1));
    }
    return 0;
}

static int teardown_complete(void **state)
{
    (void)state;
    lua_close(L);
    L = NULL;
    return 0;
}

/* call C.line(line) and render the candidate list as "a|b|c" */
static void completions(const char *line, char *out, size_t outsz)
{
    lua_getglobal(L, "C");
    lua_getfield(L, -1, "line");
    lua_remove(L, -2); /* plain function call, not a method */
    lua_pushstring(L, line);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        fail_msg("complete failed: %s", lua_tostring(L, -1));
    }
    size_t n = 0;
    out[0] = '\0';
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        const char *cand = lua_tostring(L, -1);
        assert_non_null(cand);
        n += strlen(cand) + 2;
        assert_true(n < outsz);
        if (out[0] != '\0') {
            strcat(out, "|");
        }
        strcat(out, cand);
        lua_pop(L, 1);
    }
    lua_pop(L, 1); /* result table */
}

/* -- globals and keywords --------------------------------------------- */

static void test_complete_global_prefix(void **state)
{
    (void)state;
    char buf[256];
    completions("pri", buf, sizeof(buf));
    assert_non_null(strstr(buf, "print"));
}

static void test_complete_keyword(void **state)
{
    (void)state;
    char buf[256];
    completions("whi", buf, sizeof(buf));
    assert_non_null(strstr(buf, "while"));
    assert_null(strstr(buf, "print"));
}

static void test_complete_exact_global_gets_call_parens(void **state)
{
    (void)state;
    /* unique match whose value is a function completes as a call */
    char buf[256];
    completions("prin", buf, sizeof(buf));
    assert_non_null(strstr(buf, "print("));
}

static void test_complete_no_match_after_paren(void **state)
{
    (void)state;
    char buf[256];
    completions("print(", buf, sizeof(buf)); /* nothing word-like: empty */
    assert_string_equal(buf, "");
}

/* -- field chains ------------------------------------------------------ */

static void test_complete_table_field(void **state)
{
    (void)state;
    if (luaL_dostring(L, "t = { alpha = 1, beta = 2 }") != LUA_OK)
        fail_msg("setup failed");
    char buf[256];
    completions("t.a", buf, sizeof(buf));
    assert_string_equal(buf, "alpha");
}

static void test_complete_field_multiple_matches(void **state)
{
    (void)state;
    if (luaL_dostring(L, "t2 = { foo1 = 1, foo2 = 2 }") != LUA_OK)
        fail_msg("setup failed");
    char buf[256];
    completions("t2.fo", buf, sizeof(buf));
    assert_non_null(strstr(buf, "foo1"));
    assert_non_null(strstr(buf, "foo2"));
}

static void test_complete_nested_chain(void **state)
{
    (void)state;
    if (luaL_dostring(L, "t3 = { sub = { gamma = 1, delta = 2 } }") != LUA_OK)
        fail_msg("setup failed");
    char buf[256];
    completions("t3.sub.g", buf, sizeof(buf));
    assert_string_equal(buf, "gamma");
}

static void test_complete_after_colon(void **state)
{
    (void)state;
    if (luaL_dostring(L, "t4 = { meth = function() end }") != LUA_OK)
        fail_msg("setup failed");
    char buf[256];
    completions("t4:met", buf, sizeof(buf));
    /* unique function match: completes as a call */
    assert_string_equal(buf, "meth(");
}

static void test_complete_stdlib_field(void **state)
{
    (state);
    char buf[256];
    completions("string.fo", buf, sizeof(buf));
    assert_non_null(strstr(buf, "format"));
}

static void test_complete_missing_base_is_empty(void **state)
{
    (void)state;
    char buf[256];
    completions("no_such_table.any", buf, sizeof(buf));
    assert_string_equal(buf, "");
}

static void test_complete_index_side_effect_is_isolated(void **state)
{
    (void)state;
    /* a raising __index must not crash completion */
    if (luaL_dostring(L,
                      "t5 = setmetatable({}, { __index = function() error('boom') end })")
        != LUA_OK)
        fail_msg("setup failed");
    char buf[256];
    completions("t5.x", buf, sizeof(buf));
    assert_string_equal(buf, "");
}

static void test_complete_metatable_field_found(void **state)
{
    (void)state;
    if (luaL_dostring(L,
                      "t6 = setmetatable({}, { __index = { method = function() end, afield = 1 } })")
        != LUA_OK)
        fail_msg("setup failed");
    char buf[256];
    completions("t6.met", buf, sizeof(buf));
    assert_string_equal(buf, "method(");
}

/* -- plugin completion sources ----------------------------------------- */

static void test_complete_plugin_source_merged(void **state)
{
    (void)state;
    if (luaL_dostring(L,
                      "C.add_source(function(line) return { 'plugin_word' } end)") != LUA_OK)
        fail_msg("add_source failed");
    char buf[256];
    completions("pri", buf, sizeof(buf));
    assert_non_null(strstr(buf, "print"));
    assert_non_null(strstr(buf, "plugin_word"));
}

static void test_complete_plugin_source_failure_isolated(void **state)
{
    (void)state;
    if (luaL_dostring(L,
                      "C.add_source(function(line) error('plugin exploded') end)\n"
                      "C.add_source(function(line) return { 'still_here' } end)") != LUA_OK)
        fail_msg("add_source failed");
    char buf[256];
    completions("pri", buf, sizeof(buf));
    assert_non_null(strstr(buf, "print"));
    assert_non_null(strstr(buf, "still_here"));
}

static void test_complete_plugin_source_non_table_ignored(void **state)
{
    (void)state;
    if (luaL_dostring(L, "C.add_source(function(line) return 42 end)") != LUA_OK)
        fail_msg("add_source failed");
    char buf[256];
    completions("pri", buf, sizeof(buf));
    assert_non_null(strstr(buf, "print"));
}

/* -- require-target completion ------------------------------------------ */

static void test_complete_require_lists_disk_packages(void **state)
{
    (void)state;
    char buf[256];
    completions("require \"dep", buf, sizeof(buf));
    assert_non_null(strstr(buf, "depalpha")); /* flat package file on disk */
    /* loaded stdlib names join the candidates */
    completions("require \"st", buf, sizeof(buf));
    assert_non_null(strstr(buf, "string"));
}

static void test_complete_require_preload_entries(void **state)
{
    (void)state;
    if (luaL_dostring(L, "package.preload['depbeta'] = function() end") != LUA_OK) {
        fail_msg("cannot seed preload: %s", lua_tostring(L, -1));
    }
    char buf[256];
    completions("require('depb", buf, sizeof(buf));
    assert_non_null(strstr(buf, "depbeta"));
}

static void test_complete_require_dotted_subpackage_file(void **state)
{
    (void)state;
    char buf[256];
    /* subpkg/flourish.lua on disk, dotted through the package name */
    completions("require \"subpkg.fl", buf, sizeof(buf));
    assert_non_null(strstr(buf, "subpkg.flourish"));
}

static void test_complete_require_dotted_loaded_field(void **state)
{
    (void)state;
    /* load the package, then continue into its loaded table's fields */
    if (luaL_dostring(L, "package.loaded['subpkg'] = { alpha = function() end, beta = 2 }") != LUA_OK) {
        fail_msg("cannot seed loaded: %s", lua_tostring(L, -1));
    }
    char buf[256];
    completions("require 'subpkg.al", buf, sizeof(buf));
    assert_non_null(strstr(buf, "subpkg.alpha"));
}

static void test_complete_require_no_match_is_empty(void **state)
{
    (void)state;
    char buf[256];
    completions("require \"zzzznope", buf, sizeof(buf));
    assert_string_equal(buf, "");
    /* the require source stays quiet when the line is not a require */
    completions("pri", buf, sizeof(buf));
    assert_non_null(strstr(buf, "print"));
}

static void test_complete_later_source_duplicates_dropped(void **state)
{
    (void)state;
    if (luaL_dostring(L,
                      "C.add_source(function(line) return { 'dup_once', 'dup_once' } end)\n"
                      "C.add_source(function(line) return { 'dup_once' } end)") != LUA_OK) {
        fail_msg("add_source failed");
    }
    char buf[256];
    completions("pri", buf, sizeof(buf));
    assert_non_null(strstr(buf, "dup_once"));
    assert_null(strstr(buf, "dup_once|dup_once")); /* deduped on merge */
}

/* -- runner ------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_complete_global_prefix, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_keyword, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_exact_global_gets_call_parens, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_no_match_after_paren, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_table_field, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_field_multiple_matches, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_nested_chain, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_after_colon, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_stdlib_field, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_missing_base_is_empty, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_index_side_effect_is_isolated, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_metatable_field_found, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_plugin_source_merged, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_plugin_source_failure_isolated, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_plugin_source_non_table_ignored, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_require_lists_disk_packages, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_require_preload_entries, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_require_dotted_subpackage_file, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_require_dotted_loaded_field, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_require_no_match_is_empty, setup_complete, teardown_complete),
        cmocka_unit_test_setup_teardown(test_complete_later_source_duplicates_dropped, setup_complete, teardown_complete),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
