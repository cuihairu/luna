/* luna_test_linedit.c — the replxx bridge (src/luna_line.c) exercised as
 * a module, on the paths a line editor only reaches from C.
 *
 * The bridge keeps its replxx instance, its Lua state and the two
 * callback refs in file statics, and the two replxx callbacks plus the
 * wake thread are static functions: replxx only calls them from inside
 * a live replxx_input() on a real terminal. To test the contract behind
 * them — candidates collected in order, the span a candidate stands
 * for, an erroring hook staying quiet, colors mapped per codepoint, a
 * history file that will not load — the translation unit is compiled
 * into this test instead of linked in, and the statics are driven
 * directly.
 *
 * What that deliberately does NOT cover is the user-visible editor
 * behaviour on a TTY (typed commit, Tab insert, ^C cancelling a line,
 * --no-color degrading the input line): the line group drives those on
 * a real pty through the real binary. The one exception is lline_read
 * itself, whose four outcomes (commit, ^C cancel, attach wake, EOF) are
 * pinned here by a forked child that owns a pty and reports each result
 * to a file, because they are the bridge's central contract.
 *
 * The wake channel's failure paths live here too, and they must run
 * FIRST: they need the instance not to exist yet (g_rx/g_wake_* are
 * process-wide, and the helper thread is never meant to be restarted
 * within a process).
 */
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cmocka.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

/* white-box: the bridge under test, statics included */
#include "luna_line.c"

/* the C++ view of replxx's opaque candidate list (test/replxx_completions_shim.cxx) */
replxx_completions *luna_test_completions_new(void);
void luna_test_completions_free(replxx_completions *c);
size_t luna_test_completions_count(const replxx_completions *c);
const char *luna_test_completions_at(const replxx_completions *c, size_t i);

static lua_State *L;
static char home[] = "/tmp/luna-test-linedit-home-XXXXXX";
static char resfile[512];
static char self_path[4096]; /* argv[0]: the child re-executes this */

static void run(lua_State *l, const char *code)
{
    if (luaL_dostring(l, code) != LUA_OK) {
        fail_msg("lua error: %s", lua_tostring(l, -1));
    }
}

/* run `fmt` with its single %s filled in by a scratch path */
static void run_path(const char *fmt, const char *path)
{
    char code[2048];
    snprintf(code, sizeof(code), fmt, path);
    run(L, code);
}

/* set the completion hook to a function with `body` (may return early) */
static void completion_hook(const char *body)
{
    char code[1024];
    snprintf(code, sizeof(code),
             "LE.set_completion(function(line) %s end)", body);
    run(L, code);
}

static void highlighter_hook(const char *body)
{
    char code[1024];
    snprintf(code, sizeof(code),
             "LE.set_highlighter(function(line) %s end)", body);
    run(L, code);
}

/* What one pass of the completion hook left behind. */
typedef struct {
    int ctx;        /* context length: replxx's in, the reported span out */
    size_t n;       /* candidates collected */
    char first[64]; /* the first one, for spot checks */
} hook_result;

static void run_completion_hook(const char *input, int ctx_in,
                                hook_result *out)
{
    replxx_completions *c = luna_test_completions_new();
    int ctx = ctx_in;
    luna_completion_cb(input, c, &ctx, NULL);
    out->ctx = ctx;
    out->n = luna_test_completions_count(c);
    out->first[0] = '\0';
    if (out->n > 0) {
        snprintf(out->first, sizeof(out->first), "%s",
                 luna_test_completions_at(c, 0));
    }
    luna_test_completions_free(c);
}

/* The color map the highlighter hook produced for `input`, read back as
 * a comma list: the color number, or "default" where the hook left no
 * span (REPLXX_COLOR_DEFAULT, i.e. the terminal's own color). */
static void run_highlight_hook(const char *input, int size, char *out,
                               size_t outsz)
{
    ReplxxColor *colors =
        size > 0 ? calloc((size_t)size, sizeof(ReplxxColor)) : NULL;
    luna_highlight_cb(input, colors, size, NULL);
    size_t at = 0;
    out[0] = '\0';
    for (int i = 0; i < size && at + 32 < outsz; i++) {
        char cell[16];
        if (colors[i] == REPLXX_COLOR_DEFAULT) {
            snprintf(cell, sizeof(cell), "default");
        } else {
            snprintf(cell, sizeof(cell), "%d", (int)colors[i]);
        }
        at += (size_t)snprintf(out + at, outsz - at, "%s%s", i ? "," : "",
                              cell);
    }
    free(colors);
}

static int setup_linedit(void **state)
{
    (void)state;
    assert_non_null(mkdtemp(home));
    snprintf(resfile, sizeof(resfile), "%s/results", home);

    L = luaL_newstate();
    assert_non_null(L);
    luaL_openlibs(L);
    luaL_requiref(L, "linedit", luaopen_luna_line, 1);
    lua_pop(L, 1);
    run(L, "LE = require 'linedit'");
    return 0;
}

static int teardown_linedit(void **state)
{
    (void)state;
    lua_close(L);
    L = NULL;
    unlink(resfile);
    rmdir(home);
    return 0;
}

/* -- the wake channel --------------------------------------------------
 *
 * These run before any hook is installed, so no replxx instance exists
 * yet: g_rx is NULL, g_wake_* are -1 and the helper thread has never
 * started. That is the only moment the channel's failure paths can be
 * driven in isolation. */

static void test_wake_channel_survives_a_full_descriptor_table(void **state)
{
    (void)state;
    /* start_wake_thread() fails its pipe() when no descriptor is free:
     * the attach bell then stays unavailable instead of taking the
     * session down. Forced by dropping the soft RLIMIT_NOFILE onto the
     * number of descriptors already open. */
    struct rlimit saved, tight;
    assert_int_equal(getrlimit(RLIMIT_NOFILE, &saved), 0);
    rlim_t ceiling = saved.rlim_cur;
    if (ceiling > 4096)
        ceiling = 4096;
    int open_fds = 0;
    for (rlim_t fd = 0; fd < ceiling; fd++)
        if (fcntl((int)fd, F_GETFD) != -1)
            open_fds++;
    tight = saved;
    tight.rlim_cur = (rlim_t)open_fds;
    /* nothing between the two setrlimit calls may assert: failing to
     * restore the limit would break every later open() in this group */
    if (setrlimit(RLIMIT_NOFILE, &tight) == 0) {
        g_wake_fd = -1;
        g_wake_wfd = -1;
        start_wake_thread();
        setrlimit(RLIMIT_NOFILE, &saved);
    }
    assert_int_equal(g_wake_fd, -1);
    assert_int_equal(g_wake_wfd, -1);
}

static void test_wake_thread_rings_with_no_instance_then_ends(void **state)
{
    (void)state;
    assert_null(g_rx); /* still nothing using the channel */
    int fds[2];
    assert_int_equal(pipe(fds), 0);
    g_wake_fd = fds[0];
    g_wake_wfd = fds[1];
    pthread_t tid;
    assert_int_equal(pthread_create(&tid, NULL, luna_wake_thread, NULL), 0);
    /* a bell arriving before replxx exists must be swallowed, not crash */
    assert_int_equal(write(fds[1], "w", 1), 1);
    usleep(50 * 1000);
    close(fds[1]); /* EOF: the loop ends and the thread returns */
    assert_int_equal(pthread_join(tid, NULL), 0);
    close(fds[0]);
    g_wake_fd = -1;
    g_wake_wfd = -1;
}

/* the thread count of this process, from /proc (Linux) */
static int thread_count(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    char line[256];
    int n = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "Threads:%d", &n) == 1)
            break;
    }
    fclose(f);
    return n;
}

static void test_wake_thread_failure_leaves_the_session_running(void **state)
{
    (void)state;
    /* pthread_create failing (RLIMIT_NPROC, tightened past the number
     * of processes this user already owns) costs the attach bell and
     * nothing else: the pipe is up, no helper reads it, and
     * start_wake_thread returns into a session that keeps working. */
    assert_int_equal(g_wake_fd, -1); /* no channel yet, so the guard passes */
    int before = thread_count();
    struct rlimit saved;
    assert_int_equal(getrlimit(RLIMIT_NPROC, &saved), 0);
    struct rlimit tight = saved;
    tight.rlim_cur = 1;
    int limited = (setrlimit(RLIMIT_NPROC, &tight) == 0);
    start_wake_thread();
    if (limited)
        setrlimit(RLIMIT_NPROC, &saved); /* restore before any assertion */
    int after = thread_count();
    /* the pipe exists whether or not the helper did ... */
    assert_true(g_wake_fd >= 0);
    assert_true(g_wake_wfd >= 0);
    luna_line_notify_wake(); /* ... so the bell writes into a pipe nobody reads */
    if (after == before) {
        /* no helper came up: give the two descriptors back and let the
         * next test start the channel properly */
        close(g_wake_fd);
        close(g_wake_wfd);
        g_wake_fd = -1;
        g_wake_wfd = -1;
    }
    /* root skips the RLIMIT_NPROC check in copy_process, so only hold
     * the process to "no new thread" where the limit really bit */
    if (geteuid() != 0 && limited)
        assert_int_equal(after, before);
}

static void test_wake_channel_starts_once(void **state)
{
    (void)state;
    /* the real channel, started for the rest of the group (and for the
     * reading child, which gets a fresh process anyway) */
    start_wake_thread();
    int fd = g_wake_fd;
    int wfd = g_wake_wfd;
    assert_true(fd >= 0);
    assert_true(wfd >= 0);
    start_wake_thread(); /* already running: no second pipe, no second thread */
    assert_int_equal(g_wake_fd, fd);
    assert_int_equal(g_wake_wfd, wfd);

    /* the bell with no write end set writes nowhere ... */
    g_wake_wfd = -1;
    luna_line_notify_wake();
    /* ... and with one, hands the helper its byte (g_rx is still NULL,
     * so the helper has nothing to emulate) */
    g_wake_wfd = wfd;
    luna_line_notify_wake();
    usleep(50 * 1000);
}

/* -- the completion hook ----------------------------------------------- */

static void test_candidates_are_collected_in_order(void **state)
{
    (void)state;
    completion_hook("return { 'alpha', 'beta' }");
    hook_result r;
    run_completion_hook("al", 7, &r); /* 7: replxx's own derived context */
    assert_int_equal(r.n, 2);
    assert_string_equal(r.first, "alpha");
    assert_int_equal(r.ctx, 7); /* no span reported: replxx keeps its own */
}

static void test_reported_span_rewrites_the_tail(void **state)
{
    (void)state;
    /* a dotted chain: "su" is what the candidate stands for, never the
     * whole "string.su" replxx would otherwise erase */
    completion_hook("return { 'sub(' }, 2");
    hook_result r;
    run_completion_hook("string.su", 8, &r);
    assert_int_equal(r.n, 1);
    assert_string_equal(r.first, "sub(");
    assert_int_equal(r.ctx, 2);
}

static void test_zero_span_inserts_without_erasing(void **state)
{
    (void)state;
    /* right after "obj:" there is no segment to rewrite */
    completion_hook("return { 'field' }, 0");
    hook_result r;
    run_completion_hook("obj:", 4, &r);
    assert_int_equal(r.n, 1);
    assert_int_equal(r.ctx, 0);
}

static void test_implausible_span_is_ignored(void **state)
{
    (void)state;
    /* negative, longer than the line, non-integer and non-numeric spans
     * all leave replxx's derived context in place */
    const char *bodies[] = {"return { 'x' }, -1", "return { 'x' }, 99",
                            "return { 'x' }, 1.5", "return { 'x' }, 'wide'"};
    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        completion_hook(bodies[i]);
        hook_result r;
        run_completion_hook("ab", 2, &r);
        assert_int_equal(r.n, 1);
        assert_int_equal(r.ctx, 2);
    }
}

static void test_erroring_hook_is_swallowed(void **state)
{
    (void)state;
    /* completion must never take the prompt down: a raising hook
     * contributes nothing and says nothing */
    completion_hook("error('boom')");
    hook_result r;
    run_completion_hook("al", 2, &r);
    assert_int_equal(r.n, 0);
    assert_int_equal(r.ctx, 2);
}

static void test_non_table_result_is_ignored(void **state)
{
    (void)state;
    completion_hook("return 42");
    hook_result r;
    run_completion_hook("al", 2, &r);
    assert_int_equal(r.n, 0);
}

static void test_non_string_candidates_are_skipped(void **state)
{
    (void)state;
    /* a source that leaks a boolean or a table entry costs nothing
     * (numbers do not: lua_tostring converts them, as everywhere) */
    completion_hook("return { 'beta', true, {} }");
    hook_result r;
    run_completion_hook("b", 1, &r);
    assert_int_equal(r.n, 1);
    assert_string_equal(r.first, "beta");
}

static void test_candidate_list_is_bounded(void **state)
{
    (void)state;
    /* the menu is a UI, not a dump: 1000 entries is the ceiling */
    run(L,
        "LE.set_completion(function(line)\n"
        "  local t = {}\n"
        "  for i = 1, 1200 do t[i] = 'c' .. i end\n"
        "  return t\n"
        "end)");
    hook_result r;
    run_completion_hook("c", 1, &r);
    assert_int_equal(r.n, 1000);
    assert_string_equal(r.first, "c1");
}

static void test_cleared_hook_short_circuits(void **state)
{
    (void)state;
    completion_hook("return { 'alpha' }");
    run(L, "LE.set_completion(nil)");
    /* the callback is unreachable through replxx now, but the guard is
     * what keeps a stale invocation harmless */
    hook_result r;
    run_completion_hook("al", 2, &r);
    assert_int_equal(r.n, 0);
    assert_int_equal(r.ctx, 2);
}

static void test_hook_must_be_a_function(void **state)
{
    (void)state;
    if (luaL_dostring(L, "LE.set_completion(42)") == LUA_OK) {
        fail_msg("a non-function completion hook must be refused");
    }
    assert_non_null(strstr(lua_tostring(L, -1), "function"));
    lua_pop(L, 1);
}

/* -- the highlighter hook ---------------------------------------------- */

static void test_spans_map_to_codepoints(void **state)
{
    (void)state;
    /* the hook answers with a bytepos -> color map; codepoints without
     * a span stay at the terminal default */
    highlighter_hook("return { [1] = 5, [3] = 7 }");
    char out[256];
    run_highlight_hook("abcdef", 6, out, sizeof(out));
    assert_string_equal(out, "5,default,7,default,default,default");
}

static void test_utf8_input_maps_by_codepoint(void **state)
{
    (void)state;
    /* one color per codepoint, but the table is keyed by byte: the hook
     * walks the UTF-8 and reads each codepoint's first byte. The input
     * is "a" + U+00E9 (2 bytes) + U+2192 (3) + U+1F389 (4), starting at
     * bytes 1, 3 and 6. */
    highlighter_hook("return { [1] = 1, [2] = 2, [4] = 3, [7] = 4 }");
    char out[256];
    run_highlight_hook("a\xC3\xA9\xE2\x86\x92\xF0\x9F\x8E\x89", 4, out,
                       sizeof(out));
    assert_string_equal(out, "1,2,3,4");
}

static void test_short_color_map_fills_the_rest(void **state)
{
    (void)state;
    /* a map shorter than the line leaves the tail at the default */
    highlighter_hook("return { [1] = 3 }");
    char out[256];
    run_highlight_hook("abcd", 4, out, sizeof(out));
    assert_string_equal(out, "3,default,default,default");
}

static void test_color_buffer_beyond_the_line_is_padded(void **state)
{
    (void)state;
    /* replxx can hand over more cells than the line has codepoints: the
     * tail is painted with the terminal default, never left stale */
    highlighter_hook("return { [1] = 3 }");
    char out[256];
    run_highlight_hook("abc", 5, out, sizeof(out));
    assert_string_equal(out, "3,default,default,default,default");
}

static void test_painting_stops_at_the_buffer_and_odd_bytes_step_one(void **state)
{
    (void)state;
    highlighter_hook("return { [1] = 1, [2] = 2 }");
    char out[256];
    /* fewer cells than the line: nothing is written past the buffer */
    run_highlight_hook("abcdef", 2, out, sizeof(out));
    assert_string_equal(out, "1,2");
    /* a stray continuation byte is not a lead: it counts as one
     * codepoint (width 1) instead of derailing the walk */
    highlighter_hook("return { [1] = 4, [2] = 5 }");
    run_highlight_hook("\x80" "xy", 3, out, sizeof(out));
    assert_string_equal(out, "4,5,default");
}

static void test_callbacks_decline_without_an_interpreter(void **state)
{
    (void)state;
    /* g_L is the dispatch point for both hooks: with no state to call
     * back into (an instance made, then the state gone), a stale
     * invocation must contribute nothing rather than dereference NULL */
    completion_hook("return { 'alpha' }");
    highlighter_hook("return { [1] = 5 }");
    lua_State *saved = g_L;
    g_L = NULL;
    hook_result r;
    run_completion_hook("al", 2, &r);
    assert_int_equal(r.n, 0);
    assert_int_equal(r.ctx, 2);
    char out[64];
    run_highlight_hook("abc", 3, out, sizeof(out));
    assert_string_equal(out, "0,0,0");
    g_L = saved;
}

static void test_erroring_highlighter_is_swallowed(void **state)
{
    (void)state;
    highlighter_hook("error('boom')");
    char out[256];
    /* a raising hook leaves the caller's buffer exactly as it found it
     * (0 here, from calloc): no crash, and no colors invented */
    run_highlight_hook("abc", 3, out, sizeof(out));
    assert_string_equal(out, "0,0,0");
}

static void test_highlight_guards_tolerate_nothing(void **state)
{
    (void)state;
    highlighter_hook("return { [1] = 5 }");
    char out[64];
    /* no room to paint: the guard returns before touching the buffer */
    run_highlight_hook("abc", 0, out, sizeof(out));
    assert_string_equal(out, "");
    run_highlight_hook("abc", -1, out, sizeof(out));
    assert_string_equal(out, "");
    /* and once the hook is cleared, a stale call is inert */
    run(L, "LE.set_highlighter(nil)");
    run_highlight_hook("abc", 3, out, sizeof(out));
    assert_string_equal(out, "0,0,0");
}

static void test_highlighter_must_be_a_function(void **state)
{
    (void)state;
    if (luaL_dostring(L, "LE.set_highlighter('x')") == LUA_OK) {
        fail_msg("a non-function highlighter must be refused");
    }
    assert_non_null(strstr(lua_tostring(L, -1), "function"));
    lua_pop(L, 1);
}

/* -- no-color switch --------------------------------------------------- */

static void test_no_color_switch_accepts_any_lua_boolean(void **state)
{
    (void)state;
    run(L, "LE.set_no_color(true) LE.set_no_color(false) LE.set_no_color()");
}

/* -- history ----------------------------------------------------------- */

static void test_history_round_trips_through_a_file(void **state)
{
    (void)state;
    char path[512];
    snprintf(path, sizeof(path), "%s/.luna_history", home);
    run(L, "LE.history_add('first line') LE.history_add('second line')");
    run_path("local ok, err = LE.history_save('%s') assert(ok, err)", path);
    /* replxx timestamps the file; the entries have to be in it */
    FILE *fh = fopen(path, "r");
    assert_non_null(fh);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fh);
    fclose(fh);
    buf[n] = '\0';
    assert_non_null(strstr(buf, "first line"));
    assert_non_null(strstr(buf, "second line"));
    /* and loading it back must succeed (a second load adds nothing) */
    run_path("local ok, err = LE.history_load('%s') assert(ok, err)", path);
    unlink(path);
}

static void test_history_load_of_a_missing_file_reports(void **state)
{
    (void)state;
    char missing[512];
    snprintf(missing, sizeof(missing), "%s/not-there", home);
    run_path("local ok, err = LE.history_load('%s')\n"
             "assert(ok == nil, 'expected failure')\n"
             "assert(err == 'cannot load history file', err)\n",
             missing);
}

static void test_history_save_into_a_missing_dir_reports(void **state)
{
    (void)state;
    char bad[512];
    snprintf(bad, sizeof(bad), "%s/no-such-dir/history", home);
    run_path("local ok, err = LE.history_save('%s')\n"
             "assert(ok == nil, 'expected failure')\n"
             "assert(err == 'cannot save history file', err)\n",
             bad);
}

static void test_history_add_must_get_a_string(void **state)
{
    (void)state;
    /* numbers are fine (luaL_checkstring converts them); a table is not */
    run(L, "LE.history_add(42)");
    if (luaL_dostring(L, "LE.history_add({})") == LUA_OK) {
        fail_msg("history_add must type-check its argument");
    }
    assert_non_null(strstr(lua_tostring(L, -1), "string"));
    lua_pop(L, 1);
}

/* -- read(): commit, ^C cancel, attach wake, EOF ----------------------- */

/* The four outcomes of replxx_input(), driven from a child on a pty:
 * the parent types ^C, commits a line, rings the attach bell with
 * SIGUSR1, then ^D. The child reports each read to a file, so the
 * assertions read the bridge's answers rather than the terminal's
 * echo.
 *
 * The child has to be a fresh process, not a plain fork: replxx decides
 * once and for all whether it has a terminal, in a static initializer
 * (`bool in(is_a_tty(0))` in replxx's terminal.cxx), and a fork keeps
 * whatever its parent computed. A test binary whose own stdin is a pipe
 * would see every read quietly fall back to reading piped lines. So the
 * test re-executes itself with CHILD_FLAG on the pty. */
#define CHILD_FLAG "--linedit-read-child"

static void on_sigusr1(int sig)
{
    (void)sig;
    luna_line_notify_wake();
}

static int child_main(const char *respath)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: the blocked read must see EINTR */
    sigaction(SIGUSR1, &sa, NULL);

    setenv("TERM", "xterm", 1);
    lua_State *cl = luaL_newstate();
    if (!cl) {
        return 70;
    }
    luaL_openlibs(cl);
    luaL_requiref(cl, "linedit", luaopen_luna_line, 1);
    lua_pop(cl, 1);
    lua_pushstring(cl, respath);
    lua_setglobal(cl, "RESULTS");
    if (luaL_dostring(cl,
                      "local le = require 'linedit'\n"
                      "local f = assert(io.open(RESULTS, 'a'))\n"
                      "local function rec(tag, ...)\n"
                      "  local parts = {}\n"
                      "  for i = 1, select('#', ...) do\n"
                      "    parts[#parts + 1] = tostring((select(i, ...)))\n"
                      "  end\n"
                      "  f:write(table.concat({tag, table.concat(parts, ',')},\n"
                      "    '='), '\\n')\n"
                      "  f:flush()\n"
                      "end\n"
                      "local l1, aborted = le.read('> ')\n"
                      "rec('cancel', l1, aborted)\n"
                      "rec('typed', le.read('> '))\n"
                      "rec('woken', le.read('> '))\n"
                      "local l4, err = le.read('> ')\n"
                      "rec('eof', l4, err)\n"
                      "f:close()\n") != LUA_OK) {
        /* the pty carries stderr, so the parent sees why */
        fprintf(stderr, "child lua error: %s\n", lua_tostring(cl, -1));
        return 71;
    }
    lua_close(cl);
    return 0;
}

static int wait_for_prompt(int master, const char *needle, int timeout_ms)
{
    char seen[4096];
    size_t got = 0;
    int waited = 0;
    while (waited < timeout_ms && got < sizeof(seen) - 1) {
        struct pollfd p = { master, POLLIN, 0 };
        if (poll(&p, 1, 50) > 0 && (p.revents & POLLIN)) {
            char raw[1024];
            ssize_t n = read(master, raw, sizeof(raw));
            if (n <= 0)
                break;
            for (ssize_t i = 0; i < n && got < sizeof(seen) - 1; i++) {
                seen[got++] = (raw[i] == '\r' || raw[i] == 0x07) ? ' '
                                                                  : raw[i];
            }
            seen[got] = '\0';
            if (strstr(seen, needle))
                return 1;
            continue; /* drain eagerly; only idle time counts */
        }
        waited += 50;
    }
    seen[got] = '\0';
    if (strstr(seen, needle))
        return 1;
    fail_msg("prompt \"%s\" never arrived; the console said: [%s]", needle,
             seen);
    return 0;
}

static void test_read_cancels_wakes_and_reports_eof(void **state)
{
    (void)state;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = 24;
    ws.ws_col = 80;
    int master;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    assert_int_not_equal(pid, -1);
    if (pid == 0) {
        if (master > 2)
            close(master);
        char *child_argv[] = { self_path, (char *)CHILD_FLAG, resfile, NULL };
        execv(self_path, child_argv);
        _exit(127);
    }

    /* ^C: the line is gone, the session is not */
    assert_true(wait_for_prompt(master, "> ", 5000));
    assert_int_equal(write(master, "\x03", 1), 1);
    assert_true(wait_for_prompt(master, "> ", 5000));

    /* a typed line comes back verbatim */
    assert_int_equal(write(master, "hello world\r", 12), 12);
    assert_true(wait_for_prompt(master, "> ", 5000));

    /* SIGUSR1 (the attach bell): the wake thread's synthetic Enter
     * commits an empty line, which is the REPL's poll point */
    assert_int_equal(kill(pid, SIGUSR1), 0);
    assert_true(wait_for_prompt(master, "> ", 5000));

    /* ^D on an empty line: the only true end of input */
    assert_int_equal(write(master, "\x04", 1), 1);

    int wstatus = 0;
    int waited = 0;
    while (waited < 5000) {
        if (waitpid(pid, &wstatus, WNOHANG) == pid)
            break;
        usleep(50 * 1000);
        waited += 50;
    }
    close(master);
    if (waited >= 5000) {
        kill(pid, SIGKILL);
        waitpid(pid, &wstatus, 0);
        fail_msg("the reading child never finished");
    }
    assert_true(WIFEXITED(wstatus));
    assert_int_equal(WEXITSTATUS(wstatus), 0);

    FILE *fh = fopen(resfile, "r");
    assert_non_null(fh);
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fh);
    fclose(fh);
    buf[n] = '\0';
    /* ^C -> ("", true): an empty line, flagged as cancelled */
    assert_non_null(strstr(buf, "cancel=,true\n"));
    assert_non_null(strstr(buf, "typed=hello world\n"));
    /* the attach wake -> "" without a cancel flag */
    assert_non_null(strstr(buf, "woken=\n"));
    /* ^D -> (nil, "eof") */
    assert_non_null(strstr(buf, "eof=nil,eof\n"));
}

int main(int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[1], CHILD_FLAG) == 0) {
        return child_main(argv[2]);
    }
    snprintf(self_path, sizeof(self_path), "%s", argv[0]);

    /* a closed peer must not kill the test through SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    const struct CMUnitTest tests[] = {
        /* the wake channel first: these need no replxx instance yet */
        cmocka_unit_test(test_wake_channel_survives_a_full_descriptor_table),
        cmocka_unit_test(test_wake_thread_rings_with_no_instance_then_ends),
        cmocka_unit_test(test_wake_thread_failure_leaves_the_session_running),
        cmocka_unit_test(test_wake_channel_starts_once),
        cmocka_unit_test(test_candidates_are_collected_in_order),
        cmocka_unit_test(test_reported_span_rewrites_the_tail),
        cmocka_unit_test(test_zero_span_inserts_without_erasing),
        cmocka_unit_test(test_implausible_span_is_ignored),
        cmocka_unit_test(test_erroring_hook_is_swallowed),
        cmocka_unit_test(test_non_table_result_is_ignored),
        cmocka_unit_test(test_non_string_candidates_are_skipped),
        cmocka_unit_test(test_candidate_list_is_bounded),
        cmocka_unit_test(test_cleared_hook_short_circuits),
        cmocka_unit_test(test_hook_must_be_a_function),
        cmocka_unit_test(test_spans_map_to_codepoints),
        cmocka_unit_test(test_utf8_input_maps_by_codepoint),
        cmocka_unit_test(test_short_color_map_fills_the_rest),
        cmocka_unit_test(test_color_buffer_beyond_the_line_is_padded),
        cmocka_unit_test(test_painting_stops_at_the_buffer_and_odd_bytes_step_one),
        cmocka_unit_test(test_callbacks_decline_without_an_interpreter),
        cmocka_unit_test(test_erroring_highlighter_is_swallowed),
        cmocka_unit_test(test_highlight_guards_tolerate_nothing),
        cmocka_unit_test(test_highlighter_must_be_a_function),
        cmocka_unit_test(test_no_color_switch_accepts_any_lua_boolean),
        cmocka_unit_test(test_history_round_trips_through_a_file),
        cmocka_unit_test(test_history_load_of_a_missing_file_reports),
        cmocka_unit_test(test_history_save_into_a_missing_dir_reports),
        cmocka_unit_test(test_history_add_must_get_a_string),
        /* last: it re-executes this binary on a pty */
        cmocka_unit_test(test_read_cancels_wakes_and_reports_eof),
    };
    return cmocka_run_group_tests(tests, setup_linedit, teardown_linedit);
}
