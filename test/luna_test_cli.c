/* CLI launch modes, exercised through the real luna binary:
 *   script mode  (luna script.lua args)
 *   eval mode    (luna -e 'code')
 *   interactive  (piped stdin: bare luna, and luna -i script)
 * plus a --help smoke check. Each mode is its own group.
 */

/* The harness below is POSIX end to end: fork/pipe/dup2 children with
 * poll() and signal delivery, waitpid harvests, and sh command lines
 * (printf pipes, env prefixes) through popen. Windows compiles the
 * target to an empty suite - none of that has a counterpart there yet -
 * so the build clears this layer wholesale instead of tripping over one
 * POSIX header at a time. */
#ifndef _WIN32

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <cmocka.h>

#ifndef LUNA_BIN
#define LUNA_BIN "./luna"
#endif

#ifndef LUNA_FIXTURES
#define LUNA_FIXTURES "fixtures"
#endif

/* inner hang guard, from CMake: GNU timeout, or gtimeout where macOS
 * brew provides it, or empty (no guard — ctest TIMEOUT still bounds) */
#ifndef LUNA_TIMEOUT_CMD
#define LUNA_TIMEOUT_CMD "timeout"
#endif

static char outbuf[65536];
static int last_code;

/* run `LUNA_BIN <args>` through sh; stdout+stderr land in outbuf */
static int run_luna(const char *args)
{
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "%s %s 2>&1", LUNA_BIN, args);
    FILE *p = popen(cmd, "r");
    assert_non_null(p);
    size_t n = fread(outbuf, 1, sizeof(outbuf) - 1, p);
    outbuf[n] = '\0';
    int status = pclose(p);
    last_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return 0;
}

/* pipe `input` lines into luna (printf interprets the \n escapes) */
static int run_luna_piped(const char *input, const char *args)
{
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "printf '%s' | %s %s 2>&1", input, LUNA_BIN, args);
    FILE *p = popen(cmd, "r");
    assert_non_null(p);
    size_t n = fread(outbuf, 1, sizeof(outbuf) - 1, p);
    outbuf[n] = '\0';
    int status = pclose(p);
    last_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return 0;
}

/* run `env luna <args>` through sh (env prefixes like "LUNA_COLOR=1") */
static int run_luna_env(const char *env, const char *args)
{
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "%s %s %s 2>&1", env, LUNA_BIN, args);
    FILE *p = popen(cmd, "r");
    assert_non_null(p);
    size_t n = fread(outbuf, 1, sizeof(outbuf) - 1, p);
    outbuf[n] = '\0';
    int status = pclose(p);
    last_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return 0;
}

/* -- script group ------------------------------------------------------ */

static void test_script_runs_and_gets_args(void **state)
{
    (void)state;
    run_luna("'" LUNA_FIXTURES "/hello.lua' a b");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "hello from script"));
    assert_non_null(strstr(outbuf, "a\tb")); /* args become the chunk's `...` */
}

static void test_script_error_exit_code(void **state)
{
    (void)state;
    run_luna("'" LUNA_FIXTURES "/bad.lua'");
    assert_int_equal(last_code, 1);
    assert_non_null(strstr(outbuf, "boom"));
    assert_non_null(strstr(outbuf, "stack traceback"));
}

static void test_script_missing_file(void **state)
{
    (void)state;
    run_luna("'" LUNA_FIXTURES "/no_such_script.lua'");
    assert_int_equal(last_code, 1);
    assert_non_null(strstr(outbuf, "cannot open"));
}

static void test_error_object_with_raising_tostring_is_named(void **state)
{
    (void)state;
    /* a __tostring that itself dies with a non-string: the message
     * handler's fallback names the object and the traceback still
     * renders — the runner survives a hostile error object and exits
     * cleanly (script mode is the one kernel.exec caller) */
    run_luna("'" LUNA_FIXTURES "/non_string_error.lua'");
    assert_int_equal(last_code, 1);
    assert_non_null(strstr(outbuf, "(error object is not a string)"));
    assert_non_null(strstr(outbuf, "stack traceback"));
}

static void test_error_object_tostring_message_skips_traceback(void **state)
{
    (void)state;
    /* a __tostring that succeeds and returns a string takes the
     * handler's early path: the string IS the message, rendered
     * verbatim, and no traceback is attached — the contract the
     * fallback path (raising/absent tostring) inverts */
    run_luna("'" LUNA_FIXTURES "/tostring_message.lua'");
    assert_int_equal(last_code, 1);
    assert_non_null(strstr(outbuf, "tbl: via metamethod"));
    assert_null(strstr(outbuf, "stack traceback"));
    assert_null(strstr(outbuf, "(error object is not a string)"));
}

static void test_error_object_with_non_string_tostring_falls_back(void **state)
{
    (void)state;
    /* a __tostring that returns a NUMBER: the metamethod path only
     * accepts a string, so the handler falls back to naming the object
     * and the traceback renders (the mirror of the metamethod-string
     * path above, which attaches none) */
    run_luna("'" LUNA_FIXTURES "/tostring_returns_non_string.lua'");
    assert_int_equal(last_code, 1);
    assert_non_null(strstr(outbuf, "(error object is not a string)"));
    assert_non_null(strstr(outbuf, "stack traceback"));
}

/* kernel.chmod's mode argument is parsed with strtol(base 8) and every
 * malformed shape — trailing garbage, negative, over 07777 — is the
 * same "bad mode" rejection */
static void test_chmod_rejects_bad_modes(void **state)
{
    (void)state;
    run_luna("-e 'print(pcall(kernel.chmod, \"/tmp/luna-chmod-probe\", \"444x\"))"
             " print(pcall(kernel.chmod, \"/tmp/luna-chmod-probe\", \"-1\"))"
             " print(pcall(kernel.chmod, \"/tmp/luna-chmod-probe\", \"10000\"))'");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "bad mode \"444x\""));
    assert_non_null(strstr(outbuf, "bad mode \"-1\""));
    assert_non_null(strstr(outbuf, "bad mode \"10000\""));
}

/* the incompleteness contract for each surface the heuristic guards:
 * trailing blank after an operator (space/tab/CR) and the unterminated
 * long bracket, with a complete chunk as the control */
static void test_check_classifies_truncated_chunks(void **state)
{
    (void)state;
    run_luna("-e 'local s = \"\""
             " for _, c in ipairs({\"1 + \", \"1 +\\t\", \"1 +\\r\", \"x = [[\"}) do"
             " s = s .. ({kernel.check(c)})[1] .. \",\" end"
             " print(s)"
             " print(({kernel.check(\"x = 6*7\")})[1])'");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "incomplete,incomplete,incomplete,incomplete,"));
    assert_non_null(strstr(outbuf, "ok"));
}

/* the name slot is dropped and everything after it becomes the
 * chunk's `...`, matching the script convention; the bare one-argument
 * call skips the name slot entirely */
static void test_exec_passes_script_args_after_name(void **state)
{
    (void)state;
    run_luna("-e 'print(kernel.exec(\"return select(\\\"#\\\", ...), ...\","
             " \"nm\", \"a\", \"b\"))'");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "true\t2\ta\tb"));
    run_luna("-e 'print(kernel.exec(\"return 5\"))'");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "true\t5"));
}

/* -- eval group -------------------------------------------------------- */

static void test_eval_expression_echoes(void **state)
{
    (void)state;
    run_luna("-e '6*7'");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "Out[1]: 42"));
}

static void test_eval_print(void **state)
{
    (void)state;
    run_luna("-e 'print(\"hi\", 9)'");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "hi\t9"));
}

static void test_eval_error_exit_code(void **state)
{
    (void)state;
    run_luna("-e 'error(\"bad eval\")'");
    assert_int_equal(last_code, 1);
    assert_non_null(strstr(outbuf, "bad eval"));
}

static void test_eval_incomplete_is_error(void **state)
{
    (void)state;
    /* unlike the REPL there is no next line to continue with */
    run_luna("-e 'if true then'");
    assert_int_equal(last_code, 1);
    assert_non_null(strstr(outbuf, "incomplete"));
}

/* -- signals: ^C while a one-shot mode is running ---------------------- */

/* Run the real binary with stdout+stderr merged into a pipe, wait until
 * it prints `ready` (proof its SIGINT handler and the count hook are in
 * place and the chunk is running), let it spin for `settle_ms`, deliver
 * `sig`, then reap it. Output lands in outbuf, the exit status in
 * last_code. The marker has to come from stderr: stdout into a pipe is
 * block-buffered and may not flush until the process is gone. */
static void run_luna_signalled(const char *arg1, const char *arg2,
                               const char *ready, int settle_ms, int sig)
{
    int fds[2];
    assert_int_equal(pipe(fds), 0);
    pid_t pid = fork();
    assert_int_not_equal(pid, -1);
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        if (arg2)
            execl(LUNA_BIN, LUNA_BIN, arg1, arg2, (char *)NULL);
        else
            execl(LUNA_BIN, LUNA_BIN, arg1, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);

    size_t got = 0;
    outbuf[0] = '\0';
    int ready_seen = 0;
    int waited = 0;
    while (!ready_seen && waited < 10000) {
        struct pollfd p = { fds[0], POLLIN, 0 };
        if (poll(&p, 1, 100) > 0 && (p.revents & POLLIN)) {
            ssize_t n = read(fds[0], outbuf + got, sizeof(outbuf) - 1 - got);
            if (n <= 0)
                break;
            got += (size_t)n;
            outbuf[got] = '\0';
            if (strstr(outbuf, ready))
                ready_seen = 1;
            continue; /* drain eagerly: only idle time counts */
        }
        waited += 100;
    }
    if (!ready_seen) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(fds[0]);
        fail_msg("%s never printed \"%s\"; it said: [%s]", LUNA_BIN, ready,
                 outbuf);
    }
    /* optional settle: give the chunk enough instructions for the
     * kernel's count hook to reach its two-tick attach poll before the
     * interrupt lands */
    if (settle_ms > 0)
        usleep((useconds_t)settle_ms * 1000);
    if (kill(pid, sig) != 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(fds[0]);
        fail_msg("cannot deliver signal %d to %s", sig, LUNA_BIN);
    }

    int wstatus = 0;
    waited = 0;
    while (waited < 10000) {
        if (waitpid(pid, &wstatus, WNOHANG) == pid)
            break;
        usleep(20 * 1000);
        waited += 20;
    }
    if (waited >= 10000) {
        kill(pid, SIGKILL);
        waitpid(pid, &wstatus, 0);
        close(fds[0]);
        fail_msg("%s did not stop on signal %d", LUNA_BIN, sig);
    }
    /* the child is gone: drain whatever it printed after the marker */
    while (got < sizeof(outbuf) - 1) {
        struct pollfd p = { fds[0], POLLIN, 0 };
        if (poll(&p, 1, 200) <= 0)
            break;
        ssize_t n = read(fds[0], outbuf + got, sizeof(outbuf) - 1 - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    outbuf[got] = '\0';
    close(fds[0]);
    last_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
}

/* -e keeps the script mode convention: 128 + SIGINT, and the message
 * from the interrupted chunk still reaches the caller. */
static void test_eval_interrupt_exits_130(void **state)
{
    (void)state;
    run_luna_signalled("-e",
                       "io.stderr:write('spin\\n') while true do end",
                       "spin", 0, SIGINT);
    assert_int_equal(last_code, 130);
    assert_non_null(strstr(outbuf, "interrupted"));
}

static void test_script_interrupt_exits_130(void **state)
{
    (void)state;
    run_luna_signalled(LUNA_FIXTURES "/spin.lua", NULL, "spin", 0, SIGINT);
    assert_int_equal(last_code, 130);
    assert_non_null(strstr(outbuf, "interrupted"));
}

/* The count hook polls the attach socket every two ticks even when
 * there is no socket: --no-serve never installs the poll, and a script
 * spinning long enough to reach that branch must neither trip over the
 * missing step nor lose the ^C. */
static void test_interrupt_spinning_script_without_serve(void **state)
{
    (void)state;
    run_luna_signalled("--no-serve", LUNA_FIXTURES "/spin.lua", "spin", 300,
                       SIGINT);
    assert_int_equal(last_code, 130);
    assert_non_null(strstr(outbuf, "interrupted"));
}

/* -- batch 8: script-tail auto-drain ------------------------------------
 *
 * LUNA_TIMEOUT_CMD (when present) wraps these: a drain regression that
 * pins the loop fails fast with 124 instead of holding the group
 * hostage. Markers go to stderr (unbuffered even into a pipe). */

static int run_luna_capped(const char *args)
{
    char cmd[8192];
    if (LUNA_TIMEOUT_CMD[0] != '\0')
        snprintf(cmd, sizeof(cmd), "%s 10 '%s' %s 2>&1",
                 LUNA_TIMEOUT_CMD, LUNA_BIN, args);
    else
        snprintf(cmd, sizeof(cmd), "'%s' %s 2>&1", LUNA_BIN, args);
    FILE *p = popen(cmd, "r");
    assert_non_null(p);
    size_t n = fread(outbuf, 1, sizeof(outbuf) - 1, p);
    outbuf[n] = '\0';
    int status = pclose(p);
    last_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return 0;
}

/* a plain setTimeout without loop.run() still fires: the script tail
 * drains the loop, Node's "run until empty then exit" */
static void test_script_timer_fires_without_run(void **state)
{
    (void)state;
    run_luna_capped(LUNA_FIXTURES "/drain_timer.lua");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "drained"));
}

/* all-unref'd handles never hold the process: exit is immediate and
 * the callback never runs (it may not fire at all — Node's unref) */
static void test_all_unref_exits_without_firing(void **state)
{
    (void)state;
    run_luna_capped(LUNA_FIXTURES "/drain_unref.lua");
    assert_int_equal(last_code, 0);
    assert_null(strstr(outbuf, "NEVER"));
}

/* -e drains under the same contract */
static void test_eval_timer_fires_without_run(void **state)
{
    (void)state;
    run_luna_capped("-e 'local l = require(\"loop\") "
                    "l.setTimeout(function() io.stderr:write(\"edrained\\n\")"
                    " end, 80)'");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "edrained"));
}

/* a ^C landing mid-drain gets the same 130 as one landing in the
 * script body: maybeDrain raises "interrupted" like run() does */
static void test_drain_interrupt_exits_130(void **state)
{
    (void)state;
    run_luna_signalled(LUNA_FIXTURES "/drain_wait.lua", NULL, "ready", 200,
                       SIGINT);
    assert_int_equal(last_code, 130);
    assert_non_null(strstr(outbuf, "interrupted"));
}

/* -- interactive group -------------------------------------------------- */

static void test_interactive_piped_stdin(void **state)
{
    (void)state;
    run_luna_piped("x = 1\\nx + 41\\n", "");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "Out[1]: 42"));
}

static void test_interactive_multiline_piped(void **state)
{
    (void)state;
    run_luna_piped("function f(a)\\nreturn a * 2\\nend\\nf(21)\\n", "");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "Out[1]: 42"));
}

static void test_interactive_after_script(void **state)
{
    (void)state;
    /* -i runs the script first; the console sees its globals */
    run_luna_piped("y + 1\\n", "-i '" LUNA_FIXTURES "/init_state.lua'");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "Out[1]: 42"));
}

static void test_interactive_error_keeps_going(void **state)
{
    (void)state;
    run_luna_piped("error(\"oops\")\\n40 + 2\\n", "");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "oops"));
    assert_non_null(strstr(outbuf, "Out[1]: 42"));
}

static void test_interactive_help_injected(void **state)
{
    (void)state;
    /* the console injects help()/whos() — pipe one call in */
    run_luna_piped("help(print)\\n", "");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "function("));
}

static void test_interactive_whos_injected(void **state)
{
    (void)state;
    /* whos() is the other injected convenience: a table of globals */
    run_luna_piped("q = 7\\nwhos()\\n", "");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "name"));
    assert_non_null(strstr(outbuf, "q"));
}

static void test_help_sugar_on_a_broken_expr(void **state)
{
    (void)state;
    /* `? expr` sugar evaluating to a compile failure reports it like
     * any evaluation error and the session keeps going */
    run_luna_piped("? 1+\\n40 + 2\\n", "");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "expected"));
    assert_non_null(strstr(outbuf, "Out[1]: 42"));
}

/* -- color / degradation --------------------------------------------------- */

static void test_color_forced_by_env(void **state)
{
    (void)state;
    /* not a TTY, but LUNA_COLOR=1 forces colors on (Out label + value) */
    run_luna_env("LUNA_COLOR=1", "-e '40 + 2'");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "\x1b[")); /* colored */
    assert_non_null(strstr(outbuf, "Out[1]:"));
    assert_non_null(strstr(outbuf, "42"));
}

static void test_color_disabled_by_env(void **state)
{
    (void)state;
    /* LUNA_NO_COLOR wins even when LUNA_COLOR is also set */
    run_luna_env("LUNA_COLOR=1 LUNA_NO_COLOR=1", "-e '40 + 2'");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "Out[1]: 42"));
    assert_null(strstr(outbuf, "\x1b["));
}

static void test_color_empty_value_defers_to_tty(void **state)
{
    (void)state;
    /* an EMPTY LUNA_COLOR is not a forcing value: only a non-empty one
     * decides, so the empty form falls through to the TTY check — and
     * a pipe is no TTY, so colors stay off (LUNA_COLOR=1 would force
     * them on here; that contrast is the point) */
    run_luna_env("LUNA_COLOR=", "-e '40 + 2'");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "Out[1]: 42"));
    assert_null(strstr(outbuf, "\x1b["));
}

static void test_runs_without_coverage_instrumentation(void **state)
{
    (void)state;
    /* LUNA_COVERAGE scrubbed from the child env: coverage_init takes
     * its early return and coverage_shutdown the symmetric one — the
     * session runs identically uninstrumented */
    run_luna_env("env -u LUNA_COVERAGE", "-e '40 + 2'");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "Out[1]: 42"));
    assert_null(strstr(outbuf, "coverage"));
}

/* -- help / usage -------------------------------------------------------- */

static void test_help_smoke(void **state)
{
    (void)state;
    run_luna("--help");
    assert_int_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "--eval"));
    assert_non_null(strstr(outbuf, "--interactive"));
    assert_non_null(strstr(outbuf, "examples:"));
}

static void test_unknown_flag_rejected(void **state)
{
    (void)state;
    run_luna("--bogus-flag");
    assert_int_equal(last_code, 1);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_script_runs_and_gets_args),
        cmocka_unit_test(test_script_error_exit_code),
        cmocka_unit_test(test_script_missing_file),
        cmocka_unit_test(test_error_object_with_raising_tostring_is_named),
        cmocka_unit_test(test_error_object_tostring_message_skips_traceback),
        cmocka_unit_test(test_error_object_with_non_string_tostring_falls_back),
        cmocka_unit_test(test_chmod_rejects_bad_modes),
        cmocka_unit_test(test_check_classifies_truncated_chunks),
        cmocka_unit_test(test_exec_passes_script_args_after_name),
        cmocka_unit_test(test_eval_expression_echoes),
        cmocka_unit_test(test_eval_print),
        cmocka_unit_test(test_eval_error_exit_code),
        cmocka_unit_test(test_eval_incomplete_is_error),
        cmocka_unit_test(test_eval_interrupt_exits_130),
        cmocka_unit_test(test_script_interrupt_exits_130),
        cmocka_unit_test(test_interrupt_spinning_script_without_serve),
        cmocka_unit_test(test_script_timer_fires_without_run),
        cmocka_unit_test(test_all_unref_exits_without_firing),
        cmocka_unit_test(test_eval_timer_fires_without_run),
        cmocka_unit_test(test_drain_interrupt_exits_130),
        cmocka_unit_test(test_interactive_piped_stdin),
        cmocka_unit_test(test_interactive_multiline_piped),
        cmocka_unit_test(test_interactive_after_script),
        cmocka_unit_test(test_interactive_error_keeps_going),
        cmocka_unit_test(test_interactive_help_injected),
        cmocka_unit_test(test_interactive_whos_injected),
        cmocka_unit_test(test_help_sugar_on_a_broken_expr),
        cmocka_unit_test(test_color_forced_by_env),
        cmocka_unit_test(test_color_disabled_by_env),
        cmocka_unit_test(test_color_empty_value_defers_to_tty),
        cmocka_unit_test(test_runs_without_coverage_instrumentation),
        cmocka_unit_test(test_help_smoke),
        cmocka_unit_test(test_unknown_flag_rejected),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}

#else /* _WIN32: empty suite, see the note at the top of the file */

int main(void)
{
    return 0;
}

#endif
