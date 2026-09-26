/* CLI launch modes, exercised through the real luna binary:
 *   script mode  (luna script.lua args)
 *   eval mode    (luna -e 'code')
 *   interactive  (piped stdin: bare luna, and luna -i script)
 * plus a --help smoke check. Each mode is its own group.
 */
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
        cmocka_unit_test(test_eval_expression_echoes),
        cmocka_unit_test(test_eval_print),
        cmocka_unit_test(test_eval_error_exit_code),
        cmocka_unit_test(test_eval_incomplete_is_error),
        cmocka_unit_test(test_eval_interrupt_exits_130),
        cmocka_unit_test(test_script_interrupt_exits_130),
        cmocka_unit_test(test_interrupt_spinning_script_without_serve),
        cmocka_unit_test(test_interactive_piped_stdin),
        cmocka_unit_test(test_interactive_multiline_piped),
        cmocka_unit_test(test_interactive_after_script),
        cmocka_unit_test(test_interactive_error_keeps_going),
        cmocka_unit_test(test_interactive_help_injected),
        cmocka_unit_test(test_color_forced_by_env),
        cmocka_unit_test(test_color_disabled_by_env),
        cmocka_unit_test(test_runs_without_coverage_instrumentation),
        cmocka_unit_test(test_help_smoke),
        cmocka_unit_test(test_unknown_flag_rejected),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
