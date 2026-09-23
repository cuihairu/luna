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
        cmocka_unit_test(test_interactive_piped_stdin),
        cmocka_unit_test(test_interactive_multiline_piped),
        cmocka_unit_test(test_interactive_after_script),
        cmocka_unit_test(test_interactive_error_keeps_going),
        cmocka_unit_test(test_interactive_help_injected),
        cmocka_unit_test(test_help_smoke),
        cmocka_unit_test(test_unknown_flag_rejected),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
