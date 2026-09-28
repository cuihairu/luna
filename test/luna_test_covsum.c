/* The coverage toolchain's Lua summary reporter: cmake/lua_cov_summary.lua
 * turns luacov's report file into the build-log table the lua_coverage
 * target prints. A silent parse regression here would empty the ledger
 * with a green build, so the reporter is driven through the real binary
 * with synthetic reports and its contract locked line by line:
 *   echo from the "File" header through the file rows, stop at Total,
 *   never echo the per-file body sections nor the trailing rule.
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

#ifndef LUNA_COVSUM_SCRIPT
#define LUNA_COVSUM_SCRIPT "cmake/lua_cov_summary.lua"
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

/* run_luna variant that starts the child in `dir` first (the default
 * report path resolves against the child's cwd) */
static int run_luna_in_dir(const char *dir, const char *args)
{
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "cd %s && %s %s 2>&1", dir, LUNA_BIN, args);
    FILE *p = popen(cmd, "r");
    assert_non_null(p);
    size_t n = fread(outbuf, 1, sizeof(outbuf) - 1, p);
    outbuf[n] = '\0';
    int status = pclose(p);
    last_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return 0;
}

/* count non-overlapping occurrences of `needle` in outbuf */
static int count_needle(const char *needle)
{
    int count = 0;
    const char *at = outbuf;
    while ((at = strstr(at, needle)) != NULL) {
        count++;
        at += strlen(needle);
    }
    return count;
}

static void test_summary_echoes_table_and_skips_body(void **state)
{
    (void)state;
    run_luna("'" LUNA_COVSUM_SCRIPT "'"
             " '" LUNA_FIXTURES "/covsum_report.fixture'");
    assert_int_equal(last_code, 0);
    /* the summary table, verbatim */
    assert_non_null(strstr(outbuf, "File"));
    assert_non_null(strstr(outbuf, "fixture_module.lua"));
    assert_non_null(strstr(outbuf, "other_module.lua"));
    assert_non_null(strstr(outbuf, "Total"));
    assert_non_null(strstr(outbuf, "83.33%"));
    /* the per-file body section stays out: neither its source lines
     * nor its ===== rules are echoed */
    assert_null(strstr(outbuf, "local x = 1"));
    assert_null(strstr(outbuf, "=============="));
}

static void test_summary_stops_at_total(void **state)
{
    (void)state;
    run_luna("'" LUNA_COVSUM_SCRIPT "'"
             " '" LUNA_FIXTURES "/covsum_report.fixture'");
    assert_int_equal(last_code, 0);
    /* exactly one rule reaches the log: the one under the header. The
     * trailing rule (and the body's) end printing before the echo */
    assert_int_equal(count_needle("\n----"), 1);
    /* and Total is the last word: nothing follows its number */
    const char *total = strstr(outbuf, "Total");
    assert_non_null(total);
    assert_null(strstr(total, "\n----"));
}

static void test_missing_report_fails_loudly(void **state)
{
    (void)state;
    run_luna("'" LUNA_COVSUM_SCRIPT "'"
             " '" LUNA_FIXTURES "/no_such_report.out'");
    assert_int_not_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "cannot open"));
}

/* no argument means the reporter's default report path — the same
 * loud failure, naming the file it wanted (the default-arg leg) */
static void test_default_report_path_fails_loudly(void **state)
{
    (void)state;
    /* /tmp carries no luacov.report.out */
    run_luna_in_dir("/tmp", "'" LUNA_COVSUM_SCRIPT "'");
    assert_int_not_equal(last_code, 0);
    assert_non_null(strstr(outbuf, "luacov.report.out"));
    assert_non_null(strstr(outbuf, "cannot open"));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_summary_echoes_table_and_skips_body),
        cmocka_unit_test(test_summary_stops_at_total),
        cmocka_unit_test(test_missing_report_fails_loudly),
        cmocka_unit_test(test_default_report_path_fails_loudly),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
