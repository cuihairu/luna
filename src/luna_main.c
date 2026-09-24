#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "luna_kernel.h"
#include "luna_line.h"
#include "luna_lua.h" /* generated: embedded Lua entry + repl sources */

/* lpeg.c has no lpeg.h; this is its single exported entry point. */
int luaopen_lpeg(lua_State *L);
int luaopen_luna_line(lua_State *L); /* src/luna_line.c: replxx bridge */

/* stdlib module backends (batch 8): one C entry point per library. */
int luaopen_lfs(lua_State *L);        /* luafilesystem */
int luaopen_socket_core(lua_State *L); /* luasocket */
int luaopen_mime_core(lua_State *L);  /* luasocket mime */
int luaopen_zlib(lua_State *L);       /* lua-zlib */
int luaopen_socket_unix(lua_State *L); /* luasocket unix transport: attach */

#ifdef LUNA_HAVE_OPENSSL
/* luaossl: every submodule Lua layer requires "_openssl.<sub>". */
int luaopen__openssl(lua_State *L);
int luaopen__openssl_compat(lua_State *L);
int luaopen__openssl_bignum(lua_State *L);
int luaopen__openssl_pkey(lua_State *L);
int luaopen__openssl_pubkey(lua_State *L);
int luaopen__openssl_ec_group(lua_State *L);
int luaopen__openssl_x509_name(lua_State *L);
int luaopen__openssl_x509_altname(lua_State *L);
int luaopen__openssl_x509_extension(lua_State *L);
int luaopen__openssl_x509_cert(lua_State *L);
int luaopen__openssl_x509_csr(lua_State *L);
int luaopen__openssl_x509_crl(lua_State *L);
int luaopen__openssl_x509_chain(lua_State *L);
int luaopen__openssl_x509_store(lua_State *L);
/* no luaopen__openssl_x509_store_context: upstream luaossl wraps that
 * whole module in #if 0, so the symbol does not exist in the archive */
int luaopen__openssl_pkcs12(lua_State *L);
int luaopen__openssl_ssl_context(lua_State *L);
int luaopen__openssl_ssl(lua_State *L);
int luaopen__openssl_x509_verify_param(lua_State *L);
int luaopen__openssl_digest(lua_State *L);
int luaopen__openssl_hmac(lua_State *L);
int luaopen__openssl_cipher(lua_State *L);
int luaopen__openssl_kdf(lua_State *L);
int luaopen__openssl_ocsp_response(lua_State *L);
int luaopen__openssl_ocsp_basic(lua_State *L);
int luaopen__openssl_rand(lua_State *L);
int luaopen__openssl_des(lua_State *L);
#endif

#ifndef LUNA_MODULES_DIR
#define LUNA_MODULES_DIR "./luna_modules"
#endif

#ifndef LUNA_LEXERS_DIR
#define LUNA_LEXERS_DIR "./luna_modules/lexers"
#endif

#ifndef LUNA_SO_EXT
#define LUNA_SO_EXT "so"
#endif

/* Prepend the bundled module tree to package.path/cpath so require
 * finds argparse and friends before any user configuration. */
static void setup_module_paths(lua_State *L)
{
    char item[512];

    lua_getglobal(L, "package");
    if (!lua_istable(L, -1))
        return;

    const char *fields[] = { "path", "cpath", NULL };
    for (int i = 0; fields[i]; i++) {
        lua_getfield(L, -1, fields[i]);
        const char *cur = lua_tostring(L, -1);
        char extra[2048];
        snprintf(extra, sizeof(extra),
                 "%s/?.lua;%s/?/init.lua;",
                 LUNA_MODULES_DIR, LUNA_MODULES_DIR);
        if (strcmp(fields[i], "cpath") == 0)
            snprintf(item, sizeof(item), "%s/?.%s;", LUNA_MODULES_DIR, LUNA_SO_EXT);
        else
            item[0] = '\0';
        char merged[4096];
        snprintf(merged, sizeof(merged), "%s%s%s", extra, item, cur ? cur : "");
        lua_pushstring(L, merged);
        lua_setfield(L, -3, fields[i]);
        lua_pop(L, 1); /* old value */
    }
    lua_pop(L, 1); /* package */
}

static void luna_on_sigint(int sig)
{
    (void)sig;
    luna_kernel_request_interrupt();
}

/* SIGUSR1 wakes the attach poll: flag the kernel and nudge a blocked
 * line editor (see luna_line.c's wake channel) so the REPL loop comes
 * back around to serve.step() without waiting for user keystrokes */
static void luna_on_sigusr1(int sig)
{
    (void)sig;
    luna_kernel_request_serve();
    luna_line_notify_wake();
}

/* Install SIGUSR1 WITHOUT SA_RESTART: glibc's signal() would auto-restart
 * the blocked line-editor read and the wake would never surface. */
static void install_sigusr1(void)
{
#ifndef _WIN32
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = luna_on_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* EINTR is the point */
    sigaction(SIGUSR1, &sa, NULL);
#endif
}

/* arg table, same shape as the standalone interpreter:
 * arg[0] = program name, arg[1..n] = arguments. */
static void push_arg_table(lua_State *L, int argc, char **argv)
{
    lua_createtable(L, argc, 0);
    for (int i = 0; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i);
    }
    lua_setglobal(L, "arg");
}

static int dbg_msgh(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);
    fprintf(stderr, "ERR: %s\n", msg ? msg : "(?)");
    if (luaL_dostring(L,
            "local info = debug.getinfo(2, 'l')\n"
            "print('line:', info and info.currentline)\n"
            "for i = 1, 10 do\n"
            "  local n, v = debug.getlocal(2, i)\n"
            "  if not n then break end\n"
            "  print(string.format('local %d: %s = %s', i, n, tostring(v)))\n"
            "end\n") != LUA_OK) {
        fprintf(stderr, "dbg failed: %s\n", lua_tostring(L, -1));
    }
    return 1;
}

/* Register the stdlib backends into package.loaded (glob off) so the
 * pure-Lua layers — socket.lua needing socket.core, openssl/init.lua
 * needing _openssl.pkey and friends — resolve without shared objects. */
static void register_c_modules(lua_State *L)
{
    static const struct {
        const char *name;
        lua_CFunction open;
    } mods[] = {
        { "lfs", luaopen_lfs },
        { "socket.core", luaopen_socket_core },
        { "mime.core", luaopen_mime_core },
        /* unix transport for the attach channel (luna --attach) */
        { "socket.unix", luaopen_socket_unix },
        /* C core sits at zlib.core; the user-facing "zlib" module is
         * the thin wrapper in luna_modules/zlib/ (one-shot helpers on
         * top of the streaming API) */
        { "zlib.core", luaopen_zlib },
#ifdef LUNA_HAVE_OPENSSL
        { "_openssl", luaopen__openssl },
        { "_openssl.compat", luaopen__openssl_compat },
        { "_openssl.bignum", luaopen__openssl_bignum },
        { "_openssl.pkey", luaopen__openssl_pkey },
        { "_openssl.pubkey", luaopen__openssl_pubkey },
        { "_openssl.ec_group", luaopen__openssl_ec_group },
        { "_openssl.x509.name", luaopen__openssl_x509_name },
        { "_openssl.x509.altname", luaopen__openssl_x509_altname },
        { "_openssl.x509.extension", luaopen__openssl_x509_extension },
        { "_openssl.x509.cert", luaopen__openssl_x509_cert },
        { "_openssl.x509.csr", luaopen__openssl_x509_csr },
        { "_openssl.x509.crl", luaopen__openssl_x509_crl },
        { "_openssl.x509.chain", luaopen__openssl_x509_chain },
        { "_openssl.x509.store", luaopen__openssl_x509_store },
        /* x509.store_context: disabled upstream (luaossl #if 0) */
        { "_openssl.pkcs12", luaopen__openssl_pkcs12 },
        { "_openssl.ssl.context", luaopen__openssl_ssl_context },
        { "_openssl.ssl", luaopen__openssl_ssl },
        { "_openssl.x509.verify_param", luaopen__openssl_x509_verify_param },
        { "_openssl.digest", luaopen__openssl_digest },
        { "_openssl.hmac", luaopen__openssl_hmac },
        { "_openssl.cipher", luaopen__openssl_cipher },
        { "_openssl.kdf", luaopen__openssl_kdf },
        { "_openssl.ocsp.response", luaopen__openssl_ocsp_response },
        { "_openssl.ocsp.basic", luaopen__openssl_ocsp_basic },
        { "_openssl.rand", luaopen__openssl_rand },
        { "_openssl.des", luaopen__openssl_des },
#endif
        { NULL, NULL },
    };
    for (int i = 0; mods[i].name; i++) {
        luaL_requiref(L, mods[i].name, mods[i].open, 0);
        lua_pop(L, 1);
    }
}

static int run_chunk(lua_State *L, const char *src, const char *name)
{
    if (luaL_dostring(L, src) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "luna: %s: %s\n", name, msg ? msg : "(unknown error)");
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    signal(SIGINT, luna_on_sigint);
    install_sigusr1();

    lua_State *L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "luna: cannot create Lua state\n");
        return 1;
    }
    luaL_openlibs(L);


    luaL_requiref(L, "kernel", luaopen_luna_kernel, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "lpeg", luaopen_lpeg, 0);
    lua_pop(L, 1); /* registered, resolved on demand via require */
    luaL_requiref(L, "linedit", luaopen_luna_line, 0);
    lua_pop(L, 1);

    register_c_modules(L);

    setup_module_paths(L);

    /* embedded sources for the entry chunk */
    lua_pushlstring(L, LUNA_LUA_REPL, sizeof(LUNA_LUA_REPL) - 1);
    lua_setglobal(L, "__LUNA_REPL_SRC");
    lua_pushlstring(L, LUNA_LUA_COMPLETE, sizeof(LUNA_LUA_COMPLETE) - 1);
    lua_setglobal(L, "__LUNA_COMPLETE_SRC");
    lua_pushlstring(L, LUNA_LUA_INTROSPECT, sizeof(LUNA_LUA_INTROSPECT) - 1);
    lua_setglobal(L, "__LUNA_INTROSPECT_SRC");
    lua_pushlstring(L, LUNA_LUA_HIGHLIGHT, sizeof(LUNA_LUA_HIGHLIGHT) - 1);
    lua_setglobal(L, "__LUNA_HIGHLIGHT_SRC");
    lua_pushlstring(L, LUNA_LUA_MAGIC, sizeof(LUNA_LUA_MAGIC) - 1);
    lua_setglobal(L, "__LUNA_MAGIC_SRC");
    lua_pushlstring(L, LUNA_LUA_MODULES, sizeof(LUNA_LUA_MODULES) - 1);
    lua_setglobal(L, "__LUNA_MODULES_SRC");
    lua_pushlstring(L, LUNA_LUA_PLUGINS, sizeof(LUNA_LUA_PLUGINS) - 1);
    lua_setglobal(L, "__LUNA_PLUGINS_SRC");
    lua_pushlstring(L, LUNA_LUA_SERVE, sizeof(LUNA_LUA_SERVE) - 1);
    lua_setglobal(L, "__LUNA_SERVE_SRC");
    lua_pushstring(L, LUNA_LEXERS_DIR);
    lua_setglobal(L, "__LUNA_LEXERS_DIR");

    /* snapshot the initial global names so %reset keeps the runtime's
     * own environment (stdlibs, kernel, ...) and clears only user state */
    lua_newtable(L);
    int base_idx = lua_gettop(L);
    lua_pushglobaltable(L);
    lua_pushnil(L);
    while (lua_next(L, base_idx + 1) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            lua_pushvalue(L, -2);
            lua_pushboolean(L, 1);
            lua_rawset(L, base_idx);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1); /* the globals table */
    lua_setglobal(L, "__LUNA_BASE_GLOBALS");

    push_arg_table(L, argc, argv);

    int rc;
    if (run_chunk(L, LUNA_LUA_ENTRY, "=(luna)") != 0) {
        rc = 1;
    } else if (lua_isinteger(L, -1)) {
        rc = (int)lua_tointeger(L, -1);
    } else {
        rc = 0;
    }

    lua_close(L);
    return rc;
}
