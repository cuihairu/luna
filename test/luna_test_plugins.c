/* Plugin system: discovery order (project/user/LUNA_PLUGIN_PATH),
 * manifest handling, injected modules, extension-point wiring and
 * failure isolation.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "luna_kernel.h"
#include "luna_lua.h"

int luaopen_lpeg(lua_State *L); /* highlight -> lexer -> lpeg (introspect) */
int luaopen_lfs(lua_State *L);  /* plugins.lua is_dir -> lfs.attributes */

static lua_State *L;
static char outbuf[4096];
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

static void run(lua_State *l, const char *code)
{
    if (luaL_dostring(l, code) != LUA_OK) {
        fail_msg("lua error: %s", lua_tostring(l, -1));
    }
}

/* run(code), leaving one string on the stack; returns it */
static const char *eval_string(const char *code)
{
    static char buf[512];
    run(L, code);
    const char *s = lua_tostring(L, -1);
    snprintf(buf, sizeof(buf), "%s", s ? s : "(nil)");
    lua_pop(L, 1);
    return buf;
}

static int setup_plugins(void **state)
{
    (void)state;
    /* discovery reads ./plugins (absent in the build dir), then
     * $HOME/.luna/plugins, then $LUNA_PLUGIN_PATH — point the latter
     * two at the fixtures */
    setenv("HOME", LUNA_TEST_PLUGIN_FIXTURES "/pluginshome", 1);
    setenv("LUNA_PLUGIN_PATH", LUNA_TEST_PLUGIN_FIXTURES "/pluginsenv", 1);

    L = luaL_newstate();
    assert_non_null(L);
    luaL_openlibs(L);
    luaL_requiref(L, "kernel", luaopen_luna_kernel, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "lpeg", luaopen_lpeg, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "lfs", luaopen_lfs, 0);
    lua_pop(L, 1);

    lua_getglobal(L, "kernel");
    lua_getfield(L, -1, "sink");
    lua_pushcfunction(L, sink_append);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        fail_msg("sink setup failed: %s", lua_tostring(L, -1));
    }
    lua_pop(L, 1);

    /* preload the policy modules the plugin chain touches (mirrors
     * the luna entry); package.path gains the bundled modules so
     * manifest parsing (dkjson) works like it does in the binary */
    run(L,
        "package.path = '" LUNA_TEST_MODULES_DIR "/?.lua;"
        LUNA_TEST_MODULES_DIR "/?/init.lua;' .. package.path");
    lua_pushlstring(L, LUNA_LUA_MAGIC, sizeof(LUNA_LUA_MAGIC) - 1);
    lua_setglobal(L, "__LUNA_MAGIC_SRC");
    lua_pushlstring(L, LUNA_LUA_INTROSPECT, sizeof(LUNA_LUA_INTROSPECT) - 1);
    lua_setglobal(L, "__LUNA_INTROSPECT_SRC");
    lua_pushlstring(L, LUNA_LUA_MODULES, sizeof(LUNA_LUA_MODULES) - 1);
    lua_setglobal(L, "__LUNA_MODULES_SRC");
    lua_pushlstring(L, LUNA_LUA_PLUGINS, sizeof(LUNA_LUA_PLUGINS) - 1);
    lua_setglobal(L, "__LUNA_PLUGINS_SRC");
    run(L,
        "package.preload['luna.magic'] = assert(load(__LUNA_MAGIC_SRC, '=(luna/magic)'))\n"
        "package.preload['luna.introspect'] = assert(load(__LUNA_INTROSPECT_SRC, '=(luna/introspect)'))\n"
        "package.preload['luna.modules'] = assert(load(__LUNA_MODULES_SRC, '=(luna/modules)'))\n"
        "package.preload['luna.plugins'] = assert(load(__LUNA_PLUGINS_SRC, '=(luna/plugins)'))\n"
        "P = require('luna.plugins')\n"
        "P.load_all()");
    return 0;
}

static int teardown_plugins(void **state)
{
    (void)state;
    unsetenv("HOME");
    unsetenv("LUNA_PLUGIN_PATH");
    lua_close(L);
    L = NULL;
    return 0;
}

/* -- discovery group ------------------------------------------------------ */

static void test_plugins_from_home_and_env_path_load(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("return tostring(P.loaded['good'] ~= nil) .. '/' .. "
                    "tostring(P.loaded['envplug'] ~= nil)"),
        "true/true");
}

static void test_manifest_reports_are_introspectable(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("return P.loaded['good'].version .. '/' .. "
                    "P.loaded['good'].description"),
        "1.0.0/registers a magic command and injects a module");
}

static void test_same_name_shadows_are_overridden(void **state)
{
    (void)state;
    /* shadow-a sorts before shadow-b inside the same location: the
     * first name wins, the later directory is recorded */
    assert_string_equal(
        eval_string("return P.loaded['shadow'].version"), "1.0.0");
    assert_non_null(strstr(
        eval_string("return tostring(P.overridden['shadow'])"), "shadow-b"));
}

/* -- failure isolation group ----------------------------------------------- */

static void test_raising_plugin_is_reported_not_fatal(void **state)
{
    (void)state;
    assert_non_null(strstr(
        eval_string("return P.failed['broken']"), "boom from broken"));
    /* siblings after the failure still loaded */
    assert_string_equal(
        eval_string("return tostring(P.loaded['goodpack'] ~= nil)"), "true");
}

/* A manifest that does not parse is one failure reason (the parser's
 * own words), a manifest that parses but names nothing another — both
 * are reported under the directory that has no name to file them by,
 * and neither stops the plugins that follow. */
static void test_unparsable_manifest_is_reported_by_dir(void **state)
{
    (void)state;
    assert_non_null(strstr(
        eval_string("local found\n"
                    "for k, v in pairs(P.failed) do\n"
                    "  if k:find('badjson') then found = v end\n"
                    "end\n"
                    "return found"),
        "does not parse"));
    /* the siblings of the broken manifests still loaded */
    assert_string_equal(
        eval_string("return tostring(P.loaded['good'] ~= nil)"), "true");
}

static void test_nameless_manifest_is_reported_by_dir(void **state)
{
    (void)state;
    assert_non_null(strstr(
        eval_string("local found\n"
                    "for k, v in pairs(P.failed) do\n"
                    "  if k:find('noname') then found = v end\n"
                    "end\n"
                    "return found"),
        "no usable name"));
}

/* -- extension points group ------------------------------------------------- */

static void test_injected_modules_resolve_via_require(void **state)
{
    (void)state;
    /* plain value */
    assert_string_equal(eval_string("return require('plugmod').n"), "42");
    /* loader value, resolved lazily */
    assert_string_equal(eval_string("return require('packmod').from"), "pack");
}

static void test_plugin_registers_a_working_magic_command(void **state)
{
    (void)state;
    outlen = 0;
    outbuf[0] = '\0';
    run(L, "local m = require('luna.magic')\n"
           "assert(m.dispatch({}, 'plug', 'hello'))");
    assert_non_null(strstr(outbuf, "plug ran: hello"));
}

/* -- runner ------------------------------------------------------------------ */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_plugins_from_home_and_env_path_load, setup_plugins, teardown_plugins),
        cmocka_unit_test_setup_teardown(test_manifest_reports_are_introspectable, setup_plugins, teardown_plugins),
        cmocka_unit_test_setup_teardown(test_same_name_shadows_are_overridden, setup_plugins, teardown_plugins),
        cmocka_unit_test_setup_teardown(test_raising_plugin_is_reported_not_fatal, setup_plugins, teardown_plugins),
        cmocka_unit_test_setup_teardown(test_unparsable_manifest_is_reported_by_dir, setup_plugins, teardown_plugins),
        cmocka_unit_test_setup_teardown(test_nameless_manifest_is_reported_by_dir, setup_plugins, teardown_plugins),
        cmocka_unit_test_setup_teardown(test_injected_modules_resolve_via_require, setup_plugins, teardown_plugins),
        cmocka_unit_test_setup_teardown(test_plugin_registers_a_working_magic_command, setup_plugins, teardown_plugins),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
