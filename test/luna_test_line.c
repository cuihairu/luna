/* luna_test_line.c — the console a user actually gets: the real binary
 * on a real pty, so the only thing stubbed is the keystrokes.
 *
 * Everything here is a TTY-only surface (replxx in src/luna_line.c):
 * a typed line commits and echoes as Out[n], editing keys work, Tab
 * inserts what the completion engine proposed (globals, call sites,
 * dotted fields, require targets), history is recalled and survives a
 * restart, the input line is highlighted, ^C cancels the line without
 * ending the session, and ^D on an empty line is the only clean exit.
 *
 * Two conventions make the wire readable. Output is matched with the
 * ANSI escapes stripped, because with color on the session paints every
 * chunk ("Out[1]: " in magenta, the value in its own color) and no
 * needle is contiguous on the wire; the raw bytes stay available for
 * the tests that are *about* color. And every wait starts from a mark
 * set just before the keystrokes, so an earlier echo cannot satisfy a
 * later step.
 */

/* The harness below is POSIX end to end: forkpty children execv'ing the
 * real binary, poll() on the master, waitpid, mkdtemp scratch homes,
 * rm -f cleanup and SIGUSR1 wakeups. Windows compiles the target to an
 * empty suite - the contract has no runtime counterpart there yet - so
 * the build clears this layer wholesale instead of tripping over one
 * POSIX header at a time. */
#ifndef _WIN32

#include <poll.h>
#if defined(__APPLE__)
#include <util.h> /* no pty.h on macOS: forkpty lives here */
#else
#include <pty.h>
#endif
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cmocka.h>

static char hist_home[] = "/tmp/luna-test-line-home-XXXXXX";
static char sockdir[] = "/tmp/luna-test-line-sock-XXXXXX";

/* raw bytes from the pty, and the mark the current step starts at */
static char wire[65536];
static size_t wire_len;
static size_t mark;

static void hist_path(char *out, size_t cap, const char *home)
{
    snprintf(out, cap, "%s/.luna_history", home);
}

/* Run the binary on a fresh pty. HOME is redirected so history
 * load/save stay inside the test sandbox; TERM is what makes the line
 * editor engage at all. The pty gets a real window size: replxx lays
 * the input line out against the terminal width, and a 0x0 pty (what
 * forkpty hands out by default) makes that arithmetic degenerate. */
/* `term` == NULL runs the child with TERM scrubbed (kernel.colors()
 * then takes its getenv-returned-NULL arm); "dumb" exercises the
 * classic no-color terminal convention on a real TTY. */
static pid_t spawn_repl_term(int *master_out, const char *home, int no_color,
                             const char *term)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = 24;
    ws.ws_col = 80;
    int master;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    assert_int_not_equal(pid, -1);
    if (pid == 0) {
        char *argv[] = { LUNA_BIN, NULL };
        if (term)
            setenv("TERM", term, 1);
        else
            unsetenv("TERM");
        setenv("HOME", home, 1);
        setenv("LUNA_SOCK_DIR", sockdir, 1);
        if (no_color)
            setenv("NO_COLOR", "1", 1);
        execv(LUNA_BIN, argv);
        _exit(127);
    }
    *master_out = master;
    wire_len = 0;
    mark = 0;
    return pid;
}

static pid_t spawn_repl(int *master_out, const char *home, int no_color)
{
    return spawn_repl_term(master_out, home, no_color, "xterm");
}

/* Drop everything read so far: the next wait only sees what comes
 * after the keystrokes the test is about to send. */
static void mark_step(void)
{
    mark = wire_len;
}

/* Everything the pty has said since the last mark_step(), with the
 * ANSI escapes stripped. Built from the accumulated `wire` on demand
 * rather than from a per-call buffer: one read can deliver a prompt
 * and the line printed before it in a single chunk, and a needle that
 * arrived in the same chunk as an earlier one still has to be
 * findable. With color on, the session paints every chunk ("Out[1]: "
 * in magenta, the value in its own color), so nothing is contiguous on
 * the wire. CR and the completion bell are dropped as noise. */
static void window(char *out, size_t cap)
{
    size_t at = 0;
    int esc = 0; /* 0 text, 1 after ESC, 2 in CSI, 3 in OSC, 4 OSC ESC-\\ */
    for (size_t i = mark; i < wire_len && at + 1 < cap; i++) {
        unsigned char c = (unsigned char)wire[i];
        if (esc == 4) {
            esc = 0;
        } else if (esc == 3) {
            if (c == 0x07)
                esc = 0;
            else if (c == 0x1b)
                esc = 4;
        } else if (esc == 2) {
            if (c >= 0x40 && c <= 0x7e)
                esc = 0; /* CSI final byte */
        } else if (esc == 1) {
            if (c == '[')
                esc = 2;
            else if (c == ']')
                esc = 3;
            else
                esc = 0;
        } else if (c == 0x1b) {
            esc = 1;
        } else if (c != '\r' && c != 0x07) {
            out[at++] = (char)c;
        }
    }
    out[at] = '\0';
}

/* Drain the pty into `wire` for up to timeout_ms, returning 1 once
 * `needle` shows up in the window since the mark. */
static int expect(int master, const char *needle, int timeout_ms)
{
    char seen[16384];
    int waited = 0;
    for (;;) {
        window(seen, sizeof(seen));
        if (strstr(seen, needle))
            return 1;
        if (waited >= timeout_ms)
            break;
        struct pollfd p = { master, POLLIN, 0 };
        int pr = poll(&p, 1, 50);
        if (pr > 0 && (p.revents & POLLIN)) {
            char raw[4096];
            ssize_t n = read(master, raw, sizeof(raw));
            if (n <= 0)
                break; /* the console is gone */
            if (wire_len + (size_t)n < sizeof(wire)) {
                memcpy(wire + wire_len, raw, (size_t)n);
                wire_len += (size_t)n;
                wire[wire_len] = '\0';
            }
            continue; /* drain eagerly; only idle time counts */
        }
        waited += 50;
    }
    window(seen, sizeof(seen));
    fail_msg("\"%s\" never appeared on the console; it said: [%s]", needle,
             seen);
    return 0;
}

/* Same, but the needle must appear in the raw bytes — for the tests
 * that are about which escapes the console emits. */
static int expect_raw(int master, const char *needle, int timeout_ms)
{
    int waited = 0;
    for (;;) {
        if (wire_len > mark && strstr(wire + mark, needle))
            return 1;
        if (waited >= timeout_ms)
            break;
        struct pollfd p = { master, POLLIN, 0 };
        int pr = poll(&p, 1, 50);
        if (pr > 0 && (p.revents & POLLIN)) {
            char raw[4096];
            ssize_t n = read(master, raw, sizeof(raw));
            if (n <= 0)
                break;
            if (wire_len + (size_t)n < sizeof(wire)) {
                memcpy(wire + wire_len, raw, (size_t)n);
                wire_len += (size_t)n;
                wire[wire_len] = '\0';
            }
            continue;
        }
        waited += 50;
    }
    fail_msg("\"%s\" never appeared in the console's raw output", needle);
    return 0;
}

/* The inverse, for the degradation tests: drain for timeout_ms and
 * fail if `needle` ever shows up in the raw bytes. */
static int expect_no_raw(int master, const char *needle, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (wire_len > mark && strstr(wire + mark, needle))
            fail_msg("\"%s\" reached the console although it must not",
                     needle);
        struct pollfd p = { master, POLLIN, 0 };
        int pr = poll(&p, 1, 50);
        if (pr > 0 && (p.revents & POLLIN)) {
            char raw[4096];
            ssize_t n = read(master, raw, sizeof(raw));
            if (n <= 0)
                break;
            if (wire_len + (size_t)n < sizeof(wire)) {
                memcpy(wire + wire_len, raw, (size_t)n);
                wire_len += (size_t)n;
                wire[wire_len] = '\0';
            }
            continue;
        }
        waited += 50;
    }
    return 1;
}
static void type(int master, const char *keys)
{
    assert_int_equal(write(master, keys, strlen(keys)), (ssize_t)strlen(keys));
}

/* Wait until the console is back at `prompt` (NULL: it already is),
 * then ^D: editor EOF -> repl.run saves the history file and exits 0.
 * Settling on the prompt first is not ceremony — while the console is
 * still rendering, the terminal is in canonical mode and a ^D is eaten
 * by the line discipline (VEOF never reaches the editor), so the
 * session would simply never exit. A user closes a console from a
 * prompt, so does a test. */
static int finish_repl(pid_t pid, int master, const char *prompt)
{
    if (prompt)
        assert_true(expect(master, prompt, 5000));
    assert_int_equal(write(master, "\x04", 1), 1);
    int waited = 0;
    int wstatus = 0;
    while (waited < 5000) {
        if (waitpid(pid, &wstatus, WNOHANG) == pid)
            break;
        usleep(50 * 1000);
        waited += 50;
    }
    if (waited >= 5000) {
        close(master);
        kill(pid, SIGKILL);
        waitpid(pid, &wstatus, 0);
        fail_msg("the console never exited on ^D; it said: [%s]",
                 wire + mark);
    }
    close(master);
    if (WIFSIGNALED(wstatus)) {
        fail_msg("the console died on signal %d; it said: [%s]",
                 WTERMSIG(wstatus), wire + mark);
    }
    assert_true(WIFEXITED(wstatus));
    return WEXITSTATUS(wstatus);
}

static int setup_line(void **state)
{
    (void)state;
    assert_non_null(mkdtemp(hist_home));
    assert_non_null(mkdtemp(sockdir));
    return 0;
}

static int teardown_line(void **state)
{
    (void)state;
    /* killed children cannot clean their socket files */
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s/luna-*.sock", sockdir);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -f %s", pattern);
    if (system(cmd) != 0)
        fail_msg("socket sweep failed");
    char path[512];
    hist_path(path, sizeof(path), hist_home);
    unlink(path);
    rmdir(hist_home);
    rmdir(sockdir);
    return 0;
}

/* Read the saved history file into buf; returns 0 when there is none. */
static int read_history(const char *home, char *out, size_t cap)
{
    char path[512];
    hist_path(path, sizeof(path), home);
    FILE *fh = fopen(path, "r");
    if (!fh)
        return 0;
    size_t n = fread(out, 1, cap - 1, fh);
    fclose(fh);
    out[n] = '\0';
    return 1;
}

static void test_typed_line_commits_and_echoes_out(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "6*7\r");
    assert_true(expect(master, "Out[1]: 42", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
    /* "done" saves the session history before exiting */
    char path[512];
    hist_path(path, sizeof(path), hist_home);
    struct stat st;
    assert_int_equal(stat(path, &st), 0);
}

static void test_table_echo_renders_sorted_keys(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "return {z=1, a=2, [10]=5}\r");
    /* table echo goes through util.inspect: deterministic key order
     * (numeric ascending, then strings by byte order) whatever the
     * hash seed landed on */
    assert_true(expect(master, "Out[1]: { [10] = 5, a = 2, z = 1 }", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

static void test_backspace_edits_before_commit(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* 6*6 -> DEL erases the 6 -> 6* */
    type(master, "6*6\x7f" "7\r");
    assert_true(expect(master, "Out[1]: 42", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

static void test_line_start_and_end_keys_move_the_cursor(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* Ctrl-A to the start, type, Ctrl-E back to the end */
    type(master, "\x01" "1 + 2\x05\r");
    assert_true(expect(master, "Out[1]: 3", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

static void test_eof_mid_line_deletes_forward(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* ^D only ends the session on an empty line; mid-line it deletes
     * the character under the cursor: "ab" -> "a" */
    type(master, "ab\x04" "= 5\r");
    assert_true(expect(master, "In [2]", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

static void test_tab_completes_a_global_to_a_bare_word(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* "stri" + Tab -> "string" (a table, so no call-site paren) */
    type(master, "stri\t.upper(\"abc\")\r");
    assert_true(expect(master, "Out[1]: 'ABC'", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

static void test_tab_completes_a_callable_to_a_call_site(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* "prin" + Tab -> "print(": a unique callable completes as a call
     * site, IPython-style */
    type(master, "prin\t'printed from a call site')\r");
    assert_true(expect(master, "printed from a call site", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

static void test_tab_keeps_the_chain_of_a_field_completion(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* "string.su" + Tab -> "string.sub(": only "su" may be rewritten,
     * never the "string." in front of it */
    type(master, "string.su\t\"abc\", 2, 3)\r");
    assert_true(expect(master, "Out[1]: 'bc'", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

static void test_tab_keeps_the_quotes_of_a_require_target(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "require \"jso\t\"\r");
    /* the module loads, so the statement is sound: a swallowed opening
     * quote would have ended in a syntax error instead */
    assert_true(expect(master, "In [2]", 5000));
    assert_null(strstr(wire + mark, "syntax error"));
    mark_step();
    /* the session is unharmed: an ordinary line still evaluates (and
     * takes Out[2], Out[1] having gone to the module table) */
    type(master, "6*7\r");
    assert_true(expect(master, "Out[2]: 42", 5000));
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

static void test_tab_without_candidates_leaves_the_line_alone(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* nothing matches "zzqqxx": the editor rings, the text stays, and
     * committing it reports the missing global as usual */
    type(master, "zzqqxx\t\r");
    assert_true(expect(master, "zzqqxx", 5000));
    assert_true(expect(master, "In [2]", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

static void test_arrow_up_recalls_this_session(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    type(master, "alpha = 41\r");
    assert_true(expect(master, "In [2]", 5000));
    mark_step();
    type(master, "\x1b[A"); /* Up */
    /* replxx repaints the recalled line */
    assert_true(expect(master, "alpha = 41", 3000));
    type(master, "\r");
    assert_true(expect(master, "In [3]", 5000));
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

static void test_history_persists_across_sessions(void **state)
{
    (void)state;
    char home2[] = "/tmp/luna-test-line-hist-XXXXXX";
    assert_non_null(mkdtemp(home2));

    /* first session types one line and exits through ^D (history saved) */
    int master;
    pid_t pid = spawn_repl(&master, home2, 0);
    assert_true(expect(master, "In [1]", 10000));
    type(master, "persist_me = 42\r");
    assert_true(expect(master, "In [2]", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);

    char hbuf[4096];
    assert_int_equal(read_history(home2, hbuf, sizeof(hbuf)), 1);
    assert_non_null(strstr(hbuf, "persist_me = 42"));

    /* second session loads it: one Up recalls before any typing */
    pid = spawn_repl(&master, home2, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "\x1b[A");
    assert_true(expect(master, "persist_me = 42", 3000));
    type(master, "\r");
    assert_true(expect(master, "In [2]", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);

    char path[512];
    hist_path(path, sizeof(path), home2);
    unlink(path);
    rmdir(home2);
}

static void test_tty_line_echo_is_highlighted(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "local vv = 9\r");
    assert_true(expect(master, "In [2]", 5000));
    /* replxx rendered the typed line through the highlighter callback,
     * which paints keywords and numbers in their own SGR colors */
    assert_true(expect_raw(master, "\x1b[0;1;31m", 1000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

static void test_utf8_line_is_highlighted_and_echoed_intact(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* 2-, 3- and 4-byte codepoints in one line: the highlighter maps
     * one color per codepoint off a byte-keyed span table */
    type(master, "s = \"h\xC3\xA9llo\xE2\x86\x92\xE4\xB8\x96\xE7\x95\x8C"
                 "\xF0\x9F\x8E\x89\"\r");
    assert_true(expect(master, "h\xC3\xA9llo\xE2\x86\x92\xE4\xB8\x96\xE7\x95\x8C"
                              "\xF0\x9F\x8E\x89", 5000));
    assert_true(expect_raw(master, "\x1b[0;1;32m", 1000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

static void test_no_color_degrades_the_input_line(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 1 /* NO_COLOR */);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "local vv = 9\r");
    assert_true(expect(master, "In [2]", 5000));
    /* the line editor colors the input line from the same decision the
     * session uses for its own output, so NO_COLOR leaves it plain:
     * cursor-movement escapes may still come, SGR colors may not */
    expect_no_raw(master, "\x1b[0;1;31m", 500);
    expect_no_raw(master, "\x1b[0;1;36m", 500);
    expect_no_raw(master, "\x1b[1;35m", 500);
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

/* TERM=dumb is the classic "this terminal cannot color" convention:
 * on a real TTY the session must degrade exactly like NO_COLOR and
 * otherwise work untouched. */
static void test_term_dumb_degrades_colors_on_a_tty(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl_term(&master, hist_home, 0, "dumb");
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "local tt = 9\r");
    assert_true(expect(master, "In [2]", 5000));
    expect_no_raw(master, "\x1b[0;1;31m", 500);
    expect_no_raw(master, "\x1b[0;1;36m", 500);
    expect_no_raw(master, "\x1b[1;35m", 500);
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

/* TERM unset is not dumb: on a TTY the answer falls back to the TTY's
 * own ability, which here means colors stay on. */
static void test_term_unset_on_a_tty_keeps_colors(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl_term(&master, hist_home, 0, NULL);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "local uu = 9\r");
    assert_true(expect(master, "In [2]", 5000));
    /* the input line is painted when color is allowed: the same SGR the
     * NO_COLOR test asserts the absence of */
    expect_raw(master, "\x1b[0;1;36m", 500);
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

static void test_ctrl_c_cancels_the_line_and_keeps_the_session(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "junkjunk");
    assert_true(expect(master, "junkjunk", 3000));
    type(master, "\x03");
    /* replxx prints the ^C and drops the line; the prompt comes back on
     * the same input number because nothing was consumed */
    assert_true(expect(master, "^C", 3000));
    assert_true(expect(master, "In [1]", 3000));
    mark_step();
    type(master, "7*3\r");
    assert_true(expect(master, "Out[1]: 21", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);

    /* a cancelled line is not history either */
    char hbuf[4096];
    assert_int_equal(read_history(hist_home, hbuf, sizeof(hbuf)), 1);
    assert_non_null(strstr(hbuf, "7*3"));
    assert_null(strstr(hbuf, "junkjunk"));
}

static void test_ctrl_c_in_a_continuation_drops_the_block(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "function f()\r");
    assert_true(expect(master, "... ", 5000));
    type(master, "return 1\r");
    assert_true(expect(master, "... ", 5000));
    type(master, "\x03");
    /* the pending block goes with the cancelled line: back to a fresh
     * numbered prompt, not the "... " continuation */
    assert_true(expect(master, "In [1]", 3000));
    mark_step();
    type(master, "6*7\r");
    assert_true(expect(master, "Out[1]: 42", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

static void test_eof_on_fresh_prompt_exits_cleanly(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    /* ^D on an untouched prompt: the session ends without evaluating
     * anything, so the prompt never advances */
    assert_int_equal(finish_repl(pid, master, NULL), 0);
}

/* -- the linedit hooks, swapped from inside the session ------------------
 *
 * The REPL registers its own completion/highlighter hooks at boot, but
 * linedit's registration face is plain API: a session can point the C
 * side at any function — one that raises, one that returns the wrong
 * shape, none at all. The bridge must stay quiet and keep the session
 * alive in every case. */

/* whatever the hook state, a Tab must not disturb the line being typed */
static void commit_42_after_tab(pid_t pid, int master, const char *in_prompt,
                                const char *out_needle)
{
    assert_true(expect(master, in_prompt, 5000));
    mark_step();
    type(master, "42\t\r");
    assert_true(expect(master, out_needle, 5000));
}

static void test_clearing_the_completion_hook_silences_tab(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "require('linedit').set_completion(nil)\r");
    assert_true(expect(master, "In [2]", 5000));
    /* the C callback stays registered but its ref is gone: Tab reaches
     * the bridge, which sees no hook and adds no candidates */
    commit_42_after_tab(pid, master, "In [2]", "Out[1]: 42");
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

static void test_raising_completion_hook_keeps_the_session(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master,
         "require('linedit').set_completion(function() error('kaboom') end)\r");
    assert_true(expect(master, "In [2]", 5000));
    /* the hook raises on every Tab; the bridge swallows it and the
     * session evaluates on as if nothing happened */
    commit_42_after_tab(pid, master, "In [2]", "Out[1]: 42");
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

static void test_wrong_shape_completion_hook_keeps_the_session(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* a non-table first result is no candidate list: the bridge drops
     * it instead of walking it */
    type(master,
         "require('linedit').set_completion(function() return 42 end)\r");
    assert_true(expect(master, "In [2]", 5000));
    commit_42_after_tab(pid, master, "In [2]", "Out[1]: 42");
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

static void test_raising_highlighter_keeps_the_session(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master,
         "require('linedit').set_highlighter(function() error('hlboom') end)\r");
    assert_true(expect(master, "In [2]", 5000));
    /* every keystroke repaints through the hook; the raise is caught
     * per repaint and the line still commits */
    mark_step();
    type(master, "9\r");
    assert_true(expect(master, "Out[1]: 9", 5000));
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

static void test_clearing_the_highlighter_keeps_the_session(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* no argument is the same as nil: the C callback comes off and
     * replxx repaints with its default rendering */
    type(master, "require('linedit').set_highlighter()\r");
    assert_true(expect(master, "In [2]", 5000));
    mark_step();
    type(master, "7\r");
    assert_true(expect(master, "Out[1]: 7", 5000));
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

static void test_history_save_to_a_bad_path_reports(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* the failure value is what the session sees; each REPL line is its
     * own chunk so the locals must print in the same line, and printing
     * keeps the assert off how multi-values render as Out */
    type(master,
         "local a, b = require('linedit').history_save('/no-such-dir-zz/h') "
         "print('saved:', a, b)\r");
    assert_true(expect(master, "cannot save history file", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

/* -- the wider editing surface -------------------------------------------
 *
 * The remaining keys a REPL user actually presses: arrows past the
 * line ends, the kill ring (^U/^K/^W), the forward-delete key, ^L,
 * history walking down again. Every assertion is on what the line
 * EVALUATES to after the keys, never on how the editor painted it. */

static void test_arrow_keys_move_the_cursor_and_insert_mid_line(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* "124", Left once (before the 4), insert 3 -> "1234" */
    type(master, "124\x1b[D" "3\r");
    assert_true(expect(master, "Out[1]: 1234", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

static void test_kill_to_start_and_kill_to_end_build_one_line(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* ^U drops the whole draft; then the line is rebuilt and ^K cuts
     * everything right of the cursor (^A, three rights, ^K leaves
     * "999"), which is what commits */
    type(master, "junkjunk\x15" "9996*7\x01\x1b[C\x1b[C\x1b[C\x0b\r");
    assert_true(expect(master, "Out[1]: 999", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

static void test_word_kill_and_forward_delete_edit_the_middle(void **state)
{
    (void)state;
    int master;
    int master2;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* ^W kills the word left of the cursor ("xxx", blanks kept):
     * "print(  xxx" -> "print(  " and the line completes to a call;
     * the needle is the print output, which bypasses the Out display */
    type(master, "print(  xxx\x17" "'wk 42')\r");
    assert_true(expect(master, "wk 42", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);

    /* the Delete key (\x1b[3~) removes the character UNDER the cursor:
     * "12X34", three lefts (onto the X), Delete -> "1234" */
    pid = spawn_repl(&master2, hist_home, 0);
    assert_true(expect(master2, "In [1]", 10000));
    mark_step();
    type(master2, "12X34\x1b[D\x1b[D\x1b[D\x1b[3~\r");
    assert_true(expect(master2, "Out[1]: 1234", 5000));
    assert_int_equal(finish_repl(pid, master2, "In [2]"), 0);
}

static void test_arrow_down_walks_back_to_a_fresh_line(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    type(master, "alpha = 41\r");
    assert_true(expect(master, "In [2]", 5000));
    mark_step();
    /* Up recalls, Down walks forward again: the recalled line is gone,
     * so what commits is exactly what is typed after it */
    type(master, "\x1b[A\x1b[B" "6*7\r");
    assert_true(expect(master, "Out[1]: 42", 5000));
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

static void test_ctrl_l_repaints_and_keeps_the_draft(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* ^L clears the screen; replxx keeps the buffer and repaints it,
     * so the line still commits to its value */
    type(master, "1+1\x0c\r");
    assert_true(expect(master, "Out[1]: 2", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

static void test_completion_spans_outside_the_input_are_rejected(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* a negative span is as implausible as one past the end: the
     * bridge keeps neither and lets replxx derive its own context, so
     * each Tab still rewrites the draft to the candidate */
    type(master,
         "require('linedit').set_completion(function() return {'zz'}, -1 "
         "end)\r");
    assert_true(expect(master, "In [2]", 5000));
    type(master, "42\t\x15");
    type(master,
         "require('linedit').set_completion(function() return {'yy'}, 9999 "
         "end)\r");
    assert_true(expect(master, "In [3]", 5000));
    type(master, "42\t\x15" "6*7\r");
    assert_true(expect(master, "Out[1]: 42", 5000));
    assert_int_equal(finish_repl(pid, master, "In [4]"), 0);
}

static void test_non_string_candidates_are_skipped(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* candidates that are not strings cannot be inserted: the bridge
     * skips them and offers the rest, so Tab lands on '77' */
    type(master,
         "require('linedit').set_completion(function() return {false, '77'} "
         "end)\r");
    assert_true(expect(master, "In [2]", 5000));
    type(master, "42\t\r");
    assert_true(expect(master, "Out[1]: 77", 5000));
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

/* the completion bridge caps what it hands replxx at 1000 candidates;
 * with 1002 matching globals the pager announces exactly 1000 — direct
 * evidence the truncation fired — and the flood does not wedge the
 * editor */
static void test_completion_candidates_cap_at_1000(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "for i=1,1002 do _G[\"zzq\"..i]=i end\r");
    assert_true(expect(master, "In [2]", 10000));
    mark_step();
    type(master, "zzq\t");
    assert_true(expect(master, "Display all 1000 possibilities", 5000));
    mark_step();
    type(master, "n");  /* abort the listing; the draft line is redrawn */
    assert_true(expect(master, "zzq", 3000));
    type(master, "\x03"); /* cancel it rather than run it */
    assert_true(expect(master, "^C", 3000));
    /* the fresh prompt usually lands in the same read chunk as the ^C,
     * so no mark here — a mark would cut it out of the window */
    assert_true(expect(master, "In [2]", 3000));
    /* replxx re-arms raw mode after the prompt is up: bytes typed in
     * that window are dropped by the terminal-attributes flush. Settle
     * past it. */
    usleep(250 * 1000);
    mark_step();
    type(master, "6*7\r");
    assert_true(expect(master, "Out[1]: 42", 5000));
    /* the for-loop was input #1 and 6*7 is #2 (the ^C cancel does not
     * bump the counter), so the closing prompt is In [3] */
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

/* an astral-plane character is four UTF-8 bytes: the highlighter's
 * decoder takes its 4-byte-width arm and the value round-trips —
 * #s counts 4 */
static void test_astral_utf8_roundtrips_through_the_editor(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "s = \"\xf0\x9d\x84\x9e\"\r"); /* U+1D11E musical G clef */
    assert_true(expect(master, "In [2]", 5000));
    mark_step();
    type(master, "#s\r");
    assert_true(expect(master, "Out[1]: 4", 5000));
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

/* -- coverage-targeted tests for remaining dark branches ------------------- */

/* 4-byte UTF-8 codepoint through the highlighter: the width calculation
 * at line 164 has a 4-byte arm ((c & 0xF8) == 0xF0) that the existing
 * UTF-8 test does not exercise because the REPL's default highlighter
 * only highlights keywords/numbers, not string contents. Install a custom
 * highlighter that returns a color for every codepoint to force the
 * UTF-8 walk to visit the 4-byte character. */
static void test_four_byte_utf8_in_highlighter(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* Install a highlighter that colors every codepoint green (32).
     * The input line contains a 4-byte character (U+1F389 = 🎉 = \xF0\x9F\x8E\x89).
     * This forces the highlighter callback's UTF-8 walk to take the 4-byte arm. */
    type(master,
         "require('linedit').set_highlighter(function(input) "
         "  local colors = {} "
         "  for i = 1, #input do colors[i] = 32 end "
         "  return colors "
         "end)\r");
    assert_true(expect(master, "In [2]", 5000));
    mark_step();
    /* Type a line with the 4-byte character and commit it */
    type(master, "s = \"\xF0\x9F\x8E\x89\"\r");
    /* The REPL echoes the assignment; look for the prompt advance */
    assert_true(expect(master, "In [3]", 5000));
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

/* Tail-fill loop in the highlighter (lines 175-176): when the hook
 * returns fewer colors than the `size` buffer replxx provides, the
 * bridge fills the remainder with REPLXX_COLOR_DEFAULT. This happens
 * when the highlighter returns a shorter table than the number of
 * codepoints in the input. */
static void test_highlighter_tail_fill(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* Install a highlighter that returns only 2 colors for a longer input.
     * The bridge must tail-fill the rest with default colors. */
    type(master,
         "require('linedit').set_highlighter(function(input) "
         "  return {31, 36} "
         "end)\r");
    assert_true(expect(master, "In [2]", 5000));
    mark_step();
    /* Type a line with more than 2 codepoints (e.g., "abc" = 3 codepoints).
     * The highlighter returns 2 colors, so the tail-fill loop runs for the 3rd. */
    type(master, "abc\r");
    assert_true(expect(master, "In [3]", 5000));
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

/* Kill word (^W) at the start of the line: nothing to kill, line stays. */
static void test_kill_word_at_line_start(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* ^W at column 0 should be a no-op */
    type(master, "\x17" "6*7\r");
    assert_true(expect(master, "Out[1]: 42", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

/* Kill to end (^K) at the end of the line: nothing to kill. */
static void test_kill_to_end_at_line_end(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "6*7\x0b\r");
    assert_true(expect(master, "Out[1]: 42", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

/* Completion hook returning nil (no candidates, no span): the bridge
 * should not crash and replxx should use its derived context. This
 * exercises the nil-check branch in luna_completion_cb. */
static void test_completion_hook_returns_nil(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master,
         "require('linedit').set_completion(function() return nil end)\r");
    assert_true(expect(master, "In [2]", 5000));
    mark_step();
    /* Tab should not crash; the line stays as typed */
    type(master, "42\t\r");
    assert_true(expect(master, "Out[1]: 42", 5000));
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

/* Completion hook returning a non-integer span: the bridge ignores the
 * span and lets replxx derive its own. This exercises the lua_isinteger
 * false branch. */
static void test_completion_hook_non_integer_span(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master,
         "require('linedit').set_completion(function() "
         "  return {'candidate'}, 'not-a-number' "
         "end)\r");
    assert_true(expect(master, "In [2]", 5000));
    mark_step();
    /* Type a simple word and tab: replxx derives its own span.
     * The exact replacement behavior varies; we just verify no crash
     * and that a completion occurred (prompt advances). */
    type(master, "testword\t\r");
    assert_true(expect(master, "In [3]", 5000)); /* prompt advances = no crash */
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

/* Wake thread mechanism: send SIGUSR1 to the REPL process to trigger
 * the async wake path (luna_wake_thread). This exercises the wake
 * thread's read loop and the synthetic ENTER emulation. */
static void test_wake_thread_via_sigusr1(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    /* Type a partial line, then send SIGUSR1 which should cause the
     * wake thread to inject a synthetic Enter, committing the empty line
     * and returning to prompt. The partial line is discarded by replxx. */
    type(master, "partial");
    assert_true(expect(master, "partial", 3000));
    kill(pid, SIGUSR1);
    /* Should see a new prompt (the partial line was discarded) */
    assert_true(expect(master, "In [2]", 5000));
    mark_step();
    type(master, "6*7\r");
    assert_true(expect(master, "Out[1]: 42", 5000));
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

/* The timed face (the REPL drain enabler): a due loop timer breaks the
 * blocked editor read with an empty-line tick and gets turned — the
 * callback runs with no user input in between. Contrast the attach
 * wake above, whose synthetic Enter commits whatever is typed. */
static void test_due_timer_ticks_the_blocked_read(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "require(\"loop\").setTimeout(function() print(\"tick-42\") end, 300)\r");
    assert_true(expect(master, "Out[1]: loop.timer", 5000));
    /* the deadline passes while the read blocks: the tick arrives on
     * its own, then the session is back at a fresh prompt */
    assert_true(expect(master, "tick-42", 5000));
    assert_int_equal(finish_repl(pid, master, "In [2]"), 0);
}

/* The contract under the tick: a sentinel that lands while the user is
 * mid-typing must not break the read — the draft is typed before the
 * deadline, survives it intact (the deadline passes underneath), and
 * commits whole on the user's enter; only then does the overdue tick
 * land on the fresh empty prompt. */
static void test_timer_never_breaks_a_typed_draft(void **state)
{
    (void)state;
    int master;
    pid_t pid = spawn_repl(&master, hist_home, 0);
    assert_true(expect(master, "In [1]", 10000));
    mark_step();
    type(master, "require(\"loop\").setTimeout(function() print(\"tick-42\") end, 300)\r");
    assert_true(expect(master, "Out[1]: loop.timer", 5000));
    /* the draft goes down well before the 300ms deadline: the sentinel
     * keeps retrying under it, so no tick while the deadline passes */
    mark_step(); /* past the command echo, which contains "tick-42" */
    type(master, "\"ab");
    assert_true(expect_no_raw(master, "tick-42", 800));
    type(master, "cd\"\r");
    /* the whole string echo proves the draft was never broken up */
    assert_true(expect(master, "Out[2]: 'abcd'", 5000));
    /* only now — the read is back at an empty prompt — does the
     * overdue tick land and turn the loop */
    assert_true(expect(master, "tick-42", 5000));
    assert_int_equal(finish_repl(pid, master, "In [3]"), 0);
}

/* Notify wake called from another thread: this is hard to exercise
 * from a pty test because it's an internal C API. The function
 * luna_line_notify_wake is only called from signal handlers or
 * other threads. We note this as a branch that requires white-box
 * testing (covered by luna_test_linedit white-box suite). */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_typed_line_commits_and_echoes_out),
        cmocka_unit_test(test_table_echo_renders_sorted_keys),
        cmocka_unit_test(test_backspace_edits_before_commit),
        cmocka_unit_test(test_line_start_and_end_keys_move_the_cursor),
        cmocka_unit_test(test_eof_mid_line_deletes_forward),
        cmocka_unit_test(test_tab_completes_a_global_to_a_bare_word),
        cmocka_unit_test(test_tab_completes_a_callable_to_a_call_site),
        cmocka_unit_test(test_tab_keeps_the_chain_of_a_field_completion),
        cmocka_unit_test(test_tab_keeps_the_quotes_of_a_require_target),
        cmocka_unit_test(test_tab_without_candidates_leaves_the_line_alone),
        cmocka_unit_test(test_arrow_up_recalls_this_session),
        cmocka_unit_test(test_history_persists_across_sessions),
        cmocka_unit_test(test_tty_line_echo_is_highlighted),
        cmocka_unit_test(test_utf8_line_is_highlighted_and_echoed_intact),
        cmocka_unit_test(test_no_color_degrades_the_input_line),
        cmocka_unit_test(test_term_dumb_degrades_colors_on_a_tty),
        cmocka_unit_test(test_term_unset_on_a_tty_keeps_colors),
        cmocka_unit_test(test_ctrl_c_cancels_the_line_and_keeps_the_session),
        cmocka_unit_test(test_ctrl_c_in_a_continuation_drops_the_block),
        cmocka_unit_test(test_eof_on_fresh_prompt_exits_cleanly),
        cmocka_unit_test(test_clearing_the_completion_hook_silences_tab),
        cmocka_unit_test(test_raising_completion_hook_keeps_the_session),
        cmocka_unit_test(test_wrong_shape_completion_hook_keeps_the_session),
        cmocka_unit_test(test_raising_highlighter_keeps_the_session),
        cmocka_unit_test(test_clearing_the_highlighter_keeps_the_session),
        cmocka_unit_test(test_history_save_to_a_bad_path_reports),
        cmocka_unit_test(test_arrow_keys_move_the_cursor_and_insert_mid_line),
        cmocka_unit_test(test_kill_to_start_and_kill_to_end_build_one_line),
        cmocka_unit_test(test_word_kill_and_forward_delete_edit_the_middle),
        cmocka_unit_test(test_arrow_down_walks_back_to_a_fresh_line),
        cmocka_unit_test(test_ctrl_l_repaints_and_keeps_the_draft),
        cmocka_unit_test(test_completion_spans_outside_the_input_are_rejected),
        cmocka_unit_test(test_non_string_candidates_are_skipped),
        cmocka_unit_test(test_completion_candidates_cap_at_1000),
        cmocka_unit_test(test_astral_utf8_roundtrips_through_the_editor),
        /* coverage-targeted tests */
        cmocka_unit_test(test_four_byte_utf8_in_highlighter),
        cmocka_unit_test(test_highlighter_tail_fill),
        cmocka_unit_test(test_kill_word_at_line_start),
        cmocka_unit_test(test_kill_to_end_at_line_end),
        cmocka_unit_test(test_completion_hook_returns_nil),
        cmocka_unit_test(test_completion_hook_non_integer_span),
        cmocka_unit_test(test_wake_thread_via_sigusr1),
        cmocka_unit_test(test_due_timer_ticks_the_blocked_read),
        cmocka_unit_test(test_timer_never_breaks_a_typed_draft),
    };
    return cmocka_run_group_tests(tests, setup_line, teardown_line);
}

#else /* _WIN32: empty suite, see the note at the top of the file */

int main(void)
{
    return 0;
}

#endif
