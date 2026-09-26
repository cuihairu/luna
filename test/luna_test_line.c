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
#include <poll.h>
#include <pty.h>
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
static pid_t spawn_repl(int *master_out, const char *home, int no_color)
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
        setenv("TERM", "xterm", 1);
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
    close(master);
    if (waited >= 5000) {
        kill(pid, SIGKILL);
        waitpid(pid, &wstatus, 0);
        fail_msg("the console never exited on ^D; it said: [%s]",
                 wire + mark);
    }
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

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_typed_line_commits_and_echoes_out),
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
        cmocka_unit_test(test_ctrl_c_cancels_the_line_and_keeps_the_session),
        cmocka_unit_test(test_ctrl_c_in_a_continuation_drops_the_block),
        cmocka_unit_test(test_eof_on_fresh_prompt_exits_cleanly),
        cmocka_unit_test(test_clearing_the_completion_hook_silences_tab),
        cmocka_unit_test(test_raising_completion_hook_keeps_the_session),
        cmocka_unit_test(test_wrong_shape_completion_hook_keeps_the_session),
        cmocka_unit_test(test_raising_highlighter_keeps_the_session),
        cmocka_unit_test(test_clearing_the_highlighter_keeps_the_session),
        cmocka_unit_test(test_history_save_to_a_bad_path_reports),
    };
    return cmocka_run_group_tests(tests, setup_line, teardown_line);
}
