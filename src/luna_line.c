/* luna_line.c — replxx bridge exposed to Lua as require "linedit".
 *
 * One process-wide replxx instance; callbacks dispatch back into Lua
 * through registry refs, with failures swallowed (line editing must
 * never take the interpreter down). The editor itself is only used on
 * a TTY — repl.run keeps its plain io.read loop for piped stdin.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "luna_line.h"

#include "replxx.h"

#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#endif

static Replxx *g_rx = NULL;
static lua_State *g_L = NULL;
static int g_completion_ref = -1; /* LUA_NOREF until set */
static int g_highlight_ref = -1;

/* ---- wake channel -------------------------------------------------
 * replxx swallows EINTR internally and has no public way to break a
 * blocked input() from the same thread a signal handler runs on (its
 * async-notify paths deliberately no-op for the input thread). So the
 * SIGUSR1 handler only writes one byte to this pipe (async-signal-
 * safe) and a dedicated helper thread turns that into
 * replxx_emulate_key_press(REPLXX_KEY_ENTER) — from *another* thread,
 * which is the one case replxx's emulate path relays through its
 * self-pipe. replxx sees a synthetic Enter, input() returns the empty
 * line, and the REPL loop polls the attach socket. */
#ifndef _WIN32
static int g_wake_fd = -1;  /* read end: the wake thread blocks on it */
static int g_wake_wfd = -1; /* write end: the signal handler writes it */

static void *luna_wake_thread(void *arg)
{
    char b;
    (void)arg;
    while (read(g_wake_fd, &b, 1) == 1) {
        Replxx *rx = g_rx; /* single assignment at init; process-lived */
        if (rx)
            /* COMMIT_LINE is bound to KEY::ENTER (= control('M')), not
             * the bare '\r' codepoint -- a bare \r just rings the bell */
            replxx_emulate_key_press(rx, REPLXX_KEY_ENTER);
    }
    return NULL;
}

static void start_wake_thread(void)
{
    int fds[2];
    pthread_t tid;
    if (g_wake_fd >= 0)
        return;
    if (pipe(fds) != 0)
        return; /* attach wake unavailable; everything else still works */
    g_wake_fd = fds[0];
    g_wake_wfd = fds[1];
    if (pthread_create(&tid, NULL, luna_wake_thread, NULL) == 0)
        pthread_detach(tid);
}

void luna_line_notify_wake(void)
{
    if (g_wake_wfd >= 0) {
        char b = 'w';
        ssize_t ignored = write(g_wake_wfd, &b, 1);
        (void)ignored;
    }
}
#else
void luna_line_notify_wake(void)
{
}
#endif

static Replxx *ensure_rx(lua_State *L)
{
    if (!g_rx) {
        g_rx = replxx_init();
        g_L = L;
        replxx_set_completion_callback(g_rx, NULL, NULL);
#ifndef _WIN32
        start_wake_thread();
#endif
    }
    return g_rx;
}

/* completion callback: fn(input) -> {insert-string, ...} [, span]
 *
 * The optional second result is how many trailing characters of the
 * input the candidates stand for (see luna.complete): candidates are
 * insert-text, so "string.su" -> "sub(" must rewrite "su" and
 * `require "jso` -> "json` must rewrite `jso`. replxx would otherwise
 * erase the whole run its own word-break characters find — swallowing
 * the `string.` and the `require "` with it — so the reported span wins
 * whenever it is a plausible length. A callback that returns only a
 * candidate list keeps replxx's derived context. */
static void luna_completion_cb(const char *input, replxx_completions *cp,
                               int *context_len, void *userdata)
{
    (void)userdata;
    if (!g_L || g_completion_ref < 0)
        return;
    int base = lua_gettop(g_L);
    lua_rawgeti(g_L, LUA_REGISTRYINDEX, g_completion_ref);
    lua_pushstring(g_L, input);
    if (lua_pcall(g_L, 1, 2, 0) != LUA_OK) {
        lua_settop(g_L, base); /* error message; stay quiet */
        return;
    }
    /* two results: the candidate table, then the span it stands for
     * (nil when the hook only returns candidates) */
    if (!lua_istable(g_L, base + 1)) {
        lua_settop(g_L, base);
        return;
    }
    if (lua_isinteger(g_L, base + 2)) {
        lua_Integer span = lua_tointeger(g_L, base + 2);
        if (span >= 0 && span <= (lua_Integer)strlen(input))
            *context_len = (int)span;
    }
    size_t n = lua_rawlen(g_L, base + 1);
    for (size_t i = 1; i <= n && i <= 1000; i++) {
        lua_rawgeti(g_L, base + 1, (int)i);
        const char *cand = lua_tostring(g_L, -1);
        if (cand)
            replxx_add_completion(cp, cand);
        lua_pop(g_L, 1);
    }
    lua_settop(g_L, base);
}

/* highlighter callback: fn(input) -> { [bytepos+1] = replxx color int }
 * replxx wants one color per unicode codepoint, lexer spans are byte
 * based — walk the UTF-8 and map each codepoint to its first byte. */
static void luna_highlight_cb(const char *input, ReplxxColor *colors,
                              int size, void *userdata)
{
    (void)userdata;
    if (!g_L || g_highlight_ref < 0 || size <= 0)
        return;
    lua_rawgeti(g_L, LUA_REGISTRYINDEX, g_highlight_ref);
    lua_pushstring(g_L, input);
    if (lua_pcall(g_L, 1, 1, 0) != LUA_OK) {
        lua_pop(g_L, 1);
        return;
    }
    size_t blen = strlen(input);
    int cp = 0;
    for (size_t b = 0; b < blen && cp < size;) {
        unsigned char c = (unsigned char)input[b];
        size_t width = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2
                       : ((c & 0xF0) == 0xE0) ? 3
                       : ((c & 0xF8) == 0xF0) ? 4
                                              : 1;
        lua_rawgeti(g_L, -1, (int)b + 1);
        if (lua_isnil(g_L, -1))
            colors[cp] = REPLXX_COLOR_DEFAULT;
        else
            colors[cp] = (ReplxxColor)lua_tointeger(g_L, -1);
        lua_pop(g_L, 1);
        b += width;
        cp++;
    }
    for (; cp < size; cp++)
        colors[cp] = REPLXX_COLOR_DEFAULT;
    lua_pop(g_L, 1);
}

/* linedit.set_completion(fn | nil) */
static int lline_set_completion(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        g_completion_ref = -1;
        return 0;
    }
    luaL_checktype(L, 1, LUA_TFUNCTION);
    ensure_rx(L);
    lua_pushvalue(L, 1);
    g_completion_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    replxx_set_completion_callback(g_rx, luna_completion_cb, NULL);
    /* Where replxx's own fallback context may not cross: whitespace,
     * the comma/colon and the brackets around a call or an index. Dots
     * and quotes are NOT breaks — a dotted chain or a quoted require
     * target keeps its context whole — while ':' stays a break so a
     * source that reports no span still rewrites only the method name.
     * When the hook does report a span (the usual case), it wins. */
    replxx_set_word_break_characters(g_rx, " \t\r\n,:()[]{}");
    return 0;
}

/* linedit.set_highlighter(fn | nil) */
static int lline_set_highlighter(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        ensure_rx(L); /* clearing before any read still needs the instance */
        g_highlight_ref = -1;
        replxx_set_highlighter_callback(g_rx, NULL, NULL);
        return 0;
    }
    luaL_checktype(L, 1, LUA_TFUNCTION);
    ensure_rx(L);
    lua_pushvalue(L, 1);
    g_highlight_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    replxx_set_highlighter_callback(g_rx, luna_highlight_cb, NULL);
    return 0;
}

/* linedit.set_no_color(on) -- the editor's own SGR output follows the
 * same decision the session makes for its chrome (--no-color /
 * NO_COLOR), so `--no-color luna` really does leave the input line
 * uncolored instead of only the echoed results. */
static int lline_set_no_color(lua_State *L)
{
    ensure_rx(L);
    replxx_set_no_color(g_rx, lua_toboolean(L, 1) ? 1 : 0);
    return 0;
}

/* linedit.read(prompt) -> line | nil, err [, aborted]
 *
 * replxx reports a cancelled line and a real end of input the same
 * way: input() returns NULL for both ^C (abort_line, which drops the
 * buffer and prints "^C") and ^D on an empty line (send_eof). Only
 * errno separates them -- abort_line() sets EAGAIN immediately before
 * it bails and nothing else on that path touches errno -- so clear it
 * before the call and read it straight after, letting a stale EAGAIN
 * from an unrelated syscall not masquerade as a cancel. A cancel is
 * NOT an end of session: the second return value tells the REPL to
 * drop the line (and any pending block) and prompt again. */
static int lline_read(lua_State *L)
{
    size_t plen;
    const char *prompt = luaL_optlstring(L, 1, "", &plen);
    Replxx *rx = ensure_rx(L);
    errno = 0;
    const char *line = replxx_input(rx, prompt);
    int why = errno;
    if (!line) {
        if (why == EAGAIN) {
            lua_pushstring(L, ""); /* discarded by replxx already */
            lua_pushboolean(L, 1);
            return 2;
        }
        lua_pushnil(L);
        lua_pushstring(L, "eof");
        return 2;
    }
    lua_pushstring(L, line);
    return 1;
}

/* linedit.history_add(line) */
static int lline_history_add(lua_State *L)
{
    const char *line = luaL_checkstring(L, 1);
    replxx_history_add(ensure_rx(L), line);
    return 0;
}

/* linedit.history_load(path) -> ok | nil, err */
static int lline_history_load(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    if (replxx_history_load(ensure_rx(L), path) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "cannot load history file");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* linedit.history_save(path) -> ok | nil, err */
static int lline_history_save(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    if (replxx_history_save(ensure_rx(L), path) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "cannot save history file");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg lline_funcs[] = {
    { "read", lline_read },
    { "set_completion", lline_set_completion },
    { "set_highlighter", lline_set_highlighter },
    { "set_no_color", lline_set_no_color },
    { "history_add", lline_history_add },
    { "history_load", lline_history_load },
    { "history_save", lline_history_save },
    { NULL, NULL },
};

int luaopen_luna_line(lua_State *L)
{
    luaL_newlib(L, lline_funcs);
    return 1;
}
