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
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
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
 * line, and the REPL loop polls the attach socket.
 *
 * The same helper grew the timed face (the REPL drain's enabler): the
 * REPL arms a one-shot deadline (linedit.arm_timer) for how long the
 * event loop wants its backend poll to wait, and at the deadline the
 * helper sends a synthetic key instead — a sentinel code above every
 * replxx KEY_* (max ~0x00110120) and below BASE_SHIFT (0x01000000)
 * that no terminal byte stream can decode to. The sentinel's handler
 * runs on the input thread inside replxx — the one context where
 * replxx_get_state is contractually safe — and decides there: an
 * empty prompt returns RETURN, which breaks input() with an empty
 * line while bypassing replxx's commit_line entirely (no history
 * entry, no repaint beyond input()'s trailing newline) — a
 * side-effect-free tick the REPL turns the loop on. A non-empty
 * prompt means the user is mid-typing: CONTINUE leaves their text
 * untouched and re-arms the retry just past the moment they stop. */
#ifndef _WIN32
#define LUNA_TIMER_KEY 0x00800000 /* sentinel, unreachable from a tty */

static int g_wake_fd = -1;  /* read end: the wake thread blocks on it */
static int g_wake_wfd = -1; /* write end: writers poke it */
static int g_wake_thread_up = 0;
static atomic_int_fast64_t g_timer_deadline_ms; /* monotonic; 0 = disarmed */

static int64_t luna_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static void luna_poke_wake(void)
{
    if (g_wake_wfd >= 0) {
        char b = 'a'; /* "something to recompute": timer arm/disarm */
        ssize_t ignored = write(g_wake_wfd, &b, 1);
        (void)ignored;
    }
}

static void *luna_wake_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int timeout = -1;
        int64_t deadline = atomic_load(&g_timer_deadline_ms);
        if (deadline != 0) {
            int64_t left = deadline - luna_mono_ms();
            timeout = left <= 0 ? 0
                               : (left > 0x7fffffff ? 0x7fffffff
                                                    : (int)left);
        }
        struct pollfd pfd = { .fd = g_wake_fd, .events = POLLIN, .revents = 0 };
        int rc = poll(&pfd, 1, timeout);
        if (rc > 0) {
            char b;
            if (read(g_wake_fd, &b, 1) <= 0)
                break; /* write end closed: the wake channel is gone */
            {
                Replxx *rx = g_rx; /* single assignment at init; process-lived */
                /* 'w' = attach wake: an unconditional synthetic Enter
                 * (its contract — it may commit a partial line — is
                 * untouched). Any other byte is a rearm/disarm poke:
                 * fall through and recompute the poll timeout. */
                if (rx && b == 'w')
                    /* COMMIT_LINE is bound to KEY::ENTER (= control('M')),
                     * not the bare '\r' codepoint -- a bare \r just
                     * rings the bell */
                    replxx_emulate_key_press(rx, REPLXX_KEY_ENTER);
            }
        } else if (rc == 0 && atomic_exchange(&g_timer_deadline_ms, 0) != 0) {
            Replxx *rx = g_rx;
            if (rx)
                replxx_emulate_key_press(rx, LUNA_TIMER_KEY);
        }
        /* rc < 0: EINTR or transient — recompute and go again */
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
    if (pthread_create(&tid, NULL, luna_wake_thread, NULL) == 0) {
        pthread_detach(tid);
        g_wake_thread_up = 1;
    }
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

/* The timer sentinel's handler — the emptiness peek that makes the
 * timed face safe. It runs on replxx's input thread inside
 * get_input_line, the documented handler context for
 * replxx_get_state, so reading the live buffer here is race-free by
 * construction. */
#ifndef _WIN32
static ReplxxActionResult luna_timer_key_handler(int code, void *userdata)
{
    ReplxxState st;
    (void)code;
    (void)userdata;
    if (!g_rx)
        return REPLXX_ACTION_RESULT_CONTINUE;
    replxx_get_state(g_rx, &st);
    if (st.text && st.text[0] == '\0')
        return REPLXX_ACTION_RESULT_RETURN;
    /* user is mid-typing: retry shortly, never break under them */
    atomic_store(&g_timer_deadline_ms, luna_mono_ms() + 100);
    luna_poke_wake();
    return REPLXX_ACTION_RESULT_CONTINUE;
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
        replxx_bind_key(g_rx, LUNA_TIMER_KEY, luna_timer_key_handler, NULL);
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
        /* mirror the completion clear: the replxx callback stays put —
         * its C wrapper binds even a NULL fn into a callable and would
         * call the null pointer on the next repaint — and the bridge's
         * own ref guard is what silences it */
        g_highlight_ref = -1;
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
#ifndef _WIN32
    /* the one-shot is self-clearing on fire; on any other return it is
     * obsolete and the next read re-decides its own arm */
    atomic_store(&g_timer_deadline_ms, 0);
#endif
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

/* linedit.arm_timer(ms) -> armed | false
 *
 * ms >= 0: arm the one-shot — at the deadline the wake thread sends
 * its sentinel key and the blocked read breaks with an empty line
 * (only ever on an empty prompt; mid-typing retries). ms < 0: disarm.
 * Returns false where the timed face is unavailable (no wake thread —
 * Windows keeps the plain blocking read), so callers know not to wait
 * on a tick. */
static int lline_arm_timer(lua_State *L)
{
    lua_Number ms = luaL_checknumber(L, 1);
#ifndef _WIN32
    ensure_rx(L); /* binds the sentinel handler if not yet bound */
    if (ms > 2147483647.0)
        ms = 2147483647.0;
    atomic_store(&g_timer_deadline_ms,
                 ms < 0 ? 0 : luna_mono_ms() + (int64_t)ms);
    luna_poke_wake(); /* the thread recomputes its poll timeout */
    lua_pushboolean(L, g_wake_thread_up);
    return 1;
#else
    (void)ms;
    lua_pushboolean(L, 0);
    return 1;
#endif
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
    { "arm_timer", lline_arm_timer },
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
