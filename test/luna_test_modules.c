/* Module system: Node-style require resolution (bare names walking up
 * luna_modules/, package.json manifests, relative paths, package.loaded
 * caching) and the stdlib modules backed by the bundled libraries.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include <cmocka.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "luna_lua.h" /* embedded luna.modules source */
#include "luna_cov.h"

int luaopen_lfs(lua_State *L);
int luaopen_socket_core(lua_State *L);
int luaopen_mime_core(lua_State *L);
int luaopen_zlib(lua_State *L);
#ifdef LUNA_HAVE_OPENSSL
int luaopen__openssl(lua_State *L);
int luaopen__openssl_digest(lua_State *L);
int luaopen__openssl_hmac(lua_State *L);
int luaopen__openssl_rand(lua_State *L);
#endif

static lua_State *L;

static void run(lua_State *l, const char *code)
{
    if (luaL_dostring(l, code) != LUA_OK) {
        fail_msg("lua error: %s", lua_tostring(l, -1));
    }
}

/* run(code) and return the single string the code produced */
static const char *eval_string(const char *code)
{
    static char buf[512];
    run(L, code);
    const char *s = lua_tostring(L, -1);
    snprintf(buf, sizeof(buf), "%s", s ? s : "(nil)");
    lua_pop(L, 1);
    return buf;
}

/* run(code) -> boolean the code left on the stack */
static int eval_bool(const char *code)
{
    run(L, code);
    int v = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

static int setup_modules(void **state)
{
    (void)state;
    L = luaL_newstate();
    assert_non_null(L);
    luaL_openlibs(L);
    luna_cov_setup(L);

    /* stage the C backends the way luna_main does */
    luaL_requiref(L, "lfs", luaopen_lfs, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "socket.core", luaopen_socket_core, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "mime.core", luaopen_mime_core, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "zlib.core", luaopen_zlib, 0);
    lua_pop(L, 1);
#ifdef LUNA_HAVE_OPENSSL
    luaL_requiref(L, "_openssl", luaopen__openssl, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "_openssl.digest", luaopen__openssl_digest, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "_openssl.hmac", luaopen__openssl_hmac, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "_openssl.rand", luaopen__openssl_rand, 0);
    lua_pop(L, 1);
#endif

    /* bundled pure-Lua modules (dkjson, socket, ...) reachable through
     * package.path, mirroring luna's setup_module_paths */
    run(L,
        "package.path = '" LUNA_TEST_MODULES_DIR "/?.lua;"
        LUNA_TEST_MODULES_DIR "/?/init.lua;' .. package.path");

    /* install the module system (mirrors the luna entry chunk) */
    lua_pushlstring(L, LUNA_LUA_MODULES, sizeof(LUNA_LUA_MODULES) - 1);
    lua_setglobal(L, "__LUNA_MODULES_SRC");
    run(L,
        "package.preload['luna.modules'] = assert(load(__LUNA_MODULES_SRC, luna_chunkname('modules')))\n"
        "require('luna.modules').install()\n"
        "M = require('luna.modules')");
    return 0;
}

static int teardown_modules(void **state)
{
    (void)state;
    luna_cov_teardown(L);
    lua_close(L);
    L = NULL;
    return 0;
}

/* -- require resolution group -------------------------------------------- */

static void test_bare_module_from_requiring_file_dir(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local v = dofile('" FIXTURES "/myapp/app.lua')\n"
                    "return v"),
        "app:dep1:dep2:util");
}

static void test_manifest_main_picks_entry(void **state)
{
    (void)state;
    /* dep2 has no init.lua: only the manifest's "main" field can find
     * lib/entry.lua */
    assert_true(eval_bool(
        "local v = dofile('" FIXTURES "/myapp/app.lua')\n"
        "return v:find(':dep2:', 1, true) ~= nil"));
    /* and the parsed manifest is introspectable */
    assert_string_equal(
        eval_string("local m = M.manifest('" FIXTURES
                    "/myapp/luna_modules/dep2')\n"
                    "return m.name .. '/' .. m.version"),
        "dep2/1.0.0");
}

static void test_bare_lookup_walks_up_directories(void **state)
{
    (void)state;
    /* inner/ has no luna_modules: the walk continues in nested/ */
    assert_string_equal(
        eval_string("return dofile('" FIXTURES "/nested/inner/app2.lua')"),
        "up:nested-dep1");
}

static void test_relative_require_resolves_to_caller_dir(void **state)
{
    (void)state;
    /* app.lua requires "./lib/util" (covered by the bare test above);
     * here the resolver's explicit from-directory form is checked, so
     * the case does not depend on the test process's cwd */
    assert_string_equal(
        eval_string("return M.resolve_relative('./lib/util', '" FIXTURES
                    "/myapp')().name"),
        "util");
}

/* -- in-package subpath group ------------------------------------------- */

static void test_subpath_resolves_inside_package(void **state)
{
    (void)state;
    /* dep1.lib.helper (dotted and slash spellings), dep4.lib.deep's
     * init.lua convention, and dep3.sub where the literal dotted
     * package wins over the subpath file dep3/sub.lua */
    assert_string_equal(
        eval_string("return dofile('" FIXTURES "/myapp/app3.lua')"),
        "subpaths:dep1-helper:dep1-helper:dep4-deep:dep3-literal");
}

static void test_subpath_walks_up_directories(void **state)
{
    (void)state;
    /* inner/ has no luna_modules: dep1.tools.hook resolves one level up */
    assert_string_equal(
        eval_string("return dofile('" FIXTURES "/nested/inner/app4.lua')"),
        "nested-hook");
}

static void test_missing_subpath_still_falls_through(void **state)
{
    (void)state;
    /* no dep5 anywhere: the searcher chain keeps its "not found" verdict */
    assert_true(eval_bool(
        "local ok, err = pcall(dofile, '" FIXTURES "/myapp/app5.lua')\n"
        "return ok == false and tostring(err):find('not found', 1, true) ~= nil"));
}

static void test_loaded_caches_across_requires(void **state){
    (void)state;
    run(L, "_G.__DEP1_LOADS = nil");
    /* two dofile passes: the second require "dep1" must hit
     * package.loaded, not re-run init.lua */
    assert_string_equal(
        eval_string("dofile('" FIXTURES "/myapp/app.lua')\n"
                    "local first = _G.__DEP1_LOADS\n"
                    "dofile('" FIXTURES "/myapp/app.lua')\n"
                    "return tostring(first) .. '/' .. tostring(_G.__DEP1_LOADS)"),
        "1/1");
}

static void test_missing_module_reports_clearly(void **state)
{
    (void)state;
    assert_true(eval_bool(
        "local ok, err = pcall(require, 'definitely_not_a_package_xyz')\n"
        "return ok == false and type(err) == 'string' and err:find('not found', 1, true) ~= nil"));
}

/* -- stdlib modules group ------------------------------------------------- */

static void test_json_roundtrip_and_node_aliases(void **state)
{
    (void)state;
    assert_true(eval_bool(
        "local json = require('json')\n"
        "local t = {name = 'luna', n = 42, nested = {true}}\n"
        "local enc = json.encode(t)\n"
        "local back = json.decode(enc)\n"
        "assert(back.name == 'luna' and back.n == 42 and back.nested[1] == true)\n"
        "assert(json.parse == json.decode and json.stringify == json.encode)\n"
        "return true"));
}

static void test_fs_reads_and_attributes(void **state)
{
    (void)state;
    assert_true(eval_bool(
        "local fs = require('fs')\n"
        "assert(fs.isFile('" FIXTURES "/myapp/app.lua'))\n"
        "assert(fs.isDirectory('" FIXTURES "/myapp'))\n"
        "assert(not fs.exists('" FIXTURES "/nope/nothing'))\n"
        "local src = fs.readFileSync('" FIXTURES "/myapp/lib/util.lua')\n"
        "assert(src:find('util', 1, true))\n"
        "return true"));
}

static void test_fs_write_read_roundtrip(void **state)
{
    (void)state;
    assert_true(eval_bool(
        "local fs = require('fs')\n"
        "local path = os.tmpname()\n"
        "assert(fs.writeFileSync(path, 'luna-fs-roundtrip'))\n"
        "assert(fs.readFileSync(path) == 'luna-fs-roundtrip')\n"
        "os.remove(path)\n"
        "return true"));
}

static void test_fs_append_mkdir_roundtrip(void **state)
{
    (void)state;
    assert_true(eval_bool(
        "local fs = require('fs')\n"
        "local base = os.tmpname()\n"
        "os.remove(base) -- want the name, not the file\n"
        "assert(fs.mkdirSync(base .. '/a/b', true))\n"
        "assert(fs.isDirectory(base .. '/a/b'))\n"
        "assert(fs.mkdirSync(base .. '/a/b', true)) -- idempotent when recursive\n"
        "fs.writeFileSync(base .. '/a/log.txt', 'x')\n"
        "assert(fs.appendFileSync(base .. '/a/log.txt', 'one;'))\n"
        "assert(fs.appendFileSync(base .. '/a/log.txt', 'two'))\n"
        "assert(fs.readFileSync(base .. '/a/log.txt') == 'xone;two')\n"
        "os.remove(base .. '/a/log.txt')\n"
        "fs.rmdir(base .. '/a/b')\n"
        "fs.rmdir(base .. '/a')\n"
        "fs.rmdir(base)\n"
        "return true"));
}

static void test_fs_readdir_sort_and_error_paths(void **state)
{
    (void)state;
    assert_true(eval_bool(
        "local fs = require('fs')\n"
        "local base = os.tmpname()\n"
        "os.remove(base)\n"
        "fs.mkdirSync(base .. '/zdir', true)\n"
        "fs.mkdirSync(base .. '/adir', true)\n"
        "fs.writeFileSync(base .. '/mfile', '')\n"
        "local names = fs.readdirSync(base)\n"
        "assert(#names == 3 and names[1] == 'adir' and names[2] == 'mfile' and names[3] == 'zdir')\n"
        "local okr, errr = pcall(fs.readdirSync, base .. '/nope')\n"
        "assert(not okr and tostring(errr):find('cannot open', 1, true) ~= nil)\n"
        "local oka, erra = pcall(fs.appendFileSync, base .. '/nope/x', 'data')\n"
        "assert(not oka and tostring(erra):find('cannot open', 1, true) ~= nil)\n"
        "local okm, errm = pcall(fs.mkdirSync, base .. '/adir')\n"
        "assert(not okm and tostring(errm):find('already exists', 1, true) ~= nil)\n"
        "assert(not pcall(fs.mkdirSync, base .. '/q/r/s')) -- no parents, not recursive\n"
        "os.remove(base .. '/mfile')\n"
        "fs.rmdir(base .. '/adir')\n"
        "fs.rmdir(base .. '/zdir')\n"
        "fs.rmdir(base)\n"
        "return true"));
}

static void test_zlib_one_shot_roundtrip(void **state)
{
    (void)state;
    assert_true(eval_bool(
        "local zlib = require('zlib')\n"
        "local text = ('luna-luna-luna-'):rep(100)\n"
        "local packed = zlib.compress(text)\n"
        "assert(#packed < #text)\n"
        "assert(zlib.decompress(packed) == text)\n"
        "return true"));
}

static void test_net_and_http_expose_functions(void **state)
{
    (void)state;
    assert_true(eval_bool(
        "local net = require('net')\n"
        "local http = require('http')\n"
        "assert(type(net.tcp) == 'function' and type(net.udp) == 'function')\n"
        "assert(type(net.connect) == 'function' and type(net.bind) == 'function')\n"
        "assert(type(http.request) == 'function')\n"
        "local socket = require('socket')\n"
        "assert(type(socket.tcp) == 'function')\n"
        "return true"));
}

#ifdef LUNA_HAVE_OPENSSL
static void test_crypto_known_digest_vector(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("return require('crypto').sha256('abc')"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

static void test_crypto_hmac_and_random(void **state)
{
    (void)state;
    assert_true(eval_bool(
        "local crypto = require('crypto')\n"
        "assert(crypto.hmac('sha256', 'key', 'msg') == "
        "crypto.hmac('sha256', 'key', 'msg'))\n"
        "assert(crypto.hmac('sha256', 'k1', 'm') ~= crypto.hmac('sha256', 'k2', 'm'))\n"
        "assert(#crypto.randombytes(16) == 16)\n"
        "return true"));
}
#else
static void test_crypto_fails_with_guidance(void **state)
{
    (void)state;
    assert_true(eval_bool(
        "local ok, err = pcall(require, 'crypto')\n"
        "return ok == false and tostring(err):find('OpenSSL', 1, true) ~= nil"));
}
#endif

/* -- runner ---------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_bare_module_from_requiring_file_dir, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_manifest_main_picks_entry, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_bare_lookup_walks_up_directories, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_relative_require_resolves_to_caller_dir, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_subpath_resolves_inside_package, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_subpath_walks_up_directories, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_missing_subpath_still_falls_through, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_loaded_caches_across_requires, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_missing_module_reports_clearly, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_json_roundtrip_and_node_aliases, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_fs_reads_and_attributes, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_fs_write_read_roundtrip, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_fs_append_mkdir_roundtrip, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_fs_readdir_sort_and_error_paths, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_zlib_one_shot_roundtrip, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_net_and_http_expose_functions, setup_modules, teardown_modules),
#ifdef LUNA_HAVE_OPENSSL
        cmocka_unit_test_setup_teardown(test_crypto_known_digest_vector, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_crypto_hmac_and_random, setup_modules, teardown_modules),
#else
        cmocka_unit_test_setup_teardown(test_crypto_fails_with_guidance, setup_modules, teardown_modules),
#endif
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
