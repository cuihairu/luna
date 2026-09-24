#include "luna_kernel.h"
#include "lauxlib.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* interrupt flag                                                      */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t luna_interrupt_flag = 0;

/* serve flag: set by SIGUSR1 (or tests) so the REPL loop polls the
 * attach socket — the attach client sends the signal to interrupt the
 * blocked line editor and wake the poll */
static volatile sig_atomic_t luna_serve_flag = 0;

/* instruction ticks since the last attach poll (two hook hits apart) */
static unsigned luna_serve_ticks = 0;

void luna_kernel_request_serve(void)
{
    luna_serve_flag = 1;
}

void luna_kernel_request_interrupt(void)
{
    luna_interrupt_flag = 1;
}

/* Called by the REPL loop when it returns to the prompt: an interrupt
 * that arrived while nothing was running is discarded, bash-style. */
static int k_clear_interrupt(lua_State *L)
{
    (void)L;
    luna_interrupt_flag = 0;
    return 0;
}

/* kernel.serve_requested() -> bool: reads and clears the serve flag
 * (the Lua poll loop calls this before stepping the attach socket) */
static int k_serve_requested(lua_State *L)
{
    (void)L;
    lua_pushboolean(L, luna_serve_flag);
    luna_serve_flag = 0;
    return 1;
}

/* kernel.pid() -> integer: this process's pid (the attach socket path
 * is derived from it, and the attach client signals it with SIGUSR1) */
static int k_pid(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)getpid());
    return 1;
}

/* kernel.wake(pid): SIGUSR1 the target so its blocked line editor
 * returns empty and the REPL poll runs (attach client side). */
static int k_wake(lua_State *L)
{
    lua_Integer pid = luaL_checkinteger(L, 1);
    if (kill((pid_t)pid, SIGUSR1) != 0)
        luaL_error(L, "wake: %s", strerror(errno));
    return 0;
}

/* kernel.chmod(path, "600"): restrict the attach socket to its owner.
 * luafilesystem 1.9 ships no chmod and spawning a shell from the
 * runtime is not an option. */
static int k_chmod(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    const char *modestr = luaL_checkstring(L, 2);
    char *end;
    long m = strtol(modestr, &end, 8);
    if (end == modestr || *end != '\0' || m < 0 || m > 07777)
        luaL_error(L, "chmod: bad mode \"%s\"", modestr);
    if (chmod(path, (mode_t)m) != 0)
        luaL_error(L, "chmod: %s", strerror(errno));
    return 0;
}

/* Count hook installed while user code runs: aborts the chunk when a
 * SIGINT has been requested, and on every second hit (~200k
 * instructions) polls the attach socket. __LUNA_SERVE_STEP is
 * installed by serve.start(); the poll runs with hooks suspended and
 * this hook is reinstated on the way out, so a long script — even a
 * loop — stays reachable from `luna --attach`. */
static void luna_count_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (luna_interrupt_flag)
        luaL_error(L, "interrupted (SIGINT)");
    if (++luna_serve_ticks >= 2) {
        luna_serve_ticks = 0;
        lua_sethook(L, NULL, 0, 0); /* suspend during the nested exec */
        lua_getglobal(L, "__LUNA_SERVE_STEP");
        if (lua_isfunction(L, -1)) {
            if (lua_pcall(L, 0, 0, 0) != LUA_OK)
                lua_pop(L, 1); /* a failing poll must not kill the chunk */
        } else {
            lua_pop(L, 1);
        }
        lua_sethook(L, luna_count_hook, LUA_MASKCOUNT, 100000);
    }
}

/* ------------------------------------------------------------------ */
/* output funnel                                                       */
/* ------------------------------------------------------------------ */

/* All kernel output goes through emit(): to the Lua sink function when
 * one is registered (tests, pipe mode, plugins), else to stdout. */
static const char SINK_KEY = 'k';

static void emit(lua_State *L, const char *s, size_t n)
{
    lua_pushlightuserdata(L, (void *)&SINK_KEY);
    lua_gettable(L, LUA_REGISTRYINDEX);
    if (lua_isfunction(L, -1)) {
        lua_pushlstring(L, s, n);
        lua_call(L, 1, 0);
    } else {
        fwrite(s, 1, n, stdout);
        fflush(stdout);
    }
    lua_pop(L, 1); /* sink or nil */
}

static int k_write(lua_State *L)
{
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t len;
        const char *s = luaL_checklstring(L, i, &len);
        emit(L, s, len);
    }
    return 0;
}

/* kernel.sink(fn|nil): route output to a Lua function (receives every
 * string in order) instead of stdout. nil restores stdout. */
static int k_sink(lua_State *L)
{
    lua_pushlightuserdata(L, (void *)&SINK_KEY);
    if (lua_isnoneornil(L, 1)) {
        lua_pushnil(L);
    } else {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushvalue(L, 1);
    }
    lua_settable(L, LUA_REGISTRYINDEX);
    return 0;
}

/* print(): same formatting contract as the standard library print
 * (tab-separated tostring values, trailing newline) but routed through
 * the output funnel so captures and plugins see it too. */
static int luna_print(lua_State *L)
{
    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1)
            luaL_addlstring(&b, "\t", 1);
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_addlstring(&b, "\n", 1);
    luaL_pushresult(&b);
    size_t len;
    const char *s = lua_tolstring(L, -1, &len);
    emit(L, s, len);
    lua_pop(L, 1);
    return 0;
}

/* ------------------------------------------------------------------ */
/* chunk loading and execution                                         */
/* ------------------------------------------------------------------ */

/* Heuristic: a chunk whose last non-space byte is an operator or an
 * opening delimiter cannot end there — the input must continue. */
static int tail_looks_incomplete(const char *s, size_t len)
{
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r'))
        len--;
    if (len == 0)
        return 0;
    char c = s[len - 1];
    if (strchr("+-*/%^#=<>,.([:~", c))
        return 1;
    /* trailing bare keywords */
    static const char *const words[] = { "and", "or", "not", "return", NULL };
    for (int i = 0; words[i]; i++) {
        size_t wl = strlen(words[i]);
        if (len >= wl && strncmp(s + len - wl, words[i], wl) == 0)
            return 1;
    }
    return 0;
}

/* kernel.check(chunk) -> status: "ok" | "incomplete" | "error", msg
 *
 * "incomplete" means the chunk failed to parse because it ended too
 * early (the REPL should keep reading lines); any other syntax error is
 * a real error the user should see. */
static int k_check(lua_State *L)
{
    size_t len;
    const char *chunk = luaL_checklstring(L, 1, &len);
    const char *name = luaL_optstring(L, 2, "=(repl)");

    if (luaL_loadbufferx(L, chunk, len, name, NULL) == LUA_OK) {
        lua_pushliteral(L, "ok");
        lua_pushnil(L);
        return 2;
    }

    const char *msg = lua_tostring(L, -1);
    const char *incomplete = NULL;
    if (msg) {
        /* "<eof>" covers truncated blocks; "unfinished string"/"long
         * string" cover an unterminated quote; the tail heuristic
         * covers trailing operators like "1 +" (whose parser error
         * does not mention <eof>). */
        if (strstr(msg, "<eof>"))
            incomplete = "incomplete";
        else if (strstr(msg, "unfinished string") || strstr(msg, "unfinished long"))
            incomplete = "incomplete";
        else if (tail_looks_incomplete(chunk, len))
            incomplete = "incomplete";
    }
    if (incomplete) {
        lua_pushstring(L, incomplete);
        lua_pushnil(L);
        return 2;
    }
    lua_pushliteral(L, "error");
    lua_pushvalue(L, -2);
    return 2;
}

/* Message handler for user chunks: attaches a full traceback. */
static int luna_msghandler(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);
    if (msg == NULL) { /* is it an error object with a __tostring? */
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING)
            return 1; /* that string is the message */
        msg = "(error object is not a string)";
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

/* kernel.exec(chunk, [name], [args...]) -> ok, ... | false, errmsg
 *
 * Runs a complete chunk under the interrupt count hook; on error the
 * message carries a full traceback. Arguments after the optional name
 * become the chunk's `...`, matching the standalone interpreter's
 * script convention. */
static int k_exec(lua_State *L)
{
    size_t len;
    const char *chunk = luaL_checklstring(L, 1, &len);
    const char *name = luaL_optstring(L, 2, "=(repl)");
    if (lua_gettop(L) >= 2)
        lua_remove(L, 2); /* drop the name slot; script args shift down */
    int nargs = lua_gettop(L) - 1; /* chunk + script args */

    if (luaL_loadbufferx(L, chunk, len, name, NULL) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        if (!msg)
            msg = "unknown load error";
        lua_pushboolean(L, 0);
        lua_pushstring(L, msg);
        return 2;
    }

    /* stack: [chunk, (args...), func] -> [chunk, msgh, func, (args...)]:
     * the handler sits below the function, the function below its args,
     * exactly the layout lua_pcall(nargs) expects. */
    lua_pushcfunction(L, luna_msghandler);
    lua_insert(L, 2);
    lua_rotate(L, 3, 1);
    int msghi = 2; /* stack index of the handler */

    lua_sethook(L, luna_count_hook, LUA_MASKCOUNT, 100000);
    int status = lua_pcall(L, nargs, LUA_MULTRET, msghi);
    lua_sethook(L, NULL, 0, 0);
    luna_interrupt_flag = 0; /* consumed or stale: either way, reset */

    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        if (!msg)
            msg = "(non-string error object)";
        lua_pushboolean(L, 0);
        lua_pushstring(L, msg);
        return 2;
    }

    int nres = lua_gettop(L) - msghi; /* results above the handler */
    lua_pushboolean(L, 1);
    lua_insert(L, msghi);  /* true below the results */
    lua_remove(L, msghi + 1); /* drop the handler slot */
    return nres + 1;
}

/* ------------------------------------------------------------------ */
/* misc probes                                                         */
/* ------------------------------------------------------------------ */

/* kernel.millis() -> monotonic milliseconds (integer) */
static int k_millis(lua_State *L)
{
    struct timespec ts;
#ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    timespec_get(&ts, TIME_UTC);
#endif
    lua_Integer ms = (lua_Integer)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    lua_pushinteger(L, ms);
    return 1;
}

static int k_tty(lua_State *L)
{
    (void)L;
#ifndef _WIN32
    lua_pushboolean(L, isatty(0));
#else
    lua_pushboolean(L, 0);
#endif
    return 1;
}

/* colors allowed: interactive terminal, TERM not "dumb", and neither
 * NO_COLOR nor LUNA_NO_COLOR set (https://no-color.org). */
static int k_colors(lua_State *L)
{
#ifndef _WIN32
    int tty = isatty(1);
#else
    int tty = 0;
#endif
    const char *term = getenv("TERM");
    /* NO_COLOR / LUNA_NO_COLOR win over everything (no-color.org);
     * LUNA_COLOR=1/0 forces the answer either way — CI tests rely on it. */
    if (getenv("NO_COLOR") || getenv("LUNA_NO_COLOR")) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const char *force = getenv("LUNA_COLOR");
    if (force && *force) {
        lua_pushboolean(L, strcmp(force, "0") != 0);
        return 1;
    }
    int ok = tty && !(term && strcmp(term, "dumb") == 0);
    lua_pushboolean(L, ok);
    return 1;
}

/* kernel.umask([mask]) -> previous mask: sets the process file-mode
 * creation mask (integer, e.g. tonumber("077", 8) = 0600-created files).
 * POSIX umask has no query-only mode; serve.start() tightens it around
 * socket bind so the attach socket file is owner-only from the first
 * instant, then restores the previous mask. */
static int k_umask(lua_State *L)
{
#ifndef _WIN32
    mode_t old = umask((mode_t)luaL_optinteger(L, 1, 0077));
    lua_pushinteger(L, (lua_Integer)old);
    return 1;
#else
    (void)L;
    lua_pushinteger(L, 0);
    return 1;
#endif
}

/* kernel.version() -> string */
static int k_version(lua_State *L)
{
    lua_pushliteral(L, "luna 0.1.0");
    return 1;
}

/* ------------------------------------------------------------------ */

static const luaL_Reg kernel_funcs[] = {
    { "check", k_check },
    { "exec", k_exec },
    { "write", k_write },
    { "sink", k_sink },
    { "clear_interrupt", k_clear_interrupt },
    { "serve_requested", k_serve_requested },
    { "pid", k_pid },
    { "wake", k_wake },
    { "chmod", k_chmod },
    { "umask", k_umask },
    { "millis", k_millis },
    { "tty", k_tty },
    { "colors", k_colors },
    { "version", k_version },
    { NULL, NULL },
};

int luaopen_luna_kernel(lua_State *L)
{
    luaL_newlib(L, kernel_funcs);

    /* print() routes through the funnel from now on */
    lua_pushcfunction(L, luna_print);
    lua_setglobal(L, "print");

    return 1;
}
