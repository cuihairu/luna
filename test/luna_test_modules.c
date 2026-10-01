/* Module system: Node-style require resolution (bare names walking up
 * luna_modules/, package.json manifests, relative paths, package.loaded
 * caching) and the stdlib modules backed by the bundled libraries.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "luna_lua.h" /* embedded luna.modules source */
#include "luna_cov.h"

int luaopen_lpeg(lua_State *L);
int luaopen_lfs(lua_State *L);
int luaopen_socket_core(lua_State *L);
int luaopen_mime_core(lua_State *L);
int luaopen_zlib(lua_State *L);
int luaopen_toml_core(lua_State *L);
int luaopen_yaml(lua_State *L);
int luaopen_lxp(lua_State *L);
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
    luaL_requiref(L, "lpeg", luaopen_lpeg, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "lfs", luaopen_lfs, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "socket.core", luaopen_socket_core, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "mime.core", luaopen_mime_core, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "zlib.core", luaopen_zlib, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "toml.core", luaopen_toml_core, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "yaml.core", luaopen_yaml, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "lxp", luaopen_lxp, 0);
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
    lua_pushlstring(L, LUNA_LUA_ROCKS, sizeof(LUNA_LUA_ROCKS) - 1);
    lua_setglobal(L, "__LUNA_ROCKS_SRC");
    run(L,
        "package.preload['luna.modules'] = assert(load(__LUNA_MODULES_SRC, luna_chunkname('modules')))\n"
        "package.preload['luna.rocks'] = assert(load(__LUNA_ROCKS_SRC, luna_chunkname('rocks')))\n"
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

/* -- round-8 corner sweep: manifest "main" extension variants, the
 * relative init.lua convention, idempotent install, and the
 * working-directory roots -- */

static void test_install_twice_reports_already_there(void **state)
{
    (void)state;
    /* the searcher is in place: a second install says so */
    assert_false(eval_bool("return M.install()"));
}

static void test_relative_init_convention_and_miss(void **state)
{
    (void)state;
    /* "./reldir" has no reldir.lua: the init.lua convention answers */
    assert_string_equal(
        eval_string("local _, path = M.resolve_relative('./reldir', '"
                    FIXTURES "/myapp')\n"
                    "return tostring(path):match('reldir/init%.lua$')"
                    "~= nil and 'init' or 'no'"),
        "init");
    /* nothing at all: nil lets the searcher chain carry on */
    assert_false(eval_bool(
        "return M.resolve_relative('./no-such-rel-zz', '" FIXTURES
        "/myapp') ~= nil"));
}

static void test_unloadable_entry_and_empty_name_fall_through(void **state)
{
    (void)state;
    /* a manifest main that cannot even be compiled: load_entry hands
     * back nil and the bare walk comes up empty instead of erroring */
    assert_false(eval_bool(
        "return M.resolve_bare('mainbad', '" FIXTURES "/myapp') ~= nil"));
    /* an empty module name: the searcher declines it outright */
    assert_false(eval_bool("return M.searcher('') ~= nil"));
}

static void test_manifest_main_without_lua_suffix_variants(void **state)
{
    (void)state;
    /* "main": "lib" — lib.lua exists and wins */
    assert_true(eval_bool(
        "local loader, path = M.resolve_bare('mainfile', '" FIXTURES
        "/myapp')\n"
        "return path ~= nil and path:find('mainfile/lib%.lua$') ~= nil"));
    /* "main": "sub" — no sub.lua, so sub/init.lua answers */
    assert_true(eval_bool(
        "local loader, path = M.resolve_bare('maindir', '" FIXTURES
        "/myapp')\n"
        "return path ~= nil and path:find('maindir/sub/init%.lua$') ~= nil"));
    /* a manifest without a usable main and no init.lua: the package
     * entry is nil and the bare walk carries on (and comes up empty) */
    assert_false(eval_bool(
        "return M.resolve_bare('mainvoid', '" FIXTURES "/myapp') ~= nil"));
}

static void test_relative_source_caller_absolutizes_via_lfs(void **state)
{
    (void)state;
    /* loading by a RELATIVE path gives the chunk source
     * '@relapp/main.lua'; caller_dir absolutizes it through lfs and
     * the bare lookup finds flatmod one level up */
    char cwd[512];
    assert_non_null(getcwd(cwd, sizeof cwd));
    assert_int_equal(chdir(FIXTURES), 0);
    if (luaL_loadfilex(L, "myapp/relapp/main.lua", NULL) != LUA_OK ||
        lua_pcall(L, 0, 1, 0) != LUA_OK) {
        chdir(cwd);
        fail_msg("relapp chunk failed: %s", lua_tostring(L, -1));
    }
    const char *r = lua_tostring(L, -1);
    assert_string_equal(r ? r : "", "flat-ok");
    lua_pop(L, 1);
    assert_int_equal(chdir(cwd), 0);
}

static void test_start_dir_falls_back_without_lfs(void **state)
{
    (void)state;
    /* with lfs out of the picture, a file-less caller's upward walk
     * roots at "." and the require reports the usual not-found */
    assert_true(eval_bool(
        "local saved = package.loaded.lfs\n"
        "package.loaded.lfs = nil\n"
        "package.preload.lfs = function() error('stub: lfs away') end\n"
        "local ok, err = pcall(require, 'no-such-mod-zz')\n"
        "package.preload.lfs = nil\n"
        "package.loaded.lfs = saved\n"
        "return ok == false and tostring(err):find('no-such-mod-zz', 1, true) ~= nil"));
}

/* luna.rocks' own load-time and vendor guards, driven from this VM
 * where the kernel module (which hands the real paths over) is absent */

static void test_rocks_module_needs_dkjson(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "package.loaded['luna.rocks'] = nil\n"
        "local saved_pre = package.preload['dkjson']\n"
        "local saved_loaded = package.loaded['dkjson']\n"
        "package.loaded['dkjson'] = nil\n"
        "package.preload['dkjson'] = function() error('stub: dkjson away') end\n"
        "local ok, err = pcall(require, 'luna.rocks')\n"
        "package.preload['dkjson'] = saved_pre\n"
        "package.loaded['dkjson'] = saved_loaded\n"
        "return tostring(ok) .. ',' ..\n"
        "    tostring(tostring(err):find('needs dkjson', 1, true) ~= nil)"),
        "false,true");
}

static void test_rocks_vendor_legs_die_before_luarocks(void **state)
{
    (void)state;
    /* empty LUNA_VENDOR_DIR reads as unset; a bogus one fails the
     * vendored-sources check — both must die before any luarocks code
     * loads. os.exit is stubbed so the VM survives the exit(1). */
    setenv("LUNA_VENDOR_DIR", "", 1);
    assert_string_equal(eval_string(
        "local rocks = require('luna.rocks')\n"
        "local code, real = nil, os.exit\n"
        "os.exit = function(c) code = c or 0 error('OS_EXIT') end\n"
        "local ok = pcall(rocks.dispatch, {'list'})\n"
        "os.exit = real\n"
        "return tostring(ok) .. ',' .. tostring(code)"),
        "false,1");
    setenv("LUNA_VENDOR_DIR", "/nonexistent-luna-vendor", 1);
    assert_string_equal(eval_string(
        "local code, real = nil, os.exit\n"
        "os.exit = function(c) code = c or 0 error('OS_EXIT') end\n"
        "local ok = pcall(require('luna.rocks').dispatch, {'list'})\n"
        "os.exit = real\n"
        "return tostring(ok) .. ',' .. tostring(code)"),
        "false,1");
    unsetenv("LUNA_VENDOR_DIR");
    /* a command outside the four names returns 1 without exiting or
     * touching luarocks — the luna entry's ROCKS_CMDS gate means this
     * leg is only reachable through a direct dispatch call */
    assert_string_equal(eval_string(
        "local ok, rc = pcall(require('luna.rocks').dispatch, {'frobnicate'})\n"
        "return tostring(ok) .. ',' .. tostring(rc)"),
        "true,1");
}

/* -- csv / ini format modules (batch 2, LPeg) ------------------------------ */

static void test_csv_decode_basics_and_eols(void **state)
{
    (void)state;
    /* LF, CRLF and mixed all decode; fields stay strings, never trimmed */
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local enc = require('json').encode\n"
                    "assert(enc(csv.decode('a,b\\nc,d')) == '[[\"a\",\"b\"],[\"c\",\"d\"]]')\n"
                    "assert(enc(csv.decode('a,b\\r\\nc,d\\r\\n')) == enc(csv.decode('a,b\\nc,d')))\n"
                    "assert(enc(csv.decode(' sp ,kept ')) == '[[\" sp \",\"kept \"]]')\n"
                    "assert(enc(csv.decode('')) == '[]')\n"
                    "return 'ok'"),
        "ok");
    /* edges: no phantom record after a trailing newline; a blank line is
     * an empty record; a trailing delimiter yields an empty last field */
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local enc = require('json').encode\n"
                    "assert(#csv.decode('a\\n') == 1)\n"
                    "assert(enc(csv.decode('a\\n\\n')) == '[[\"a\"],[\"\"]]')\n"
                    "assert(enc(csv.decode('a,')) == '[[\"a\",\"\"]]')\n"
                    "assert(enc(csv.decode('a,,b')) == '[[\"a\",\"\",\"b\"]]')\n"
                    "return 'ok'"),
        "ok");
    /* a lone CR is not an EOL — rejected with its exact position */
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local t, e = csv.decode('a\\rb')\n"
                    "return tostring(t) .. ' | ' .. e"),
        "nil | csv: unexpected character after field at line 1, column 2");
}

static void test_csv_quoted_fields_rfc4180(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local enc = require('json').encode\n"
                    /* quoted comma, doubled quote, embedded newline */
                    "assert(enc(csv.decode('\"a,b\",c')) == '[[\"a,b\",\"c\"]]')\n"
                    "assert(csv.decode('\"x\"\"y\"')[1][1] == 'x\"y')\n"
                    "assert(csv.decode('\"line1\\nline2\",z')[1][1] == 'line1\\nline2')\n"
                    "assert(enc(csv.decode('\"\"')) == '[[\"\"]]')\n"
                    "return 'ok'"),
        "ok");
    /* unterminated quote is reported where the closing quote was expected */
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local t, e = csv.decode('a,\"b')\n"
                    "return tostring(t) .. ' | ' .. e"),
        "nil | csv: unterminated quoted field at line 1, column 5");
    /* same case on line 2: position must track lines (UTF-8 columns) */
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local t, e = csv.decode('a,b\\n\\u{00E9},\"x')\n"
                    "return e"),
        "csv: unterminated quoted field at line 2, column 5");
    /* stray character after a properly closed quoted field */
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local t, e = csv.decode('a,\"b\"x')\n"
                    "return tostring(t) .. ' | ' .. e"),
        "nil | csv: unexpected character after field at line 1, column 6");
}

static void test_csv_headers_and_delimiter(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local rows = csv.decode('name,age\\nbob,3\\n,4', {headers = true})\n"
                    "assert(rows[1].name == 'bob' and rows[1].age == '3')\n"
                    "assert(rows[2].name == '' and rows[2].age == '4')\n"
                    "return 'ok'"),
        "ok");
    /* duplicate header names: last one wins; extra columns stay positional */
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local rows = csv.decode('a,b,a\\n1,2,3,4', {headers = true})\n"
                    "assert(rows[1].a == '3' and rows[1].b == '2' and rows[1][1] == '4')\n"
                    "return 'ok'"),
        "ok");
    /* any single-byte delimiter except quote/CR/LF; bad ones raise */
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local enc = require('json').encode\n"
                    "assert(enc(csv.decode('a;b\\nc;d', {delimiter = ';'})) == '[[\"a\",\"b\"],[\"c\",\"d\"]]')\n"
                    "local ok, e = pcall(csv.decode, 'a', {delimiter = '\"'})\n"
                    "assert(not ok and e:find('delimiter', 1, true))\n"
                    "return 'ok'"),
        "ok");
}

static void test_csv_encode_basics(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    /* rows are joined by eol (CRLF default); no trailing eol;
                     * quote only when a field needs it */
                    "assert(csv.encode({{'a,b', 'c\"d'}}) == '\"a,b\",\"c\"\"d\"')\n"
                    "assert(csv.encode({{'plain'}}) == 'plain')\n"
                    "assert(csv.encode({{'a'},{'b'}}) == 'a\\r\\nb')\n"
                    "assert(csv.encode({}, {eol = '\\n'}) == '')\n"
                    "assert(csv.encode({{'a'}}, {eol = '\\n'}) == 'a')\n"
                    /* numbers/booleans stringify */
                    "assert(csv.encode({{1, true}}, {eol = '\\n'}) == '1,true')\n"
                    "return 'ok'"),
        "ok");
    /* unsupported field type: nil, err without raising */
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local s, e = csv.encode({{function() end}})\n"
                    "return tostring(s) .. ' | ' .. (e:match('unsupported field type (%a+)'))"),
        "nil | function");
    /* non-table rows rejected; row type is named in the message */
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local s, e = csv.encode({'oops'})\n"
                    "return tostring(s) .. ' | ' .. e"),
        "nil | csv: row 1 is a string, expected a table");
}

static void test_csv_encode_headers_mapping(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local out = csv.encode(\n"
                    "    {{name = 'bob', age = 3}, {name = 'eve', age = 4}},\n"
                    "    {headers = {'name', 'age'}, eol = '\\n'})\n"
                    "assert(out == 'name,age\\nbob,3\\neve,4')\n"
                    "return 'ok'"),
        "ok");
    /* array-shaped rows pass through under the same header option */
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local out = csv.encode({{'x', 'y'}, {'z'}},\n"
                    "    {headers = {'h1', 'h2'}, eol = '\\n'})\n"
                    "assert(out == 'h1,h2\\nx,y\\nz')\n"
                    "return 'ok'"),
        "ok");
    /* headers = true has no defensible meaning (table order) — it raises */
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local ok, e = pcall(csv.encode, {{}}, {headers = true})\n"
                    "return tostring(ok) .. ' | ' .. tostring(e:find('explicit array', 1, true) ~= nil)"),
        "false | true");
}

static void test_csv_roundtrip_and_lines(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local enc = require('json').encode\n"
                    "local rows = {{'a,b', 'c\"d', 'e'}, {'', 'x'}}\n"
                    "local back = csv.decode(csv.encode(rows))\n"
                    "assert(enc(back) == enc(rows))\n"
                    "return 'ok'"),
        "ok");
    /* lines(): one record per call, quoted newlines stay inside the field */
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local enc = require('json').encode\n"
                    "local got = {}\n"
                    "for row in csv.lines('h1\\n\"a\\nb\",2\\nlast') do got[#got + 1] = row end\n"
                    "assert(#got == 3 and got[1][1] == 'h1')\n"
                    "assert(got[2][1] == 'a\\nb' and got[2][2] == '2')\n"
                    "assert(got[3][1] == 'last')\n"
                    "return 'ok'"),
        "ok");
    /* lines() raises on malformed input (an iterator's nil means "done") */
    assert_string_equal(
        eval_string("local csv = require('csv')\n"
                    "local it = csv.lines('a,\"broken')\n"
                    "local ok, e = pcall(it)\n"
                    "return tostring(ok) .. ' | ' .. e"),
        "false | csv: unterminated quoted field at line 1, column 10");
}

static void test_ini_decode_basics(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local t = ini.decode([[\n"
                    "# full-line comment\n"
                    "root = before any section\n"
                    "; semicolon comments too\n"
                    "\n"
                    "[server]\n"
                    "host = example.org   \n"
                    "port = 8080\n"
                    "]])\n"
                    "assert(t.root == 'before any section')\n"
                    "assert(t.server.host == 'example.org')\n"
                    "assert(t.server.port == '8080')\n"
                    "assert(t.server.host2 == nil)\n"
                    "return 'ok'"),
        "ok");
    /* values are strings unless cast; comments never strip mid-value '#' */
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local t = ini.decode('path = /tmp/x#not-a-comment\\nflag = ; also value')\n"
                    "assert(t.path == '/tmp/x#not-a-comment')\n"
                    "assert(t.flag == '; also value')\n"
                    "return 'ok'"),
        "ok");
    /* blank values, empty sections, lone-CR leniency */
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local t = ini.decode('empty =\\n[nothing]\\r\\nnext = 1')\n"
                    "assert(t.empty == '')\n"
                    "assert(t.nothing.next == '1')\n"
                    "return 'ok'"),
        "ok");
}

static void test_ini_quoted_values(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local t = ini.decode(\n"
                    "    'q = \"a b\"\\n'\n"
                    "    .. 'esc = \"x\\\\\"y\\\\\\\\z\"\\n'\n"
                    "    .. 'empty = \"\"\\n')\n"
                    "assert(t.q == 'a b')\n"
                    "assert(t.esc == 'x\"y\\\\z')\n"
                    "assert(t.empty == '')\n"
                    "return 'ok'"),
        "ok");
    /* unterminated quote reported where the close was expected */
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local t, e = ini.decode('a = \"bcd')\n"
                    "return tostring(t) .. ' | ' .. e"),
        "nil | ini: unterminated quoted value at line 1, column 9");
    /* data after a closed quoted value */
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local t, e = ini.decode('a = \"b\" x')\n"
                    "return e"),
        "ini: unexpected data after quoted value at line 1, column 9");
}

static void test_ini_duplicates_and_cast(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local t = ini.decode('k = first\\nk = second\\n[s]\\nx = 1\\n[s]\\ny = 2')\n"
                    "assert(t.k == 'second')\n"
                    "assert(t.s.x == '1' and t.s.y == '2')\n"
                    "return 'ok'"),
        "ok");
    /* cast converts tonumber-able values and lowercase true/false */
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local t = ini.decode('n = 42\\nf = 1.5\\nhex = 0x10\\nb = true\\nB = false\\n[s]\\ninner = 7',\n"
                    "                   {cast = true})\n"
                    "assert(t.n == 42 and t.f == 1.5 and t.hex == 16)\n"
                    "assert(t.b == true and t.B == false)\n"
                    "assert(t.s.inner == 7)\n"
                    "return 'ok'"),
        "ok");
    /* without cast everything stays a string */
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local t = ini.decode('n = 42')\n"
                    "assert(t.n == '42')\n"
                    "return 'ok'"),
        "ok");
}

static void test_ini_error_shapes(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local t, e = ini.decode('[sect\\n')\n"
                    "return tostring(t) .. ' | ' .. e"),
        "nil | ini: section header missing ']' at line 1, column 6");
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local t, e = ini.decode('justkey')\n"
                    "return e"),
        "ini: expected '[section]' or 'key = value' at line 1, column 1");
    /* inline content after a section header is not a comment here */
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local t, e = ini.decode('[a] ; trailing')\n"
                    "return e"),
        "ini: unexpected data after section header at line 1, column 5");
    /* errors carry line numbers across the document */
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local t, e = ini.decode('a = 1\\nb = 2\\nbroken line')\n"
                    "return e"),
        "ini: expected '[section]' or 'key = value' at line 3, column 1");
}

static void test_ini_encode_and_roundtrip(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local out = ini.encode({top = 'plain', keep = ' has spaces ',\n"
                    "                        q = 'a\"b', sec = {a = '1', b = 'true'}})\n"
                    "assert(out:find('top = plain', 1, true))\n"
                    "assert(out:find('keep = \" has spaces \"', 1, true))\n"
                    "assert(out:find('q = \"a\\\\\"b\"', 1, true))\n"
                    "assert(out:find('%[sec%]'))\n"
                    "assert(out:find('a = 1'))\n"
                    "assert(ini.encode({}) == '')\n"
                    "local flat = ini.encode({x = 1, y = true})\n"
                    "assert(flat == 'x = 1\\ny = true' or flat == 'y = true\\nx = 1')\n"
                    "return 'ok'"),
        "ok");
    /* roundtrip: decode(encode(t)) returns an equal table */
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local enc = require('json').encode\n"
                    "local t = {root = 'a', s = {x = '1', y = 'z z'}, t2 = {k = ''}}\n"
                    "local back = ini.decode(ini.encode(t))\n"
                    "assert(back.root == 'a' and back.s.x == '1')\n"
                    "assert(back.s.y == 'z z' and back.t2.k == '')\n"
                    "return 'ok'"),
        "ok");
    /* bad keys, nested sections and unsupported values report nil, err */
    assert_string_equal(
        eval_string("local ini = require('ini')\n"
                    "local s1 = ini.encode({['k=x'] = 'v'})\n"
                    "local s2 = ini.encode({s = {inner = {deep = 1}}})\n"
                    "local s3 = ini.encode({f = function() end})\n"
                    "return tostring(s1) .. tostring(s2) .. tostring(s3 ~= nil)"),
        "nilnilfalse");
}

/* -- toml module (batch 3, tomlc17 R260821 + wrapper encode) --------------- */

static void test_toml_decode_scalars(void **state)
{
    (void)state;
    /* basic-string escapes, literal strings, all integer spellings
     * including both int64 bounds, floats incl inf/nan, booleans */
    assert_string_equal(
        eval_string("local t = require('toml').decode([==[\n"
                    "s = \"a\\tb\\né 😀 \\\"q\\\"\"\n"
                    "l = 'C:\\raw\\no\\escape'\n"
                    "hex = 0xDEADBEEF\n"
                    "oct = 0o755\n"
                    "bin = 0b1010\n"
                    "grouped = 1_000_000\n"
                    "max = 9223372036854775807\n"
                    "min = -9223372036854775808\n"
                    "f = [inf, -inf, 6.626e-34, -0.0, 3.0]\n"
                    "b = [true, false]\n"
                    "]==])\n"
                    "assert(t.s == 'a\\tb\\né 😀 \\\"q\\\"')\n"
                    "assert(t.l == 'C:\\\\raw\\\\no\\\\escape')\n"
                    "assert(t.hex == 0xDEADBEEF and t.oct == 493 and t.bin == 10)\n"
                    "assert(t.grouped == 1000000 and math.type(t.grouped) == 'integer')\n"
                    "assert(t.max == math.maxinteger and t.min == math.mininteger)\n"
                    "assert(t.f[1] == math.huge and t.f[2] == -math.huge)\n"
                    "assert(t.f[3] == 6.626e-34 and t.f[4] == 0.0 and 1 / t.f[4] < 0)\n"
                    "assert(math.type(t.f[5]) == 'float' and t.f[5] == 3.0)\n"
                    "assert(t.b[1] == true and t.b[2] == false)\n"
                    "return 'ok'"),
        "ok");
}

static void test_toml_decode_multiline_and_unicode(void **state)
{
    (void)state;
    /* multiline basic: the first newline after """ is trimmed, escapes
     * work, a line-ending backslash swallows the following whitespace */
    assert_string_equal(
        eval_string("local t = require('toml').decode([==[\n"
                    "one = \"\"\"\n"
                    "line one\n"
                    "line two \\n third\"\"\"\n"
                    "two = \"\"\"\\\n"
                    "   joined\\\n"
                    "   lines\"\"\"\n"
                    "lit = '''\n"
                    "raw \\n stays'''\n"
                    "]==])\n"
                    "assert(t.one == 'line one\\nline two \\n third')\n"
                    "assert(t.two == 'joinedlines')\n"
                    "assert(t.lit == 'raw \\\\n stays')\n"
                    "return 'ok'"),
        "ok");
    /* \uXXXX and \UXXXXXXXX escapes land as real UTF-8 */
    assert_string_equal(
        eval_string("local t = require('toml').decode([=[\n"
                    "u = \"\\u00E9 \\U0001F600 \\u4E2D\\u6587\"\n"
                    "]=])\n"
                    "assert(t.u == 'é 😀 中文')\n"
                    "return 'ok'"),
        "ok");
}

static void test_toml_decode_structures(void **state)
{
    (void)state;
    /* [table], [[array of tables]], dotted keys, inline tables,
     * nested arrays — toml-test selections */
    assert_string_equal(
        eval_string("local t = require('toml').decode([==[\n"
                    "top = 1\n"
                    "[server]\n"
                    "host = 'example.org'\n"
                    "  [server.tls]\n"
                    "enabled = true\n"
                    "[[fruit]]\n"
                    "name = 'apple'\n"
                    "  [[fruit.physical]]\n"
                    "  color = 'red'\n"
                    "[[fruit]]\n"
                    "name = 'banana'\n"
                    "dotted.key.deep = 5\n"
                    "inline = { a = 1, sub = { b = 'x' } }\n"
                    "matrix = [[1, 2], [3, 4], []]\n"
                    "]==])\n"
                    "assert(t.top == 1)\n"
                    "assert(t.server.host == 'example.org' and t.server.tls.enabled == true)\n"
                    "assert(#t.fruit == 2 and t.fruit[1].name == 'apple')\n"
                    "assert(t.fruit[1].physical[1].color == 'red')\n"
                    "assert(t.fruit[2].name == 'banana')\n"
                    "assert(t.fruit[2].dotted.key.deep == 5)\n"
                    "assert(t.fruit[2].inline.a == 1 and t.fruit[2].inline.sub.b == 'x')\n"
                    "local m = t.fruit[2].matrix\n"
                    "assert(#m == 3 and m[1][2] == 2 and m[2][1] == 3 and #m[3] == 0)\n"
                    "return 'ok'"),
        "ok");
    /* empty document decodes to an empty table; duplicate keys and
     * extending an inline table are rejected with the line number */
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "local t = toml.decode('')\n"
                    "assert(type(t) == 'table' and next(t) == nil)\n"
                    "local d = toml.decode('a = 1\\na = 2')\n"
                    "assert(d == nil)\n"
                    "return 'ok'"),
        "ok");
}

static void test_toml_decode_datetimes(void **state)
{
    (void)state;
    /* local date / time / datetime / offset datetime -> component
     * tables; secfrac only when present (as a fraction); offset in
     * minutes, 0 for Z */
    assert_string_equal(
        eval_string("local t = require('toml').decode([==[\n"
                    "d = 1979-05-27\n"
                    "tm = 07:32:00\n"
                    "tmf = 00:32:00.999999\n"
                    "dt = 1979-05-27T07:32:00Z\n"
                    "dtz = 1979-05-27T00:32:00.999999-07:00\n"
                    "space = 1979-05-27 07:32:00\n"
                    "]==])\n"
                    "assert(t.d.year == 1979 and t.d.month == 5 and t.d.day == 27)\n"
                    "assert(t.d.hour == nil and t.d.offset == nil)\n"
                    "assert(t.tm.hour == 7 and t.tm.minute == 32 and t.tm.second == 0)\n"
                    "assert(t.tm.secfrac == nil and t.tm.year == nil)\n"
                    "assert(t.tmf.secfrac == 0.999999)\n"
                    "assert(t.dt.year == 1979 and t.dt.hour == 7 and t.dt.offset == 0)\n"
                    "assert(t.dtz.secfrac == 0.999999 and t.dtz.offset == -420)\n"
                    "assert(t.space.hour == 7 and t.space.offset == nil)\n"
                    "return 'ok'"),
        "ok");
}

static void test_toml_decode_errors(void **state)
{
    (void)state;
    /* parse errors carry the line; the column clause does not apply —
     * tomlc17 reports line numbers only (guide/modules.md) */
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "local t1, e1 = toml.decode('a = 1\\na = 2')\n"
                    "local t2, e2 = toml.decode('x = \"unterminated')\n"
                    "local t3, e3 = toml.decode('ok = 1\\nbad = 2 2')\n"
                    "return e1 .. ' | ' .. e2 .. ' | ' .. e3"),
        "toml: duplicate key at line 2 | toml: unterminated string at line 1"
        " | toml: ENDL expected at line 2");
    /* int64 overflow is rejected with the line, not silently truncated
     * (toml-test's integer-bounds selections) */
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "local t, e = toml.decode('over = 9223372036854775808')\n"
                    "return tostring(t) .. ' | ' .. e"),
        "nil | toml: error parsing integer at line 1");
    /* invalid UTF-8 is rejected (TOML 1.0 requires valid UTF-8) */
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "local t, e = toml.decode('k = \\255\\254 bad')\n"
                    "return tostring(t) .. ' | ' .. e"),
        "nil | toml: invalid UTF8 char at line 1");
    /* surrogate escapes are rejected inside the string pass too */
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "local t, e = toml.decode('k = \"\\\\uD800\"')\n"
                    "return tostring(t) .. ' | ' .. e"),
        "nil | toml: invalid UTF8 char \\ud800 at line 1");
}

static void test_toml_encode_exact_shapes(void **state)
{
    (void)state;
    /* scalar-first: key=value lines precede [headers] in every table;
     * blank line before each header, none trailing */
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "return toml.encode({only = 1, sub = {b = 2}})"),
        "only = 1\n\n[sub]\nb = 2");
    /* array of plain tables -> [[header]] per element */
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "return toml.encode({items = {{k = 1}, {k = 2}}})"),
        "[[items]]\nk = 1\n\n[[items]]\nk = 2");
    /* nested sections carry the full dotted path in every header: the
     * child-path helper is the difference between [a.mid.leaf] and a
     * flattened [leaf] (pinned after a {table.unpack(path), fk} table
     * constructor truncated the path and broke depth-2+ nesting) */
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "return toml.encode({a = {mid = {leaf = {x = 1},"
                    " keep = 's'}}})"),
        "[a]\n\n[a.mid]\nkeep = \"s\"\n\n[a.mid.leaf]\nx = 1");
    /* mixed/scalar arrays, datetimes and empty tables render inline;
     * one key per table — pairs order must not decide the expected text */
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "return toml.encode({arr = {1, 'a b', true}})"),
        "arr = [1, \"a b\", true]");
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "return toml.encode({dt = {hour = 7, minute = 32,"
                    " second = 0, secfrac = 0.5}})"),
        "dt = 07:32:00.5");
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "return toml.encode({dt = {year = 1979, month = 5,"
                    " day = 27, hour = 7, minute = 32, second = 0,"
                    " offset = 450}})"),
        "dt = 1979-05-27T07:32:00+07:30");
    /* nan/inf/-inf spellings; offset 0 renders as Z */
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "return toml.encode({n = 0/0})"),
        "n = nan");
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "return toml.encode({n = -math.huge})"),
        "n = -inf");
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "return toml.encode({dt = {year = 1979, month = 5,"
                    " day = 27, hour = 7, minute = 32, second = 0,"
                    " offset = 0}})"),
        "dt = 1979-05-27T07:32:00Z");
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "return toml.encode({e = {}})"),
        "e = {}");
    /* keys needing quotes; control bytes escape as \\uXXXX */
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "return toml.encode({['k.y'] = 'v'})"),
        "\"k.y\" = \"v\"");
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "return toml.encode({s = 'a\\0b'})"),
        "s = \"a\\u0000b\"");
}

static void test_toml_encode_roundtrip(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "local function eq(a, b)\n"
                    "  if a == b then return true end\n"
                    "  if type(a) ~= 'table' or type(b) ~= 'table' then return false end\n"
                    "  local n = 0\n"
                    "  for k, v in pairs(a) do\n"
                    "    if not eq(v, b[k]) then return false end\n"
                    "    n = n + 1\n"
                    "  end\n"
                    "  for _ in pairs(b) do n = n - 1 end\n"
                    "  return n == 0\n"
                    "end\n"
                    "local t = {\n"
                    "  title = 'round trip', esc = 'a\\ttab \\\\ \"q\" \\u{7F}',\n"
                    "  n = {i = 42, big = math.maxinteger, f = 0.1, third = 1/3,\n"
                    "       one = 1.0, inf = math.huge, negz = -0.0},\n"
                    "  when = {year = 1979, month = 5, day = 27, hour = 7,\n"
                    "         minute = 32, second = 0, secfrac = 0.5, offset = -420},\n"
                    "  day = {year = 2026, month = 9, day = 30},\n"
                    "  sub = {deep = {x = 'y'}},\n"
                    "  aot = {{k = 1, vals = {1, 2}}, {k = 2, vals = {}}},\n"
                    "}\n"
                    "local s = toml.encode(t)\n"
                    "assert(toml.decode(s))\n"
                    "assert(eq(t, toml.decode(s)), 'roundtrip mismatch')\n"
                    "return 'ok'"),
        "ok");
}

static void test_toml_encode_errors(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "local a = toml.encode({f = function() end})\n"
                    "local b = toml.encode({d = {year = 2020, month = 13, day = 1}})\n"
                    "local c = toml.encode({d = {hour = 1, offset = 30}})\n"
                    "return tostring(a) .. ' | ' .. tostring(b) .. ' | ' .. tostring(c)"),
        "nil | nil | nil");
    /* datetime component validation: out-of-range secfrac, offset on a
     * non-datetime shape (needs a COMPLETE time or the hour/minute/second
     * check fires first), offset beyond +/-23:59 */
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "local a = toml.encode({d = {year = 2020, month = 5, day = 1,"
                    " hour = 1, minute = 2, second = 3, secfrac = 1.5}})\n"
                    "local b = toml.encode({d = {hour = 1, minute = 2, second = 3,"
                    " offset = 30}})\n"
                    "local c = toml.encode({d = {year = 2020, month = 5, day = 1,"
                    " hour = 1, minute = 2, second = 3, offset = 99999}})\n"
                    "return tostring(a) .. ' | ' .. tostring(b) .. ' | ' .. tostring(c)"),
        "nil | nil | nil");
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "local cyc = {}\n"
                    "cyc.self = cyc\n"
                    "local _, e1 = toml.encode(cyc)\n"
                    "local _, e2 = toml.encode({[1] = 'positional'})\n"
                    "return tostring(e1) .. ' | ' .. tostring(e2)"),
        "toml: cyclic table reference | toml: key must be a string (got number)");
    /* bad arguments raise (shared contract: only bad DATA returns nil).
     * A NUMBER argument coerces the way string.format's %s does, then
     * still has to parse — "42" is not a TOML document — so it lands in
     * the nil, err bucket rather than raising. */
    assert_string_equal(
        eval_string("local toml = require('toml')\n"
                    "local ok1 = pcall(toml.decode, {})\n"
                    "local ok2 = pcall(toml.encode, 'str')\n"
                    "local c1 = toml.decode(42)\n"
                    "return tostring(ok1) .. ',' .. tostring(ok2) .. ','"
                    " .. tostring(c1)"),
        "false,false,nil");
}

/* -- yaml module (batch 4, libyaml 0.2.5 + lyaml 6.2.9 vendor) ------ */

static void test_yaml_decode_scalar_family(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local y = require('yaml')\n"
                    "local t = y.decode('ints: [42, -7, 0x1F]\\n"
                    "floats: [2.5, 6.02e23, .inf, .nan]\\n"
                    "bools: [true, FALSE, yes, off]\\n"
                    "nulls: [~, null]\\n"
                    "strs: [\\'123\\', \"123\", \\'yes\\']\\n')\n"
                    "assert(t.ints[1] == 42 and math.type(t.ints[1]) == 'integer')\n"
                    "assert(t.ints[2] == -7 and t.ints[3] == 31)\n"
                    "assert(math.type(t.ints[3]) == 'integer')\n"
                    "assert(t.floats[1] == 2.5 and math.type(t.floats[1]) == 'float')\n"
                    "assert(t.floats[2] == 6.02e23 and t.floats[3] == math.huge)\n"
                    "assert(t.floats[4] ~= t.floats[4]) -- .nan\n"
                    "assert(t.bools[1] == true and t.bools[2] == false) -- YAML 1.1: FALSE\n"
                    "assert(t.bools[3] == true and t.bools[4] == false) -- yes / off\n"
                    "assert(t.strs[1] == '123' and type(t.strs[1]) == 'string')\n"
                    "assert(t.strs[2] == '123' and t.strs[3] == 'yes')\n"
                    "assert(t.nulls ~= nil and next(t.nulls) == nil) -- null in a sequence\n"
                    "assert(y._VERSION:find('6.2.9', 1, true) ~= nil)\n"
                    "return 'ok'"),
        "ok");
}

static void test_yaml_decode_yaml11_numbers(void **state)
{
    (void)state;
    /* YAML 1.1-only number families: binary/octal/sexagesimal literals
     * and '_' digit grouping — lyaml's implicit resolvers, none of which
     * a plain tonumber would accept */
    assert_string_equal(
        eval_string("local t = require('yaml').decode("
                    "'nums: [0b1010, -0b11, 0b1010_1010, 017, -017, -0x1F]\\n"
                    "sex: [190:20:30, -190:20:30, 190:20:30.15,"
                    " -190:20:30.15]\\n"
                    "grouped: 1_000\\ninf: +.inf\\nnan: .NaN\\n')\n"
                    "assert(t.nums[1] == 10 and math.type(t.nums[1]) == 'integer')\n"
                    "assert(t.nums[2] == -3 and t.nums[3] == 170)\n"
                    "assert(t.nums[4] == 15 and t.nums[5] == -15) -- leading-0 octal\n"
                    "assert(t.nums[6] == -31) -- signed hexadecimal\n"
                    "assert(t.sex[1] == 685230 and t.sex[2] == -685230) -- base 60\n"
                    "assert(t.sex[3] == 685230.15 and math.type(t.sex[3]) == 'float')\n"
                    "assert(t.sex[4] == -685230.15) -- signed sexagesimal float\n"
                    "assert(t.grouped == 1000) -- grouped digits strip to 1_000\n"
                    "assert(t.inf == math.huge and t.nan ~= t.nan) -- spelling variants\n"
                    "return 'ok'"),
        "ok");
}

static void test_yaml_decode_explicit_tags(void **state)
{
    (void)state;
    /* explicit !!tags force the type regardless of the scalar's own
     * shape — quoted ints, bare y/n booleans, ints under !!float all
     * resolve through lyaml's explicit resolver table */
    assert_string_equal(
        eval_string("local y = require('yaml')\n"
                    "local t = y.decode('s: !!str 123\\n"
                    "ints: [!!int \"42\", !!int 0x2A, !!int 017,"
                    " !!int -0b11, !!int 190:20:30]\\n"
                    "b1: !!bool \"yes\"\\nb2: !!bool y\\n"
                    "f1: !!float 12\\nf2: !!float 017\\nf3: !!float 0x2A\\n"
                    "f4: !!float 0b101\\nf5: !!float 190:20:30.15\\n"
                    "f6: !!float .inf\\n')\n"
                    "assert(t.s == '123' and type(t.s) == 'string')\n"
                    "assert(t.ints[1] == 42 and t.ints[2] == 42 and t.ints[3] == 15)\n"
                    "assert(t.ints[4] == -3 and t.ints[5] == 685230)\n"
                    "assert(math.type(t.ints[5]) == 'integer')\n"
                    "assert(t.b1 == true and t.b2 == true) -- single-letter y/n\n"
                    "assert(t.f1 == 12.0 and math.type(t.f1) == 'float')\n"
                    "assert(t.f2 == 15.0 and t.f3 == 42.0 and t.f4 == 5.0)\n"
                    "assert(t.f5 == 685230.15 and t.f6 == math.huge)\n"
                    "assert(y.decode('n1: !!null ~\\n', {nullval = y.null}).n1 == y.null)\n"
                    "assert(y.decode('n2: !!null whatever\\n',"
                    " {nullval = y.null}).n2 == y.null) -- body is ignored\n"
                    "assert(y.decode('n3: !!null ~\\n').n3 == nil) -- default nullval\n"
                    "return 'ok'"),
        "ok");
    /* a tag the resolver rejects names the tag and the offending value */
    assert_string_equal(
        eval_string("local y = require('yaml')\n"
                    "local _, e1 = y.decode('a: !!int notanumber\\n')\n"
                    "local _, e2 = y.decode('a: !!float zz\\n')\n"
                    "return e1 .. ' | ' .. e2"),
        "yaml: invalid 'tag:yaml.org,2002:int' value: 'notanumber' at line 1, column 4"
        " | yaml: invalid 'tag:yaml.org,2002:float' value: 'zz' at line 1, column 4");
}

static void test_yaml_decode_block_scalars(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local t = require('yaml').decode('lit: |\\n  line1\\n  line2\\n"
                    "fold: >\\n  w1\\n  w2\\nstrip: |-\\n  s1\\n')\n"
                    "assert(t.lit == 'line1\\nline2\\n') -- literal keeps breaks\n"
                    "assert(t.fold == 'w1 w2\\n') -- folded joins with spaces\n"
                    "assert(t.strip == 's1') -- '-' drops the trailing break\n"
                    "return 'ok'"),
        "ok");
}

static void test_yaml_decode_flow_collections(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local t = require('yaml').decode("
                    "'seq: [1, a, {b: 2}]\\nmap: {x: [true, ~], y: {z: deep}}\\n')\n"
                    "assert(t.seq[1] == 1 and t.seq[2] == 'a' and t.seq[3].b == 2)\n"
                    "assert(t.map.x[1] == true and t.map.x[2] == nil)\n"
                    "assert(#t.map.x == 1) -- null member drops out of the sequence\n"
                    "assert(t.map.y.z == 'deep')\n"
                    "return 'ok'"),
        "ok");
}

static void test_yaml_decode_multi_document(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local y = require('yaml')\n"
                    "local docs = y.decodeAll('---\\na: 1\\n---\\nb: 2\\n')\n"
                    "assert(#docs == 2 and docs[1].a == 1 and docs[2].b == 2)\n"
                    "local first = y.decode('---\\na: 1\\n---\\nb: 2\\n')\n"
                    "assert(first.a == 1 and first.b == nil) -- decode takes doc one\n"
                    "assert(#y.decodeAll('') == 0)\n"
                    "local v, e = y.decode('')\n"
                    "assert(v == nil and e == nil) -- empty stream: nil, no error\n"
                    "return 'ok'"),
        "ok");
    /* a null document under the default nullval lands as a nil slot */
    assert_string_equal(
        eval_string("local mixed = require('yaml').decodeAll("
                    "'---\\na: null\\n---\\n~\\n')\n"
                    "return tostring(#mixed) .. ',' .. tostring(mixed[1].a == nil)"
                    " .. ',' .. tostring(mixed[2] == nil)"),
        "1,true,true");
}

static void test_yaml_decode_all_opts_and_errors(void **state)
{
    (void)state;
    /* decodeAll takes the same opts table as decode — the nullval
     * sentinel reaches every document of the stream */
    assert_string_equal(
        eval_string("local y = require('yaml')\n"
                    "local docs = y.decodeAll('---\\n~\\n---\\n7\\n',"
                    " {nullval = y.null})\n"
                    "assert(#docs == 2 and docs[1] == y.null and docs[2] == 7)\n"
                    "return 'ok'"),
        "ok");
    /* bad input anywhere in the stream fails the whole call, through
     * the same fail() normalization as decode */
    assert_string_equal(
        eval_string("local t, e = require('yaml').decodeAll("
                    "'a: 1\\n---\\nb: [1, 2\\n')\n"
                    "return tostring(t) .. ' | ' .. e"),
        "nil | yaml: did not find expected ',' or ']' at line 3, column 8");
}

static void test_yaml_decode_errors(void **state)
{
    (void)state;
    /* lyaml reports the mark of the last event that PARSED (1-based,
     * UTF-8 character columns — same口径 as csv/ini), normalized to the
     * stdlib error shape by the wrapper */
    assert_string_equal(
        eval_string("local y = require('yaml')\n"
                    "local t1, e1 = y.decode('a: 1\\n  b: 2\\n')\n"
                    "local t2, e2 = y.decode('key: \"unterminated\\nother: 1\\n')\n"
                    "local t3, e3 = y.decode('a: [1, 2\\n')\n"
                    "local t4, e4 = y.decode('a: *nope\\n')\n"
                    "return e1 .. ' | ' .. e2 .. ' | ' .. e3 .. ' | ' .. e4"),
        "yaml: mapping values are not allowed in this context at line 1, column 4"
        " | yaml: found unexpected end of stream at line 1, column 1"
        " | yaml: did not find expected ',' or ']' at line 1, column 8"
        " | yaml: invalid reference: nope at line 1, column 4");
    /* the line tracks the document, not just line 1 */
    assert_string_equal(
        eval_string("local t, e = require('yaml').decode('a: 1\\nb: 2\\n  c: 3\\n')\n"
                    "return tostring(t) .. ' | ' .. e"),
        "nil | yaml: mapping values are not allowed in this context at line 2, column 4");
    /* columns count UTF-8 characters: the marked scalar 'v' sits at
     * character 4 (byte 5) after the two-byte é — observed 5, not 6 */
    assert_string_equal(
        eval_string("local t, e = require('yaml').decode('ké: v\\n  b: 2\\n')\n"
                    "return e"),
        "yaml: mapping values are not allowed in this context at line 1, column 5");
}

static void test_yaml_null_semantics(void **state)
{
    (void)state;
    /* default: null becomes nil — the key is absent, json/dkjson style */
    assert_string_equal(
        eval_string("local y = require('yaml')\n"
                    "local t = y.decode('a: 1\\nb: null\\nc: ~\\n')\n"
                    "local keys = 0\n"
                    "for _ in pairs(t) do keys = keys + 1 end\n"
                    "assert(t.a == 1 and t.b == nil and t.c == nil and keys == 1)\n"
                    "assert(y.decode('~') == nil) -- null document decodes to nil\n"
                    "return 'ok'"),
        "ok");
    /* opts.nullval = yaml.null keeps lyaml's sentinel instead */
    assert_string_equal(
        eval_string("local y = require('yaml')\n"
                    "local n = y.decode('a: 1\\nb: null\\n', {nullval = y.null})\n"
                    "assert(n.a == 1 and n.b == y.null)\n"
                    "assert(y.decode('~', {nullval = y.null}) == y.null)\n"
                    "return 'ok'"),
        "ok");
    /* substitution reaches nested values and sequence members */
    assert_string_equal(
        eval_string("local y = require('yaml')\n"
                    "local nx = y.decode('a:\\n  b: null\\nc: [null, 1]\\n',"
                    "                    {nullval = y.null})\n"
                    "assert(nx.a.b == y.null and nx.c[1] == y.null and nx.c[2] == 1)\n"
                    "return 'ok'"),
        "ok");
}

static void test_yaml_anchors_share_references(void **state)
{
    (void)state;
    /* aliases resolve to the SAME table, not a copy */
    assert_string_equal(
        eval_string("local t = require('yaml').decode('x: &a [1, 2]\\ny: *a\\n')\n"
                    "assert(t.x == t.y)\n"
                    "local m = require('yaml').decode("
                    "'base: &b {k: v}\\nuse1: {f: *b}\\n')\n"
                    "assert(m.use1.f == m.base)\n"
                    "return 'ok'"),
        "ok");
    /* a self-referential anchor must not loop the null-substitution walk */
    assert_string_equal(
        eval_string("local s = require('yaml').decode('&a\\nself: *a\\n')\n"
                    "return tostring(s.self == s)"),
        "true");
}

static void test_yaml_decode_merge_keys(void **state)
{
    (void)state;
    /* '<<' merge keys (YAML 1.1): referenced mapping's pairs land in the
     * target only where no own key occupies the slot */
    assert_string_equal(
        eval_string("local t = require('yaml').decode("
                    "'defaults: &d\\n  a: 1\\n  b: 2\\n"
                    "item:\\n  <<: *d\\n  b: 3\\n')\n"
                    "assert(t.item.a == 1 and t.item.b == 3) -- merge fills, own key wins\n"
                    "assert(t.defaults.a == 1 and t.defaults.b == 2) -- source intact\n"
                    "return 'ok'"),
        "ok");
    /* a merge SEQUENCE folds every mapping in order */
    assert_string_equal(
        eval_string("local s = require('yaml').decode("
                    "'d1: &a\\n  x: 1\\nd2: &b\\n  y: 2\\n"
                    "m:\\n  <<: [*a, *b]\\n')\n"
                    "assert(s.m.x == 1 and s.m.y == 2)\n"
                    "return 'ok'"),
        "ok");
    /* '!!merge' is the formal tag spelling of '<<', same behavior */
    assert_string_equal(
        eval_string("local g = require('yaml').decode("
                    "'defaults: &d\\n  a: 1\\n"
                    "item:\\n  !!merge : *d\\n')\n"
                    "assert(g.item.a == 1 and #g.item == 0)\n"
                    "return 'ok'"),
        "ok");
    /* merge sources must be mappings — scalars in either shape fail */
    assert_string_equal(
        eval_string("local y = require('yaml')\n"
                    "local _, e1 = y.decode('m:\\n  <<: [1]\\n')\n"
                    "local _, e2 = y.decode('m:\\n  <<: str\\n')\n"
                    "return e1 .. ' | ' .. e2"),
        "yaml: invalid '<<' sequence element 1: 1 at line 2, column 9"
        " | yaml: invalid '<<' merge event: str at line 2, column 7");
}

static void test_yaml_encode_exact_shapes(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("return require('yaml').encode({ok = true})"),
        "---\nok: true\n...\n");
    assert_string_equal(
        eval_string("return require('yaml').encode({1, 2, 3})"),
        "---\n- 1\n- 2\n- 3\n...\n");
    assert_string_equal(
        eval_string("return require('yaml').encode(42)"),
        "--- 42\n...\n");
    assert_string_equal(
        eval_string("return require('yaml').encode('hello')"),
        "--- hello\n...\n");
    /* newline-carrying strings take the literal block style, stripped */
    assert_string_equal(
        eval_string("return require('yaml').encode({s = 'line1\\nline2'})"),
        "---\ns: |-\n  line1\n  line2\n...\n");
    /* the empty table reads as an empty sequence; nil as the empty stream */
    assert_string_equal(
        eval_string("return require('yaml').encode({})"),
        "--- []\n...\n");
    assert_string_equal(
        eval_string("return require('yaml').encode(nil)"),
        "");
    /* strings that would parse as other types stay quoted on the way out */
    assert_string_equal(
        eval_string("return require('yaml').encode('123')"),
        "--- '123'\n...\n");
    assert_string_equal(
        eval_string("local y = require('yaml')\nreturn y.encode({a = 'yes'})"),
        "---\na: 'yes'\n...\n");
    /* the null sentinel emits ~; inf/nan take YAML spellings (one key
     * per table: pairs order must not decide the expected text) */
    assert_string_equal(
        eval_string("local y = require('yaml')\nreturn y.encode({a = y.null})"),
        "---\na: ~\n...\n");
    assert_string_equal(
        eval_string("return require('yaml').encode({a = math.huge})"),
        "---\na: .inf\n...\n");
    assert_string_equal(
        eval_string("return require('yaml').encode({a = -math.huge})"),
        "---\na: -.inf\n...\n");
    assert_string_equal(
        eval_string("return require('yaml').encode({a = 0/0})"),
        "---\na: .nan\n...\n");
}

static void test_yaml_encode_roundtrip(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local y = require('yaml')\n"
                    "local function eq(a, b)\n"
                    "  if a == b then return true end\n"
                    "  if type(a) ~= 'table' or type(b) ~= 'table' then return false end\n"
                    "  local n = 0\n"
                    "  for k, v in pairs(a) do\n"
                    "    if not eq(v, b[k]) then return false end\n"
                    "    n = n + 1\n"
                    "  end\n"
                    "  for _ in pairs(b) do n = n - 1 end\n"
                    "  return n == 0\n"
                    "end\n"
                    "local rich = {s = 'x\\ty', n = {1, 2.5, 'a'},\n"
                    "              deep = {k = 'v'}, flag = true, fl = 0.5}\n"
                    "assert(eq(rich, y.decode(y.encode(rich))), 'roundtrip mismatch')\n"
                    "return 'ok'"),
        "ok");
}

static void test_yaml_encode_errors(void **state)
{
    (void)state;
    /* cycles are caught before dumping (lyaml itself would blow the C
     * stack); shared siblings are NOT cycles and must encode fine */
    assert_string_equal(
        eval_string("local y = require('yaml')\n"
                    "local cyc = {}\n"
                    "cyc.self = cyc\n"
                    "local s1, e1 = y.encode(cyc)\n"
                    "local s2, e2 = y.encode({f = function() end})\n"
                    "local shared = {1, 2}\n"
                    "local s3 = y.encode({x = shared, y = shared})\n"
                    "return tostring(s1) .. ' | ' .. e2 .. ' | '"
                    " .. tostring(s3 ~= nil)"),
        "nil | yaml: cannot dump object of type 'function' | true");
}

static void test_yaml_args_contract(void **state)
{
    (void)state;
    /* bad ARGUMENTS raise; only bad DATA returns nil, err */
    assert_string_equal(
        eval_string("local y = require('yaml')\n"
                    "local _, m1 = pcall(y.decode, {})\n"
                    "local _, m2 = pcall(y.decode, 'a: 1', true)\n"
                    "local ok3 = pcall(y.decode, 42)\n"
                    "local ok4 = pcall(y.encode, {1}, 'x')\n"
                    "local ok5 = pcall(y.decodeAll, {})\n"
                    "return tostring(m1:find('expected a string', 1, true) ~= nil)"
                    " .. ',' .. tostring(m2:find('opts must be a table', 1, true) ~= nil)"
                    " .. ',' .. tostring(ok3) .. ',' .. tostring(ok4)"
                    " .. ',' .. tostring(ok5)"),
        "true,true,false,false,false");
}

/* -- xml module (batch 5, expat 2.8.5 + lua-expat 1.5.2 vendor) ------- */

static void test_xml_decode_dom_shape(void **state)
{
    (void)state;
    /* DOM 三件套: 元素 = {tag, attrs, kids}, 文本节点是 kids 里的普通字符串,
     * 属性值恒为字符串; lxp 的数字键 (属性文档序) 是绑定层细节, 不进 DOM */
    assert_string_equal(
        eval_string("local x = require('xml')\n"
                    "local d = x.decode('<person id=\"7\"><name>Ada</name><age>36</age></person>')\n"
                    "assert(d.tag == 'person')\n"
                    "assert(d.attrs.id == '7' and type(d.attrs.id) == 'string')\n"
                    "assert(#d.kids == 2)\n"
                    "assert(d.kids[1].tag == 'name' and next(d.kids[1].attrs) == nil)\n"
                    "assert(d.kids[1].kids[1] == 'Ada' and type(d.kids[1].kids[1]) == 'string')\n"
                    "assert(d.kids[2].tag == 'age' and d.kids[2].kids[1] == '36')\n"
                    "local n = 0\n"
                    "for k, v in pairs(d.attrs) do n = n + 1 end\n"
                    "assert(n == 1) -- numeric order keys dropped\n"
                    "return 'ok'"),
        "ok");
}

static void test_xml_decode_entities_cdata(void **state)
{
    (void)state;
    /* 实体展开 (&amp;/&lt;/&gt;)、CDATA 段、相邻文本片段 (实体/CDATA 边界
     * 会分片送达) 在元素边界合并; 声明/注释/DOCTYPE 不进 DOM */
    assert_string_equal(
        eval_string("local x = require('xml')\n"
                    "local d = x.decode('<r a=\"1&amp;2\">x&lt;y<![CDATA[raw <>&\"]]></r>')\n"
                    "assert(d.attrs.a == '1&2')\n"
                    "assert(d.kids[1] == 'x<yraw <>&\"') -- merged text run\n"
                    "local d2 = x.decode('<?xml version=\"1.0\"?><!-- c --><!DOCTYPE r><r><a/></r>')\n"
                    "assert(d2.tag == 'r' and #d2.kids == 1 and d2.kids[1].tag == 'a')\n"
                    "return 'ok'"),
        "ok");
}

static void test_xml_decode_namespaces(void **state)
{
    (void)state;
    /* 命名空间前缀原样保留 (<x:a> 的 tag 是 "x:a"), 不做解析展开;
     * xmlns 声明就是普通属性 */
    assert_string_equal(
        eval_string("local x = require('xml')\n"
                    "local d = x.decode('<x:a xmlns:x=\"urn:x\"><x:b/></x:a>')\n"
                    "assert(d.tag == 'x:a')\n"
                    "assert(d.attrs['xmlns:x'] == 'urn:x')\n"
                    "assert(d.kids[1].tag == 'x:b')\n"
                    "return 'ok'"),
        "ok");
}

static void test_xml_decode_errors(void **state)
{
    (void)state;
    /* 坏输入 → nil, err; 错误带行列 (expat 坐标, 列按 UTF-8 字符计 1 基) */
    assert_string_equal(
        eval_string("local x = require('xml')\n"
                    "local function e(s) local _, err = x.decode(s) return err end\n"
                    "assert(e('<a><b></a>') == 'xml: mismatched tag at line 1, column 9')\n"
                    "assert(e('<a></b>') == 'xml: mismatched tag at line 1, column 6')\n"
                    "assert(e('<a><b>') == 'xml: no element found at line 1, column 7')\n"
                    "assert(e('<a>') == 'xml: no element found at line 1, column 4')\n"
                    "assert(e('') == 'xml: no element found at line 1, column 1')\n"
                    "assert(e('<a>text</a><b/>') == 'xml: junk after document element at line 1, column 12')\n"
                    "assert(e('<a x=1></a>') == 'xml: not well-formed (invalid token) at line 1, column 6')\n"
                    "assert(e('<a>&undefined;</a>') == 'xml: undefined entity at line 1, column 4')\n"
                    "assert(e('<a><![CDATA[unclosed</a>') == 'xml: unclosed CDATA section at line 1, column 25')\n"
                    "assert(e('<r x=\"1\" x=\"2\"/>') == 'xml: duplicate attribute at line 1, column 10')\n"
                    "assert(e('<a>\\n  <b>\\n</a>') == 'xml: mismatched tag at line 3, column 3')\n"
                    "assert(e('<r>张三<x></r>') == 'xml: mismatched tag at line 1, column 11')\n"
                    "return 'ok'"),
        "ok");
}

static void test_xml_encode_exact_shapes(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local x = require('xml')\n"
                    "local doc = { tag = 'person', attrs = { id = '7' }, kids = {\n"
                    "  { tag = 'name', attrs = {}, kids = { 'Ada' } },\n"
                    "  { tag = 'age', attrs = {}, kids = { 36 } },\n"
                    "  { tag = 'bio', attrs = {}, kids = { 'a<b', '&c' } },\n"
                    "  { tag = 'empty', attrs = {}, kids = {} },\n"
                    "} }\n"
                    "assert(x.encode(doc) == '<person id=\"7\"><name>Ada</name><age>36</age>"
                    "<bio>a&lt;b&amp;c</bio><empty></empty></person>')\n"
                    "assert(x.encode(doc, { indent = 2 }) == '<person id=\"7\">\\n  <name>Ada</name>\\n"
                    "  <age>36</age>\\n  <bio>a&lt;b&amp;c</bio>\\n  <empty/>\\n</person>')\n"
                    "assert(x.encode({ tag = 'r', attrs = { n = 42 }, kids = {} }) == '<r n=\"42\"></r>')\n"
                    "assert(x.encode({ tag = 'r', attrs = { b = true }, kids = {} }) == '<r b=\"true\"></r>')\n"
                    "assert(x.encode({ tag = 'r' }) == '<r></r>') -- nil attrs/kids\n"
                    "return 'ok'"),
        "ok");
}

static void test_xml_encode_roundtrip(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local x = require('xml')\n"
                    "local src = '<person id=\"7\"><name>Ada</name><bio>a&lt;b</bio></person>'\n"
                    "assert(x.encode(x.decode(src)) == src)\n"
                    "local d = x.decode('<a><b><c>x</c></b><b><c>y</c></b></a>')\n"
                    "assert(x.encode(d, { indent = 2 }) == '<a>\\n  <b>\\n    <c>x</c>\\n  </b>\\n"
                    "  <b>\\n    <c>y</c>\\n  </b>\\n</a>')\n"
                    "return 'ok'"),
        "ok");
}

static void test_xml_encode_errors(void **state)
{
    (void)state;
    /* 环引用 (元素是自身的祖先) → nil, err; 共享兄弟不是环, 照常编码;
     * 参数类型错才 raise; 坏 DOM 形状 (嵌套面) 也是 nil, err 不 raise */
    assert_string_equal(
        eval_string("local x = require('xml')\n"
                    "local cyc = { tag = 'a', attrs = {}, kids = {} }\n"
                    "cyc.kids[1] = cyc\n"
                    "local s1, e1 = x.encode(cyc)\n"
                    "assert(s1 == nil and e1 == 'xml: cyclic table reference')\n"
                    "local shared = { tag = 's', attrs = {}, kids = {} }\n"
                    "assert(x.encode({ tag = 'r', attrs = {}, kids = { shared, shared } }) ~= nil)\n"
                    "assert(pcall(x.encode, 'str') == false)\n"
                    "assert(pcall(x.encode, { tag = 'a' }, { indent = '2' }) == false)\n"
                    "local t1, m1 = x.encode({ tag = 123, attrs = {}, kids = {} })\n"
                    "assert(t1 == nil and m1 == 'xml: element tag must be a string')\n"
                    "local t2, m2 = x.encode({ tag = 'a', attrs = 5, kids = {} })\n"
                    "assert(t2 == nil and m2 == 'xml: attrs must be a table')\n"
                    "local t3, m3 = x.encode({ tag = 'a', attrs = {}, kids = 5 })\n"
                    "assert(t3 == nil and m3 == 'xml: kids must be a table')\n"
                    "return 'ok'"),
        "ok");
    /* 混合 kid (标量 + 元素) 的美化输出: 标量 kid 独占一行 */
    assert_string_equal(
        eval_string("local x = require('xml')\n"
                    "return x.encode({ tag = 'r', attrs = {}, kids ="
                    " { 't', { tag = 'a', attrs = {}, kids = {} } } }, { indent = 2 })"),
        "<r>\n  t\n  <a/>\n</r>");
}

static void test_xml_sax_streaming(void **state)
{
    (void)state;
    /* SAX 透传: handler 表就是 lxp 的形状 (首参 self), 增量喂 + 空参收尾。
     * 默认 bufferCharData 下文本在 chunk 边界/下一事件处成片交付:
     * 'he' 在 chunk1 末尾, 'llo' 在 </a> 事件前 —— 分片是缓冲的产物,
     * 语义 (事件序列与内容) 稳定 */
    assert_string_equal(
        eval_string("local x = require('xml')\n"
                    "local ev = {}\n"
                    "local p = x.sax {\n"
                    "  StartElement = function(_, n) ev[#ev+1] = 'S:' .. n end,\n"
                    "  EndElement = function(_, n) ev[#ev+1] = 'E:' .. n end,\n"
                    "  CharacterData = function(_, t) ev[#ev+1] = 'T:' .. t end,\n"
                    "}\n"
                    "p:parse('<a x=\"1\">he')\n"
                    "p:parse('llo</a>')\n"
                    "p:parse() -- finalize\n"
                    "return table.concat(ev, '|')"),
        "S:a|T:he|T:llo|E:a");
    /* 单 chunk 完整文档: 文本在 EndElement 事件前一次性交付 */
    assert_string_equal(
        eval_string("local x = require('xml')\n"
                    "local ev = {}\n"
                    "local p = x.sax {\n"
                    "  StartElement = function(_, n) ev[#ev+1] = 'S:' .. n end,\n"
                    "  EndElement = function(_, n) ev[#ev+1] = 'E:' .. n end,\n"
                    "  CharacterData = function(_, t) ev[#ev+1] = 'T:' .. t end,\n"
                    "}\n"
                    "p:parse('<a>hello</a>')\n"
                    "p:parse()\n"
                    "return table.concat(ev, '|')"),
        "S:a|T:hello|E:a");
}

static void test_xml_args_contract(void **state)
{
    (void)state;
    /* bad ARGUMENTS raise; only bad DATA returns nil, err. sax 的 handler
     * 键校验与无参调用由 lxp 自己报 (checkcallbacks / checktype) */
    assert_string_equal(
        eval_string("local x = require('xml')\n"
                    "local _, m1 = pcall(x.decode, 42)\n"
                    "local _, m2 = pcall(x.encode, { tag = 'a' }, true)\n"
                    "local ok3 = pcall(x.sax, { Bogus = function() end })\n"
                    "local ok4 = pcall(x.sax)\n"
                    "return tostring(m1:find('expects a string', 1, true) ~= nil)"
                    " .. ',' .. tostring(m2:find('opts must be a table', 1, true) ~= nil)"
                    " .. ',' .. tostring(ok3) .. ',' .. tostring(ok4)"),
        "true,true,false,false");
}

/* -- path (Node path.posix, batch 6) ------------------------------------- */

static void test_path_normalize_join(void **state)
{
    (void)state;
    /* v24 实证: normalize 折叠连续斜杠 (含前导 //), 尾斜杠保留,
     * '.' 与 '..' 消解 (绝对路径下 .. 到根为止)。 */
    assert_string_equal(
        eval_string("local p = require('path')\n"
                    "return p.normalize('//a') .. ',' .. p.normalize('//') .. ','\n"
                    "  .. p.normalize('a//b') .. ',' .. p.normalize('a/..') .. ','\n"
                    "  .. p.normalize('foo/') .. ',' .. p.normalize('./') .. ','\n"
                    "  .. p.normalize('../') .. ',' .. p.normalize('/a/../..') .. ','\n"
                    "  .. p.normalize('a/./b/./c') .. ',' .. p.normalize('./a') .. ','\n"
                    "  .. p.normalize('a/../..') .. ',' .. p.normalize('') .. ','\n"
                    "  .. p.normalize('.')"),
        "/a,/,a/b,.,foo/,./,../,/,a/b/c,a,..,.,.");
    /* join 忽略空段后走 normalize; 无参 → '.' */
    assert_string_equal(
        eval_string("local p = require('path')\n"
                    "return p.join('a','b/','../c') .. ',' .. p.join('a','','b') .. ','\n"
                    "  .. p.join() .. ',' .. p.join('/foo','bar','./baz') .. ','\n"
                    "  .. p.join('..','x') .. ',' .. p.join('a','/b') .. ','\n"
                    "  .. p.join('.','b') .. ',' .. p.join('a','//b') .. ','\n"
                    "  .. p.join('','')"),
        "a/c,a/b,.,/foo/bar/baz,../x,a/b,b,a/b,.");
    /* isAbsolute: 仅根斜杠起头 */
    assert_string_equal(
        eval_string("local p = require('path')\n"
                    "return tostring(p.isAbsolute('/')) .. ','\n"
                    "  .. tostring(p.isAbsolute('//a')) .. ','\n"
                    "  .. tostring(p.isAbsolute('a/b')) .. ','\n"
                    "  .. tostring(p.isAbsolute('./a')) .. ','\n"
                    "  .. tostring(p.isAbsolute(''))"),
        "true,true,false,false,false");
}

static void test_path_resolve_relative(void **state)
{
    (void)state;
    /* resolve: 右起拼接至绝对段, 折叠斜杠, 剥尾斜杠 */
    assert_string_equal(
        eval_string("local p = require('path')\n"
                    "return p.resolve('/a','b','c') .. ',' .. p.resolve('//a','b') .. ','\n"
                    "  .. p.resolve('a','/b') .. ',' .. p.resolve('/a/../b','c') .. ','\n"
                    "  .. p.resolve('/','a') .. ',' .. p.resolve('/a/b/')"),
        "/a/b/c,/a/b,/b,/b/c,/a,/a/b");
    /* 无绝对段时锚定 cwd; 二次命中 cwd 缓存 */
    char cwdbuf[384];
    assert_non_null(getcwd(cwdbuf, sizeof(cwdbuf)));
    char expect[1024];
    snprintf(expect, sizeof(expect), "%s/a/b,%s/x", cwdbuf, cwdbuf);
    assert_string_equal(
        eval_string("local p = require('path')\n"
                    "return p.resolve('a','./b') .. ',' .. p.resolve('x')"),
        expect);
    /* relative: 段差 + 上跳; 同路径 (含尾斜杠差异) → '' */
    assert_string_equal(
        eval_string("local p = require('path')\n"
                    "return p.relative('/a/b','/a/c/d') .. ','\n"
                    "  .. tostring(p.relative('/a/b','/a/b') == '') .. ','\n"
                    "  .. tostring(p.relative('/a/b','/a/b/') == '') .. ','\n"
                    "  .. p.relative('/a/b/c','/a/b') .. ','\n"
                    "  .. p.relative('/a/b','/a/b/c/d') .. ','\n"
                    "  .. p.relative('/a/x','/b/y') .. ','\n"
                    "  .. p.relative('/','/a') .. ',' .. p.relative('/a','/')"),
        "../c/d,true,true,..,c/d,../../b/y,a,..");
}

static void test_path_dirname_basename_extname(void **state)
{
    (void)state;
    /* dirname 是文本操作: a//b → a/, //a//b → //a/ (斜杠原样保留) */
    assert_string_equal(
        eval_string("local p = require('path')\n"
                    "return p.dirname('/a/b/luna.lua') .. ',' .. p.dirname('luna.lua') .. ','\n"
                    "  .. p.dirname('/luna.lua') .. ',' .. p.dirname('/a/b/') .. ','\n"
                    "  .. p.dirname('a//b') .. ',' .. p.dirname('/') .. ','\n"
                    "  .. p.dirname('//a//b') .. ',' .. p.dirname('a') .. ','\n"
                    "  .. p.dirname('./a') .. ',' .. p.dirname('../a') .. ','\n"
                    "  .. p.dirname('a/') .. ',' .. p.dirname('')"),
        "/a/b,.,/,/a,a/,/,//a/,.,.,..,.,.");
    assert_string_equal(
        eval_string("local p = require('path')\n"
                    "return p.basename('/a/b/luna.lua') .. ','\n"
                    "  .. p.basename('/a/b/luna.lua','.lua') .. ','\n"
                    "  .. p.basename('/a/b/') .. ','\n"
                    "  .. tostring(p.basename('.lua','.lua') == '') .. ','\n"
                    "  .. p.basename('a.lua.lua','.lua') .. ','\n"
                    "  .. tostring(p.basename('/') == '') .. ','\n"
                    "  .. tostring(p.basename('') == '') .. ','\n"
                    "  .. p.basename('/a/b/','.x') .. ','\n"
                    "  .. p.basename('/a/b/', '')"),
        "luna.lua,luna,b,true,a.lua,true,true,b,b");
    /* extname 的全点前缀规则: '..' 与 '.bashrc' 无扩展名 */
    assert_string_equal(
        eval_string("local p = require('path')\n"
                    "return p.extname('luna.tar.gz') .. ','\n"
                    "  .. tostring(p.extname('luna') == '') .. ','\n"
                    "  .. tostring(p.extname('.bashrc') == '') .. ','\n"
                    "  .. p.extname('a..') .. ',' .. p.extname('foo.') .. ','\n"
                    "  .. p.extname('.a.b') .. ',' .. p.extname('a.b.') .. ','\n"
                    "  .. tostring(p.extname('/a/.c') == '') .. ','\n"
                    "  .. tostring(p.extname('..') == '')"),
        ".gz,true,true,.,.,.b,.,true,true");
}

static void test_path_parse_format(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local p = require('path')\n"
                    "local function s(o)\n"
                    "  return o.root..'|'..o.dir..'|'..o.base..'|'..o.name..'|'..o.ext\n"
                    "end\n"
                    "return s(p.parse('/a/b/luna.lua')) .. ',' .. s(p.parse('.')) .. ','\n"
                    "  .. s(p.parse('/')) .. ',' .. s(p.parse('a/b/')) .. ','\n"
                    "  .. s(p.parse('//x/y')) .. ',' .. s(p.parse('..')) .. ','\n"
                    "  .. s(p.parse('/luna.lua'))"),
        "/|/a/b|luna.lua|luna|.lua,||.|.|,/|/|||,|a|b|b|,/|//x|y|y|,||..|..|,"
        "/|/|luna.lua|luna|.lua");
    /* format: dir 原样拼接 (尾斜杠不吸收), 仅 root 无 dir 时不加分隔符,
     * ext 无点补点, 空串视为缺席 (JS 真值口径) */
    assert_string_equal(
        eval_string("local p = require('path')\n"
                    "return p.format{root='/',dir='/a/b',base='luna.lua',name='luna',ext='.lua'} .. ','\n"
                    "  .. p.format{dir='/a/b',name='luna',ext='.lua'} .. ','\n"
                    "  .. p.format{root='/',base='x'} .. ',' .. p.format{base='x'} .. ','\n"
                    "  .. p.format{name='x',ext='lua'} .. ',' .. p.format{name='x'} .. ','\n"
                    "  .. p.format{dir='/a/b/',name='x'} .. ','\n"
                    "  .. p.format{dir='',name='x',ext='.y'} .. ','\n"
                    "  .. p.format{dir='/',name='x'} .. ','\n"
                    "  .. p.format{dir='/a/b/',base='y'} .. ','\n"
                    "  .. tostring(p.format{} == '') .. ',' .. p.format{ext='lua'} .. ','\n"
                    "  .. p.format{name='x',ext=''} .. ',' .. p.format{dir='a',name='x'} .. ','\n"
                    "  .. p.format{root='/',name='x'}"),
        "/a/b/luna.lua,/a/b/luna.lua,/x,x,x.lua,x,/a/b//x,x.y,//x,/a/b//y,true,.lua,x,a/x,/x");
    assert_string_equal(
        eval_string("local p = require('path')\n"
                    "return p.sep .. p.delimiter .. ',' .. tostring(p.posix == p)"),
        "/:,true");
    /* format 的字段类型错 raise (JS 只校验对象本身, 这里收紧到字段) */
    assert_string_equal(
        eval_string("local p = require('path')\n"
                    "local _, m = pcall(p.format, {base = 42})\n"
                    "return tostring(m:find('path.format: base must be a string', 1, true) ~= nil)"),
        "true");
}

static void test_path_args_contract(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local p = require('path')\n"
                    "local _, m1 = pcall(p.join, 'a', 42)\n"
                    "local _, m2 = pcall(p.basename, true)\n"
                    "local _, m3 = pcall(p.format, 'notatable')\n"
                    "local _, m4 = pcall(p.parse, {})\n"
                    "local _, m5 = pcall(p.resolve, '/a', nil)\n"
                    "local _, m6 = pcall(p.basename, 'a', 42)\n"
                    "local _, m7 = pcall(p.isAbsolute, 42)\n"
                    "return tostring(m1:find('path.join', 1, true) ~= nil)"
                    " .. ',' .. tostring(m2:find('path.basename', 1, true) ~= nil)"
                    " .. ',' .. tostring(m3:find('expects a table', 1, true) ~= nil)"
                    " .. ',' .. tostring(m4:find('path.parse', 1, true) ~= nil)"
                    " .. ',' .. tostring(m5:find('path.resolve', 1, true) ~= nil)"
                    " .. ',' .. tostring(m6:find('path.basename', 1, true) ~= nil)"
                    " .. ',' .. tostring(m7:find('path.isAbsolute', 1, true) ~= nil)"),
        "true,true,true,true,true,true,true");
}

/* -- util (Node util.format/inspect, batch 6) ----------------------------- */

static void test_util_format_conversions(void **state)
{
    (void)state;
    /* v24 实证: %d/%f 不截断, %i 向零截断; 非数串 → NaN;
     * NaN/Infinity 按原词。%x/%X 为本模块扩展 (Node v24 无此对)。 */
    assert_string_equal(
        eval_string("local f = require('util').format\n"
                    "return f('%s %s','a','b') .. ','\n"
                    "  .. f('%d',42) .. ',' .. f('%d','42') .. ','\n"
                    "  .. f('%d','42abc') .. ',' .. f('%d','abc') .. ','\n"
                    "  .. f('%d',3.7) .. ',' .. f('%d',-3.7) .. ','\n"
                    "  .. f('%i',3.7) .. ',' .. f('%i',-3.7) .. ','\n"
                    "  .. f('%i','7.9')"),
        "a b,42,42,NaN,NaN,3.7,-3.7,3,-3,7");
    assert_string_equal(
        eval_string("local f = require('util').format\n"
                    "return f('%f',3) .. ',' .. f('%f',3.0) .. ','\n"
                    "  .. f('%f','2.5') .. ',' .. f('%f',0/0) .. ','\n"
                    "  .. f('%f',math.huge) .. ','\n"
                    "  .. f('%x',255) .. ',' .. f('%X',255) .. ','\n"
                    "  .. f('%x',255.9) .. ',' .. f('%x',-26) .. ','\n"
                    "  .. f('%x','nope') .. ',' .. f('%x',math.huge) .. ','\n"
                    "  .. f('%d',true) .. ',' .. f('%i','abc') .. ','\n"
                    "  .. f('%o',255) .. ',' .. f('%o',{a=1})"),
        "3,3,2.5,NaN,Infinity,ff,FF,ff,-1a,NaN,Infinity,1,NaN,255,{ a = 1 }");
    /* %j 的 dkjson 失败回退: 函数值无法序列化, 退 inspect */
    assert_string_equal(
        eval_string("local f = require('util').format\n"
                    "local s = f('%j', {f = print})\n"
                    "return tostring(s:find('{ f = function: 0x', 1, true) ~= nil)"),
        "true");
    /* %j 走 json.encode (dkjson); %% 成对折叠, 单串无参原样返回 */
    assert_string_equal(
        eval_string("local f = require('util').format\n"
                    "return f('%j',{a={1,2}}) .. ','\n"
                    "  .. f('100%%') .. ','\n"
                    "  .. f('%%','x') .. ',' .. f('50%% and %s','x')"),
        "{\"a\":[1,2]},100%%,% x,50% and x");
    assert_string_equal(
        eval_string("local f = require('util').format\n"
                    "return f('%s',3.0) .. ',' .. f('%s',true) .. ','\n"
                    "  .. f('%s',0/0) .. ',' .. f('%s',math.huge) .. ','\n"
                    "  .. f('%s',-math.huge)"),
        "3,true,NaN,Infinity,-Infinity");
}

static void test_util_format_no_fmt_and_extras(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local f = require('util').format\n"
                    "return f('%s') .. ',' .. f('a','b','c') .. ','\n"
                    "  .. f('%s:%s','a','b','extra') .. ',' .. f('%s %s','x')"),
        "%s,a b c,a:b extra,x %s");
    /* 未知转换符: '%' 原样保留且不消费参数 */
    assert_string_equal(
        eval_string("local f = require('util').format\n"
                    "return f('%','x') .. ',' .. f('%y','x') .. ','\n"
                    "  .. f('%10s','x') .. ',' .. f('%c','x') .. '|end'"),
        "% x,%y x,%10s x,|end");
    /* 首参非串: 全参空格连接; 单参非串 inspect */
    assert_string_equal(
        eval_string("local f = require('util').format\n"
                    "return f(42,'x') .. ',' .. f(42) .. ',' .. f({a=1}) .. ','\n"
                    "  .. f('x',{a=1}) .. ',' .. f('%s',{1,2})"),
        "42 x,42,{ a = 1 },x { a = 1 },{ 1, 2 }");
}

static void test_util_inspect_shapes(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local i = require('util').inspect\n"
                    "return i({1,2,3}) .. ',' .. i({a=1}) .. ',' .. i({}) .. ','\n"
                    "  .. i(true) .. ',' .. i(nil) .. ',' .. i(1.5) .. ','\n"
                    "  .. i(3.0) .. ',' .. i(0/0) .. ','\n"
                    "  .. i(math.huge) .. ',' .. i(-math.huge) .. ','\n"
                    "  .. i('héllo→')"),
        "{ 1, 2, 3 },{ a = 1 },{},true,nil,1.5,3,NaN,Infinity,-Infinity,"
        "\"héllo→\"");
    assert_string_equal(
        eval_string("local i = require('util').inspect\n"
                    "return i('quo\"te\\nnl\\ttab\\\\slash')"),
        "\"quo\\\"te\\nnl\\ttab\\\\slash\"");
    /* \r 与其余控制字节按 \\r / \\ddd 逃逸 */
    assert_string_equal(
        eval_string("local i = require('util').inspect\n"
                    "return i('a\\rb\\0c')"),
        "\"a\\rb\\000c\"");
    /* thread 走 tostring; rank-2 键 (布尔/函数) 括号表示并按 tostring 定序 */
    assert_string_equal(
        eval_string("local i = require('util').inspect\n"
                    "local th = coroutine.create(function() end)\n"
                    "local t = i({[print] = 1, [true] = 2})\n"
                    "return tostring(i(th):find('thread: 0x', 1, true) ~= nil) .. ','\n"
                    "  .. tostring(t:find('[true] = 2', 1, true) ~= nil) .. ','\n"
                    "  .. tostring(t:find('[function: 0x', 1, true) ~= nil)"),
        "true,true,true");
    /* 深度: 默认 2; 超深塌缩 [Object]/[Array]; 负数顶层即塌缩 */
    assert_string_equal(
        eval_string("local i = require('util').inspect\n"
                    "return i({a={b={c={d=1}}}}) .. ','\n"
                    "  .. i({a={b={c={d=1}}}},{depth=4}) .. ','\n"
                    "  .. i({a={b={c={d=1}}}},{depth=math.huge}) .. ','\n"
                    "  .. i({a={b=1}},{depth=0}) .. ','\n"
                    "  .. i({a={b=1}},{depth=-1}) .. ','\n"
                    "  .. i({1,{2}},{depth=0}) .. ','\n"
                    "  .. i({1,{2}},{depth=-1})"),
        "{ a = { b = { c = [Object] } } },"
        "{ a = { b = { c = { d = 1 } } } },"
        "{ a = { b = { c = { d = 1 } } } },"
        "{ a = [Object] },[Object],{ 1, [Array] },[Array]");
    /* 键: 标识符裸、Lua 关键字与特殊字符加引号; 序列段在前,
     * 非序列数字键升序、字符串键字节序 (确定性适配) */
    assert_string_equal(
        eval_string("local i = require('util').inspect\n"
                    "return i({['a b']=1, ok=2, ['1']=3, ['end']=4, [10]=5, [-1]=6})"),
        "{ [-1] = 6, [10] = 5, [\"1\"] = 3, [\"a b\"] = 1, [\"end\"] = 4, ok = 2 }");
    assert_string_equal(
        eval_string("local i = require('util').inspect\n"
                    "local s = i(function() end)\n"
                    "local c = i(print)\n"
                    "return tostring(s:find('<function ', 1, true) ~= nil) .. ','\n"
                    "  .. tostring(c:find('function: 0x', 1, true) ~= nil)"),
        "true,true");
}

static void test_util_inspect_truncation_circular(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local i = require('util').inspect\n"
                    "return i({1,2,3,4,5},{maxArrayLength=2}) .. ','\n"
                    "  .. i({1,2,3},{maxArrayLength=0}) .. ','\n"
                    "  .. i('abcdefghijklmnop',{maxStringLength=5})"),
        "{ 1, 2, ... 3 more items },{ ... 3 more items },"
        "\"abcde\"... 11 more characters");
    /* 默认上限: 数组 100 / 字符串 10000; 负上限钳 0 */
    assert_string_equal(
        eval_string("local i = require('util').inspect\n"
                    "local big = {}\n"
                    "for j = 1, 105 do big[j] = j end\n"
                    "local s = i(big)\n"
                    "local long = string.rep('x', 10005)\n"
                    "return s:match('%.%.%. (%d+) more items') .. ','\n"
                    "  .. i(long):match('%.%.%. (%d+) more characters') .. ','\n"
                    "  .. i({7,8,9}, {maxArrayLength = -1})"),
        "5,5,{ ... 3 more items }");
    assert_string_equal(
        eval_string("local i = require('util').inspect\n"
                    "local circ = {name='c'}\n"
                    "circ.self = circ\n"
                    "return i(circ)"),
        "{ name = \"c\", self = [Circular *1] }");
    /* 共享 (非环) 引用各自完整渲染, 不误标 */
    assert_string_equal(
        eval_string("local i = require('util').inspect\n"
                    "local shared = {1,2}\n"
                    "return i({a=shared, b=shared})"),
        "{ a = { 1, 2 }, b = { 1, 2 } }");
}

static void test_util_args_contract(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local u = require('util')\n"
                    "local _, m1 = pcall(u.inspect, {1}, true)\n"
                    "local _, m2 = pcall(u.inspect, {1}, {depth='x'})\n"
                    "return tostring(m1:find('opts must be a table', 1, true) ~= nil)"
                    " .. ',' .. tostring(m2:find('opts.depth must be a number', 1, true) ~= nil)"),
        "true,true");
}

/* -- events (Node EventEmitter, batch 6) ---------------------------------- */

static void test_events_dispatch(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local E = require('events')\n"
                    "local em = E.new()\n"
                    "local log = {}\n"
                    "em:on('t', function(x, y) log[#log+1] = x .. y end)\n"
                    "local a = em:emit('t', '1', '2')\n"
                    "local b = em:emit('nope')\n"
                    "return tostring(a) .. ',' .. tostring(b) .. ',' .. table.concat(log, ';')"),
        "true,false,12");
    /* once 一次即除, 返回 self */
    assert_string_equal(
        eval_string("local E = require('events')\n"
                    "local em = E.new()\n"
                    "local n = 0\n"
                    "local r = em:once('o', function() n = n + 1 end)\n"
                    "em:emit('o')\n"
                    "em:emit('o')\n"
                    "return tostring(r == em) .. ',' .. n .. ',' .. em:listenerCount('o')"),
        "true,1,0");
    /* prepend 族插队; prependOnce 首发后即除 */
    assert_string_equal(
        eval_string("local E = require('events')\n"
                    "local em = E.new()\n"
                    "local order = {}\n"
                    "em:on('e', function() order[#order+1] = 'a' end)\n"
                    "em:prependListener('e', function() order[#order+1] = 'p' end)\n"
                    "em:prependOnceListener('e', function() order[#order+1] = 'po' end)\n"
                    "em:emit('e')\n"
                    "em:emit('e')\n"
                    "return table.concat(order, ',')"),
        "po,p,a,p,a");
    /* emit 期间自移除: 本轮照常调用, 下轮不再 */
    assert_string_equal(
        eval_string("local E = require('events')\n"
                    "local em = E.new()\n"
                    "local seen = {}\n"
                    "local function h() seen[#seen+1] = 'in'; em:off('r', h) end\n"
                    "em:on('r', h)\n"
                    "em:emit('r')\n"
                    "em:emit('r')\n"
                    "return #seen"),
        "1");
    /* once 内重挂: 新 once 在下次 emit 生效 */
    assert_string_equal(
        eval_string("local E = require('events')\n"
                    "local em = E.new()\n"
                    "local n = 0\n"
                    "em:once('o', function() n = n + 1\n"
                    "  em:once('o', function() n = n + 10 end) end)\n"
                    "em:emit('o')\n"
                    "em:emit('o')\n"
                    "return n"),
        "11");
    /* listeners 返回副本且 once 以原函数入表; removeListener 只移首个匹配 */
    assert_string_equal(
        eval_string("local E = require('events')\n"
                    "local em = E.new()\n"
                    "local orig = function() end\n"
                    "em:once('x', orig)\n"
                    "local l = em:listeners('x')\n"
                    "local identity = l[1] == orig\n"
                    "l[1] = nil\n"
                    "local fa, fb = function() end, function() end\n"
                    "em:on('m', fa)\n"
                    "em:on('m', fa)\n"
                    "em:on('m', fb)\n"
                    "em:removeListener('m', fa)\n"
                    "return tostring(identity and em:listenerCount('x') == 1) .. ','\n"
                    "  .. em:listenerCount('m') .. ','\n"
                    "  .. tostring(em:listeners('m')[1] == fa)"),
        "true,2,true");
    /* removeAllListeners(ev) 清空并返回 self; 无参全清 */
    assert_string_equal(
        eval_string("local E = require('events')\n"
                    "local em = E.new()\n"
                    "em:on('a', function() end)\n"
                    "em:on('b', function() end)\n"
                    "local r = em:removeAllListeners('a')\n"
                    "local x = em:emit('a')\n"
                    "local y = em:emit('b')\n"
                    "em:removeAllListeners()\n"
                    "return tostring(r == em) .. ',' .. tostring(x) .. ','\n"
                    "  .. tostring(y) .. ',' .. em:listenerCount('b')"),
        "true,false,true,0");
}

static void test_events_builtin_events(void **state)
{
    (void)state;
    /* newListener 在添加前发出 (计数不含新者); removeListener 在移除后 */
    assert_string_equal(
        eval_string("local E = require('events')\n"
                    "local em = E.new()\n"
                    "local log = {}\n"
                    "em:on('newListener', function(ev, fn)\n"
                    "  log[#log+1] = 'new:' .. ev .. ':' .. em:listenerCount(ev)\n"
                    "end)\n"
                    "em:on('removeListener', function(ev, fn)\n"
                    "  log[#log+1] = 'rm:' .. ev\n"
                    "end)\n"
                    "local f = function() end\n"
                    "em:on('x', f)\n"
                    "em:off('x', f)\n"
                    "return table.concat(log, ',')"),
        "new:removeListener:0,new:x:0,rm:x");
    /* removeAllListeners(ev) 逐个 (LIFO) 发 removeListener */
    assert_string_equal(
        eval_string("local E = require('events')\n"
                    "local em = E.new()\n"
                    "local seq = {}\n"
                    "local f1 = function() end\n"
                    "local f2 = function() end\n"
                    "em:on('removeListener', function(ev, fn)\n"
                    "  seq[#seq+1] = fn == f1 and 'f1' or (fn == f2 and 'f2' or '?')\n"
                    "end)\n"
                    "em:on('e', f1)\n"
                    "em:on('e', f2)\n"
                    "em:removeAllListeners('e')\n"
                    "return table.concat(seq, ',')"),
        "f2,f1");
}

static void test_events_error_and_maxlisteners(void **state)
{
    (void)state;
    /* emit('error') 无监听: 表负载原样 raise; 标量包 Unhandled error.;
     * 无负载报 Unhandled 'error' event */
    assert_string_equal(
        eval_string("local E = require('events')\n"
                    "local _, e1 = pcall(function() E.new():emit('error', 'boomstr') end)\n"
                    "local _, e2 = pcall(function() E.new():emit('error', 42) end)\n"
                    "local terr = setmetatable({}, {__tostring = function() return 'boom' end})\n"
                    "local _, e3 = pcall(function() E.new():emit('error', terr) end)\n"
                    "local _, e4 = pcall(function() E.new():emit('error') end)\n"
                    "local _, e5 = pcall(function() E.new():emit('error', true) end)\n"
                    "return tostring(e1) .. ',' .. tostring(e2) .. ','\n"
                    "  .. tostring(e3 == terr) .. ',' .. tostring(e4) .. ','\n"
                    "  .. tostring(e5)"),
        "Unhandled error. ('boomstr'),Unhandled error. (42),true,"
        "Unhandled 'error' event,Unhandled error. (true)");
    /* 有监听则正常分发 */
    assert_string_equal(
        eval_string("local E = require('events')\n"
                    "local em = E.new()\n"
                    "local got\n"
                    "em:on('error', function(m) got = m end)\n"
                    "local ok = em:emit('error', 'x')\n"
                    "return tostring(ok) .. ',' .. got"),
        "true,x");
    /* 超限警告: 首个超限 add 一次, 计数为该次添加后的值; 移除后重臂;
     * 0/负上限不限。io.stderr 是普通全局表的字段, 可替换捕获。 */
    assert_string_equal(
        eval_string("local E = require('events')\n"
                    "local msgs = {}\n"
                    "local saved = io.stderr\n"
                    "io.stderr = { write = function(_, m) msgs[#msgs+1] = m end }\n"
                    "local em = E.new()\n"
                    "em:setMaxListeners(1)\n"
                    "local g1, g2 = function() end, function() end\n"
                    "em:on('w', g1)\n"
                    "em:on('w', g2)\n"
                    "em:on('w', function() end)\n"
                    "em:removeListener('w', g2)\n"
                    "em:on('w', function() end)\n"
                    "local em2 = E.new()\n"
                    "em2:setMaxListeners(0)\n"
                    "for j = 1, 20 do em2:on('u', function() end) end\n"
                    "io.stderr = saved\n"
                    "return (msgs[1] or 'NONE') .. '#' .. #msgs"),
        "MaxListenersExceededWarning: Possible EventEmitter memory leak"
        " detected. 2 w listeners added to [EventEmitter]. MaxListeners is"
        " 1. Use emitter:setMaxListeners() to increase limit\n#2");
    /* defaultMaxListeners 可调, 影响此后添加判定 */
    assert_string_equal(
        eval_string("local E = require('events')\n"
                    "local saved = io.stderr\n"
                    "local n = 0\n"
                    "io.stderr = { write = function() n = n + 1 end }\n"
                    "E.defaultMaxListeners = 2\n"
                    "local em = E.new()\n"
                    "for j = 1, 3 do em:on('q', function() end) end\n"
                    "E.defaultMaxListeners = 10\n"
                    "io.stderr = saved\n"
                    "return E.defaultMaxListeners .. ',' .. n"),
        "10,1");
}

static void test_events_args_contract(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local E = require('events')\n"
                    "local em = E.new()\n"
                    "local ok1 = pcall(function() em:on(42, print) end)\n"
                    "local ok2 = pcall(function() em:on('x', 'notfn') end)\n"
                    "local ok3 = pcall(function() em:setMaxListeners('x') end)\n"
                    "local ok4 = pcall(function() em:emit(1) end)\n"
                    "local ok5 = pcall(function() em:listeners({}) end)\n"
                    "return tostring(ok1) .. ',' .. tostring(ok2) .. ','\n"
                    "  .. tostring(ok3) .. ',' .. tostring(ok4) .. ','\n"
                    "  .. tostring(ok5)"),
        "false,false,false,false,false");
}

/* -- stream (batch 7) --------------------------------------------------- */

static void test_stream_readable_flow(void **state)
{
    (void)state;
    /* chunks 源: data 顺序、end→close; 挂 data 即开流 (end/close 先挂) */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local r = s.readableFromChunks{'a','b','c'}\n"
                    "local got, ev = {}, {}\n"
                    "r:on('end', function() ev[#ev+1] = 'end' end)\n"
                    "r:on('close', function() ev[#ev+1] = 'close' end)\n"
                    "r:on('data', function(c) got[#got+1] = c end)\n"
                    "return table.concat(got) .. ',' .. table.concat(ev, ',')"),
        "abc,end,close");
    /* 暂停态: 挂 data 前不投递, push 只入账发 'readable'; 挂 data 即开流
     * (end 先挂), 随后 push(nil) 走到 'end' */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local r = s.pushReadable()\n"
                    "local got, ev = {}, {}\n"
                    "r:on('readable', function() ev[#ev+1] = 'readable' end)\n"
                    "local room = r:push('x')\n"
                    "local paused = r:isPaused()\n"
                    "local n0 = #got\n"
                    "r:on('end', function() ev[#ev+1] = 'end' end)\n"
                    "r:on('data', function(c) got[#got+1] = c end)\n"
                    "local n1 = #got\n"
                    "r:push(nil)\n"
                    "return tostring(room) .. ',' .. tostring(paused) .. ','\n"
                    "  .. n0 .. ',' .. n1 .. ',' .. table.concat(ev, ',')"),
        "true,true,0,1,readable,end");
    /* 监听器里 pause 即刻生效, resume 续投 */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local r = s.readableFromChunks{'x','y','z'}\n"
                    "local n, sawFlowing\n"
                    "r:on('data', function(c)\n"
                    "  n = (n or 0) + 1\n"
                    "  if n == 1 then sawFlowing = not r:isPaused(); r:pause() end\n"
                    "end)\n"
                    "r:on('end', function() end)\n"
                    "local pausedAfterOne = r:isPaused()\n"
                    "r:resume()\n"
                    "return tostring(sawFlowing) .. ',' .. tostring(pausedAfterOne)\n"
                    "  .. ',' .. n"),
        "true,true,3");
    /* once('data') 同样开流; 字符串形态的内存块 */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local r = s.readableFromChunks('solo')\n"
                    "local got = {}\n"
                    "r:on('end', function() end)\n"
                    "r:once('data', function(c) got[#got+1] = c end)\n"
                    "return table.concat(got)"),
        "solo");
}

static void test_stream_backpressure_roundtrip(void **state)
{
    (void)state;
    /* 纯 writable: 到 hwm 返回 false, 排空发一次 'drain' */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local held\n"
                    "local w = s.writable{ highWaterMark = 4,\n"
                    "  _write = function(self, c, cb) held = cb end }\n"
                    "local drains = 0\n"
                    "w:on('drain', function() drains = drains + 1 end)\n"
                    "local r1 = w:write('aaaa')\n"
                    "local r2 = w:write('bbbb')\n"
                    "held() held()  -- 两笔各结一次, 队列空 + needdrain → drain\n"
                    "return tostring(r1) .. ',' .. tostring(r2) .. ',' .. drains"),
        "true,false,1");
    /* pipe 背压往返: 慢汇掐住源, 逐块放行使 drain 续推, 直到 finish */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local chunks = {}\n"
                    "for i = 1, 20 do chunks[i] = string.rep('k', 4) end\n"
                    "local src = s.readableFromChunks(chunks)\n"
                    "local pending, got, drains = {}, {}, 0\n"
                    "local sink = s.writable{ highWaterMark = 8,\n"
                    "  _write = function(self, c, cb)\n"
                    "    pending[#pending+1] = {chunk = c, cb = cb}\n"
                    "  end }\n"
                    "sink:on('drain', function() drains = drains + 1 end)\n"
                    "sink:on('finish', function() got.finished = true end)\n"
                    "src:pipe(sink)\n"
                    "local pausedAtStart = src:isPaused()\n"
                    "local total, guard = 0, 0\n"
                    "while not got.finished and guard < 2000 do\n"
                    "  guard = guard + 1\n"
                    "  local it = table.remove(pending, 1)\n"
                    "  if it then\n"
                    "    total = total + #it.chunk\n"
                    "    it.cb()\n"
                    "  elseif src:isPaused() and #sink._bufW == 0 then\n"
                    "    break\n"
                    "  end\n"
                    "end\n"
                    "return tostring(pausedAtStart) .. ',' .. total .. ','\n"
                    "  .. tostring(got.finished) .. ',' .. tostring(drains > 0)"),
        "true,80,true,true");
    /* unpipe: 摘钩后源的数据不再进汇, 源的 end 不再带动汇; 源暂停收场不毁 */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local src = s.pushReadable()\n"
                    "local got, ended = {}, false\n"
                    "local sink = s.writable{\n"
                    "  _write = function(self, c, cb) got[#got+1] = c; cb() end }\n"
                    "sink:on('finish', function() ended = true end)\n"
                    "sink:on('close', function() end)\n"
                    "src:pipe(sink)\n"
                    "src:push('p')\n"
                    "src:unpipe(sink)\n"
                    "src:push('q')\n"
                    "src:push(nil)\n"
                    "return #got .. ',' .. tostring(ended) .. ','\n"
                    "  .. tostring(src._destroyed == true)"),
        "1,false,false");
}

static void test_stream_error_destroy_close(void **state)
{
    (void)state;
    /* _read 炸 → error → close 联动; destroy 幂等 (close 恰一次) */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local r = s.readable{\n"
                    "  _read = function(self) error('source boom') end }\n"
                    "local ev = {}\n"
                    "r:on('error', function(e)\n"
                    "  ev[#ev+1] = tostring(e):find('source boom', 1, true)\n"
                    "    and 'err' or ('bad:' .. tostring(e)) end)\n"
                    "r:on('close', function() ev[#ev+1] = 'close' end)\n"
                    "r:on('data', function() end)\n"
                    "r:destroy('again')\n"
                    "return table.concat(ev, ',')"),
        "err,close");
    /* destroy(err) 无 error 监听 → raise (events 语义); 有监听不炸 */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local ok1, e1 = pcall(function()\n"
                    "  local w = s.writable{ _write = function() end }\n"
                    "  w:on('close', function() end)\n"
                    "  w:destroy('kaboom')\n"
                    "end)\n"
                    "local w2 = s.writable{ _write = function() end }\n"
                    "local sawErr = false\n"
                    "w2:on('error', function(e) sawErr = (e == 'kaboom') end)\n"
                    "w2:on('close', function() end)\n"
                    "w2:destroy('kaboom')\n"
                    "return tostring(ok1) .. ','\n"
                    "  .. tostring(tostring(e1):find('kaboom', 1, true) ~= nil)\n"
                    "  .. ',' .. tostring(sawErr)"),
        "false,true,true");
    /* write after end / after destroy: error 事件; 未决 write 回调 (在途一笔 +
     * 排队一笔) 都被 destroy 以 'stream destroyed' 结账, destroy 后 settle 无害 */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local held, errs, cbs = nil, {}, {}\n"
                    "local w = s.writable{\n"
                    "  _write = function(self, c, cb) held = cb end }\n"
                    "w:on('error', function(e) errs[#errs+1] = tostring(e) end)\n"
                    "w:write('a', function(e) cbs[#cbs+1] = 'a:' .. tostring(e) end)\n"
                    "w:write('b', function(e) cbs[#cbs+1] = 'b:' .. tostring(e) end)\n"
                    "w:end_()\n"
                    "pcall(function() w:write('c') end)\n"
                    "w:destroy('teardown')\n"
                    "held()\n"
                    "return table.concat(errs, ';') .. '|' .. table.concat(cbs, ';')"),
        "write after end;teardown|a:stream destroyed;b:stream destroyed");
    /* 'finish' → autoDestroy → 'close' (纯 writable 单脸即关) */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local w = s.writable{ _write = function(self, c, cb) cb() end }\n"
                    "local ev = {}\n"
                    "w:on('finish', function() ev[#ev+1] = 'finish' end)\n"
                    "w:on('close', function() ev[#ev+1] = 'close' end)\n"
                    "w:end_()\n"
                    "return table.concat(ev, ',')"),
        "finish,close");
}

static void test_stream_pipe_error_propagation(void **state)
{
    (void)state;
    /* 源 error → 目标 destroy(同错) + unpipe (与 Node 裸 pipe 的差异) */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local src = s.pushReadable()\n"
                    "local sink = s.writable{ _write = function(self, c, cb) cb() end }\n"
                    "local sev, dev = {}, {}\n"
                    "src:on('error', function(e) sev[#sev+1] = 'src:' .. tostring(e) end)\n"
                    "sink:on('error', function(e) dev[#dev+1] = 'dst:' .. tostring(e) end)\n"
                    "sink:on('close', function() dev[#dev+1] = 'close' end)\n"
                    "src:pipe(sink)\n"
                    "src:push('a')\n"
                    "src:destroy('net down')\n"
                    "return table.concat(sev, ',') .. '|' .. table.concat(dev, ',')\n"
                    "  .. '|' .. #src._pipes"),
        "src:net down|dst:net down,close|0");
    /* 目标 error → 只 unpipe, 源不炸不被 destroy (pushReadable 源不会
     * 自然收口, 保证 destroy 时 pipe 仍挂着) */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local src = s.pushReadable()\n"
                    "local sink = s.writable{ _write = function(self, c, cb) cb() end }\n"
                    "local serr, sclosed = false, false\n"
                    "sink:on('error', function(e) serr = (e == 'sink broke') end)\n"
                    "sink:on('close', function() sclosed = true end)\n"
                    "src:pipe(sink)\n"
                    "src:push('p')\n"
                    "sink:destroy('sink broke')\n"
                    "return tostring(serr) .. ',' .. tostring(sclosed) .. ','\n"
                    "  .. #src._pipes .. ',' .. tostring(src._destroyed == true)"),
        "true,true,0,false");
    /* 源 'end' 默认带 dest:end_(); opts.end == false 关掉 */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local sink = s.writable{ _write = function(self, c, cb) cb() end }\n"
                    "local fin = false\n"
                    "sink:on('finish', function() fin = true end)\n"
                    "sink:on('close', function() end)\n"
                    "s.readableFromChunks{'z'}:pipe(sink)\n"
                    "local sink2 = s.writable{ _write = function(self, c, cb) cb() end }\n"
                    "local fin2 = false\n"
                    "sink2:on('finish', function() fin2 = true end)\n"
                    "s.readableFromChunks{'z'}:pipe(sink2, {['end'] = false})\n"
                    "return tostring(fin) .. ',' .. tostring(fin2)"),
        "true,false");
}

static void test_stream_transform(void **state)
{
    (void)state;
    /* _transform 中继 + _flush 尾块; finish → end → close */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local t = s.transform{\n"
                    "  _transform = function(self, c, cb)\n"
                    "    self:push(c:upper()); cb() end,\n"
                    "  _flush = function(self, cb) self:push('!'); cb() end }\n"
                    "local out, ev = {}, {}\n"
                    "t:on('data', function(c) out[#out+1] = c end)\n"
                    "t:on('finish', function() ev[#ev+1] = 'finish' end)\n"
                    "t:on('end', function() ev[#ev+1] = 'end' end)\n"
                    "t:on('close', function() ev[#ev+1] = 'close' end)\n"
                    "t:write('a'); t:write('b'); t:end_()\n"
                    "return table.concat(out) .. ',' .. table.concat(ev, ',')"),
        "AB!,finish,end,close");
    /* 全链: chunks → transform → sink, pipe 两跳背压不断链 */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local up = s.transform{\n"
                    "  _transform = function(self, c, cb)\n"
                    "    self:push('[' .. c .. ']'); cb() end }\n"
                    "local acc, fin = {}, false\n"
                    "local sink = s.writable{\n"
                    "  _write = function(self, c, cb) acc[#acc+1] = c; cb() end }\n"
                    "sink:on('finish', function() fin = true end)\n"
                    "s.readableFromChunks{'x','y'}:pipe(up):pipe(sink)\n"
                    "return table.concat(acc) .. ',' .. tostring(fin)"),
        "[x][y],true");
    /* _transform 里 cb(err) → destroy 联动 */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local t = s.transform{\n"
                    "  _transform = function(self, c, cb) cb('bad chunk') end }\n"
                    "local ev = {}\n"
                    "t:on('error', function(e) ev[#ev+1] = 'err:' .. tostring(e) end)\n"
                    "t:on('close', function() ev[#ev+1] = 'close' end)\n"
                    "t:write('x')\n"
                    "return table.concat(ev, ',')"),
        "err:bad chunk,close");
}

static void test_stream_adapters(void **state)
{
    (void)state;
    /* duplexFromSock: write 转发、常驻读回调转 push、EOF → end,
     * 两脸到齐才 close, destroy 恰关 sock 一次 */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local writes, readcb, closed = {}, nil, 0\n"
                    "local fake = {\n"
                    "  write = function(self, d, cb)\n"
                    "    writes[#writes+1] = d; if cb then cb() end end,\n"
                    "  read = function(self, cb) readcb = cb end,\n"
                    "  close = function(self) closed = closed + 1 end }\n"
                    "local d = s.duplexFromSock(fake)\n"
                    "local got, ev = {}, {}\n"
                    "d:on('data', function(c) got[#got+1] = c end)\n"
                    "d:on('end', function() ev[#ev+1] = 'end' end)\n"
                    "d:on('finish', function() ev[#ev+1] = 'finish' end)\n"
                    "d:on('close', function() ev[#ev+1] = 'close' end)\n"
                    "d:write('hello')\n"
                    "readcb(nil, 'c1'); readcb(nil, 'c2')\n"
                    "local beforeClose = #ev\n"
                    "readcb(nil, nil)\n"
                    "d:end_()\n"
                    "d:destroy()\n"
                    "return table.concat(writes) .. ',' .. table.concat(got)\n"
                    "  .. ',' .. beforeClose .. ',' .. table.concat(ev, ',')\n"
                    "  .. ',' .. closed"),
        "hello,c1c2,0,end,finish,close,1");
    /* sock 读错误 → error → close, 且写侧未决回调被结账 */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local readcb, wclosed = nil, 0\n"
                    "local fake = {\n"
                    "  write = function(self, d, cb) cb() end,\n"
                    "  read = function(self, cb) readcb = cb end,\n"
                    "  close = function(self) wclosed = wclosed + 1 end }\n"
                    "local d = s.duplexFromSock(fake)\n"
                    "local ev = {}\n"
                    "d:on('error', function(e) ev[#ev+1] = 'err:' .. tostring(e) end)\n"
                    "d:on('close', function() ev[#ev+1] = 'close' end)\n"
                    "readcb('ECONNRESET')\n"
                    "return table.concat(ev, ',') .. ',' .. wclosed"),
        "err:ECONNRESET,close,1");
    /* pushReadable 视余量 (loop.http onData 模式): 队列回灌,
     * 慢消费下每轮只放 hwm 以内的量; 30 块全量到达后 EOF 要再拉一轮
     * (最后一轮 push 满额提前返回, eof 的 push(nil) 在下一轮 _read 才到) */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local queue, eof = {}, false\n"
                    "local r = s.pushReadable{ highWaterMark = 12,\n"
                    "  _read = function(self)\n"
                    "    while queue[1] do\n"
                    "      if not self:push(table.remove(queue, 1)) then return end\n"
                    "    end\n"
                    "    if eof then self:push(nil) end\n"
                    "  end }\n"
                    "local function onData(chunk) queue[#queue+1] = chunk end\n"
                    "for i = 1, 30 do onData(('c'):rep(4)) end\n"
                    "eof = true\n"
                    "local collected = {}\n"
                    "r:on('data', function(c)\n"
                    "  collected[#collected+1] = c\n"
                    "  r:pause()\n"
                    "end)\n"
                    "r:on('end', function() collected.eof = true end)\n"
                    "r:resume()\n"
                    "local guard = 0\n"
                    "while (#collected < 30 or not collected.eof) and guard < 3000 do\n"
                    "  guard = guard + 1\n"
                    "  r:resume()\n"
                    "end\n"
                    "return #collected .. ',' .. tostring(collected.eof) .. ','\n"
                    "  .. tostring(guard < 3000)"),
        "30,true,true");
}

static void test_stream_args_contract(void **state)
{
    (void)state;
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local o1 = pcall(s.readable, {_read = 42})\n"
                    "local o2 = pcall(s.writable, {_write = 'x'})\n"
                    "local o3 = pcall(s.transform, {})\n"
                    "local o4 = pcall(s.duplexFromSock, 'notasock')\n"
                    "local r = s.readableFromChunks{'a'}\n"
                    "local o5 = pcall(function() r:pipe({}) end)\n"
                    "local o6 = pcall(s.readable, {highWaterMark = 'x'})\n"
                    "local o7 = pcall(function() r:pipe(42) end)\n"
                    "local o8 = pcall(s.readableFromChunks, 42)\n"
                    "return tostring(o1) .. ',' .. tostring(o2) .. ','\n"
                    "  .. tostring(o3) .. ',' .. tostring(o4) .. ','\n"
                    "  .. tostring(o5) .. ',' .. tostring(o6) .. ','\n"
                    "  .. tostring(o7) .. ',' .. tostring(o8)"),
        "false,false,false,false,false,false,false,false");
}

/* 边腿收口: 记账/结账/幂等/适配器错误面 (主路径用例之外的暗腿) */
static void test_stream_edge_legs(void **state)
{
    (void)state;
    /* writable: 非串块按 1 记账、write cb 成功结账、end_ 尾块 + finish
     * 回调、destroy 后 end_ 静默、settle 幂等 */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local got, cbok = {}, false\n"
                    "local w1 = s.writable{\n"
                    "  _write = function(self, c, cb) got[#got+1] = c; cb() end }\n"
                    "w1:write(42, function() cbok = true end)\n"
                    "w1:write({t = 1})\n"
                    "local fin = false\n"
                    "w1:end_('tail', function() fin = true end)\n"
                    "local w2 = s.writable{ _write = function(self, c, cb) cb() end }\n"
                    "w2:on('close', function() end)\n"
                    "w2:destroy()\n"
                    "local late = pcall(function() return w2:end_('late') end)\n"
                    "local cnt, held = 0\n"
                    "local w3 = s.writable{\n"
                    "  _write = function(self, c, cb) held = cb; cnt = cnt + 1 end }\n"
                    "w3:write('a')\n"
                    "held() held()\n"
                    "return tostring(#got) .. ',' .. tostring(got[1] == 42)\n"
                    "  .. ',' .. tostring(cbok) .. ','\n"
                    "  .. tostring(got[2] ~= nil and got[2].t == 1) .. ','\n"
                    "  .. tostring(got[3] == 'tail') .. ',' .. tostring(fin)\n"
                    "  .. ',' .. tostring(late) .. ',' .. cnt"),
        "3,true,true,true,true,true,true,1");
    /* 错误结账面: _write cb(err) → destroy、_write 本身 raise、
     * destroy 后 write 发 error 事件 */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local e1 = {}\n"
                    "local w1 = s.writable{\n"
                    "  _write = function(self, c, cb) cb('werr') end }\n"
                    "w1:on('error', function(e) e1[#e1+1] = tostring(e) end)\n"
                    "w1:on('close', function() end)\n"
                    "w1:write('x')\n"
                    "local e2 = {}\n"
                    "local w2 = s.writable{ _write = function() error('sink boom') end }\n"
                    "w2:on('error', function(e) e2[#e2+1] = tostring(e) end)\n"
                    "w2:on('close', function() end)\n"
                    "pcall(function() w2:write('y') end)\n"
                    "local e3, wr = {}, nil\n"
                    "local w3 = s.writable{ _write = function(self, c, cb) cb() end }\n"
                    "w3:on('error', function(e) e3[#e3+1] = tostring(e) end)\n"
                    "w3:on('close', function() end)\n"
                    "w3:destroy()\n"
                    "wr = w3:write('z')\n"
                    "return table.concat(e1, ',') .. '|'\n"
                    "  .. tostring(e2[1]:find('sink boom', 1, true) ~= nil) .. '|'\n"
                    "  .. table.concat(e3, ',') .. ',' .. tostring(wr)"),
        "werr|true|write after destroy,false");
    /* readable 边腿: destroy 后 push 静默 false、EOF 后 push 发 error、
     * 干涸 _read 停转不炸、双 pause / 流动中 resume 早退 */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local r1 = s.pushReadable()\n"
                    "r1:on('close', function() end)\n"
                    "r1:destroy()\n"
                    "local pd = r1:push('x')\n"
                    "local r2 = s.pushReadable()\n"
                    "local eoferr = nil\n"
                    "r2:on('error', function(e) eoferr = tostring(e) end)\n"
                    "r2:push('a') r2:push(nil) r2:push('b')\n"
                    "local dry = true\n"
                    "local r3 = s.readable{ _read = function(self) end }\n"
                    "r3:on('data', function() dry = false end)\n"
                    "local r4 = s.readableFromChunks{'a','b'}\n"
                    "r4:pause() r4:pause()\n"
                    "local r5 = s.readableFromChunks{'a'}\n"
                    "r5:on('data', function() end)\n"
                    "r5:resume()\n"
                    "return tostring(pd == false) .. ','\n"
                    "  .. tostring(eoferr == 'push after EOF') .. ','\n"
                    "  .. tostring(dry and r3._started) .. ','\n"
                    "  .. tostring(r4._flowing == false) .. ','\n"
                    "  .. tostring(r5._flowing == true)"),
        "true,true,true,true,true");
    /* transform 边腿: hwm 记账 drain、write cb 成功、_transform raise、
     * settle 幂等 (cb 双发只结一次) + destroyed 后 settle 无害、
     * flush cb(err)/raise、flush 内 destroy 后 cb 走 destroyed 守卫 */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local helds = {}\n"
                    "local t1 = s.transform{ highWaterMark = 4,\n"
                    "  _transform = function(self, c, cb)\n"
                    "    helds[#helds+1] = cb end }\n"
                    "local drained = false\n"
                    "t1:on('drain', function() drained = true end)\n"
                    "local ta = t1:write('aaaa')\n"
                    "local tb = t1:write('bbbb')\n"
                    "helds[1]() helds[2]()\n"
                    "local cbok = false\n"
                    "local t2 = s.transform{\n"
                    "  _transform = function(self, c, cb) cb() cb() end }\n"
                    "t2:write('q', function() cbok = true end)\n"
                    "local ev = {}\n"
                    "local t3 = s.transform{\n"
                    "  _transform = function() error('tboom') end }\n"
                    "t3:on('error', function(e) ev[#ev+1] = tostring(e) end)\n"
                    "t3:on('close', function() ev[#ev+1] = 'close' end)\n"
                    "pcall(function() t3:write('x') end)\n"
                    "local heldt\n"
                    "local t4 = s.transform{\n"
                    "  _transform = function(self, c, cb) heldt = cb end }\n"
                    "t4:on('close', function() end)\n"
                    "t4:write('x') t4:destroy() heldt()\n"
                    "local f1, f2 = nil, nil\n"
                    "local t5 = s.transform{ _transform = function(self, c, cb) cb() end,\n"
                    "  _flush = function(self, cb) cb('ferr') end }\n"
                    "t5:on('error', function(e) f1 = tostring(e) end)\n"
                    "t5:on('close', function() end)\n"
                    "t5:write('x') t5:end_()\n"
                    "local t6 = s.transform{ _transform = function(self, c, cb) cb() end,\n"
                    "  _flush = function() error('fboom') end }\n"
                    "t6:on('error', function(e) f2 = tostring(e) end)\n"
                    "t6:on('close', function() end)\n"
                    "pcall(function() t6:write('x') t6:end_() end)\n"
                    "local fincnt = 0\n"
                    "local t7 = s.transform{ _transform = function(self, c, cb) cb() end,\n"
                    "  _flush = function(self, cb) cb() cb() end }\n"
                    "t7:on('finish', function() fincnt = fincnt + 1 end)\n"
                    "t7:write('x') t7:end_()\n"
                    "local closed8 = false\n"
                    "local t8 = s.transform{ _transform = function(self, c, cb) cb() end,\n"
                    "  _flush = function(self, cb) self:destroy() cb() end }\n"
                    "t8:on('close', function() closed8 = true end)\n"
                    "t8:write('x') t8:end_()\n"
                    "return tostring(ta and not tb) .. ',' .. tostring(drained)\n"
                    "  .. ',' .. tostring(cbok) .. ','\n"
                    "  .. tostring(ev[1]:find('tboom', 1, true) ~= nil\n"
                    "    and ev[2] == 'close') .. ','\n"
                    "  .. tostring(f1 == 'ferr') .. ','\n"
                    "  .. tostring(f2 and f2:find('fboom', 1, true) ~= nil)\n"
                    "  .. ',' .. tostring(fincnt == 1) .. ',' .. tostring(closed8)"),
        "true,true,true,true,true,true,true,true");
    /* duplexFromSock 边腿: 写转发错误 → destroy, destroy 后迟到的
     * 常驻读回调静默丢弃 */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local derr = nil\n"
                    "local fk1 = {\n"
                    "  write = function(self, d, cb) cb('EWRITE') end,\n"
                    "  read = function(self, cb) end,\n"
                    "  close = function(self) end }\n"
                    "local d1 = s.duplexFromSock(fk1)\n"
                    "d1:on('error', function(e) derr = tostring(e) end)\n"
                    "d1:on('close', function() end)\n"
                    "d1:write('x')\n"
                    "local readcb = nil\n"
                    "local fk2 = {\n"
                    "  write = function(self, d, cb) cb() end,\n"
                    "  read = function(self, cb) readcb = cb end,\n"
                    "  close = function(self) end }\n"
                    "local d2 = s.duplexFromSock(fk2)\n"
                    "d2:on('close', function() end)\n"
                    "d2:destroy()\n"
                    "readcb(nil, 'late')\n"
                    "return tostring(derr == 'EWRITE') .. ','\n"
                    "  .. tostring(d2._destroyed == true)"),
        "true,true");
    /* opts._destroy 自身 raise: pcall 兜住, 警告写 io.stderr (本体替换捕获) */
    assert_string_equal(
        eval_string("local s = require('stream')\n"
                    "local saved, warned = io.stderr, nil\n"
                    "io.stderr = { write = function(_, m) warned = m end }\n"
                    "local closed = false\n"
                    "local ok = pcall(function()\n"
                    "  local r = s.readable{ _read = function(self) end,\n"
                    "    _destroy = function(self) error('zzz') end }\n"
                    "  r:on('close', function() closed = true end)\n"
                    "  r:destroy()\n"
                    "end)\n"
                    "io.stderr = saved\n"
                    "return tostring(ok) .. ',' .. tostring(closed) .. ','\n"
                    "  .. tostring(warned ~= nil\n"
                    "    and warned:find('stream: _destroy error', 1, true) ~= nil)"),
        "true,true,true");
}

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
        cmocka_unit_test_setup_teardown(test_install_twice_reports_already_there, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_relative_init_convention_and_miss, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_unloadable_entry_and_empty_name_fall_through, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_manifest_main_without_lua_suffix_variants, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_relative_source_caller_absolutizes_via_lfs, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_start_dir_falls_back_without_lfs, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_rocks_module_needs_dkjson, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_rocks_vendor_legs_die_before_luarocks, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_csv_decode_basics_and_eols, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_csv_quoted_fields_rfc4180, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_csv_headers_and_delimiter, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_csv_encode_basics, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_csv_encode_headers_mapping, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_csv_roundtrip_and_lines, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_ini_decode_basics, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_ini_quoted_values, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_ini_duplicates_and_cast, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_ini_error_shapes, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_ini_encode_and_roundtrip, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_toml_decode_scalars, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_toml_decode_multiline_and_unicode, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_toml_decode_structures, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_toml_decode_datetimes, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_toml_decode_errors, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_toml_encode_exact_shapes, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_toml_encode_roundtrip, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_toml_encode_errors, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_yaml_decode_scalar_family, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_yaml_decode_yaml11_numbers, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_yaml_decode_explicit_tags, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_yaml_decode_block_scalars, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_yaml_decode_flow_collections, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_yaml_decode_multi_document, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_yaml_decode_all_opts_and_errors, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_yaml_decode_errors, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_yaml_null_semantics, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_yaml_anchors_share_references, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_yaml_decode_merge_keys, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_yaml_encode_exact_shapes, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_yaml_encode_roundtrip, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_yaml_encode_errors, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_yaml_args_contract, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_xml_decode_dom_shape, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_xml_decode_entities_cdata, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_xml_decode_namespaces, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_xml_decode_errors, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_xml_encode_exact_shapes, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_xml_encode_roundtrip, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_xml_encode_errors, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_xml_sax_streaming, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_xml_args_contract, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_path_normalize_join, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_path_resolve_relative, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_path_dirname_basename_extname, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_path_parse_format, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_path_args_contract, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_util_format_conversions, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_util_format_no_fmt_and_extras, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_util_inspect_shapes, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_util_inspect_truncation_circular, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_util_args_contract, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_events_dispatch, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_events_builtin_events, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_events_error_and_maxlisteners, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_events_args_contract, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_stream_readable_flow, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_stream_backpressure_roundtrip, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_stream_error_destroy_close, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_stream_pipe_error_propagation, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_stream_transform, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_stream_adapters, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_stream_args_contract, setup_modules, teardown_modules),
        cmocka_unit_test_setup_teardown(test_stream_edge_legs, setup_modules, teardown_modules),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
