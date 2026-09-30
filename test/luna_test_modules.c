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
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
