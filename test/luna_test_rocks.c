/* luna_test_rocks.c — the LuaRocks wrapper end to end:
 *
 *   1. `luna install <rock>`  → tree populated + luna.lock written
 *   2. wipe the tree          → (lock retained)
 *   3. `luna install --from-lock` → tree reproduced, sha-verified
 *
 * This drives the real luna binary in a scratch directory and needs
 * network for the online install (the CI runner has it; the from-lock
 * step itself is offline-capable via .luna/cache).
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cmocka.h>

#ifndef LUNA_BIN
#define LUNA_BIN "./luna"
#endif

static char workdir[256];
static char origdir[256];

/* run `luna <args>` in the scratch dir; returns the exit status */
static int run_luna(const char *args)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && timeout 150 '%s' %s > out.log 2>&1",
             workdir, LUNA_BIN, args);
    int rc = system(cmd);
    if (rc == -1 || !WIFEXITED(rc))
        return -1;
    return WEXITSTATUS(rc);
}

static void assert_file_exists(const char *path)
{
    struct stat st;
    assert_int_equal(stat(path, &st), 0);
}

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static int setup_rocks(void **state)
{
    (void)state;
    assert_non_null(getcwd(origdir, sizeof(origdir)));
    snprintf(workdir, sizeof(workdir), "/tmp/luna-rocks-test-XXXXXX");
    assert_non_null(mkdtemp(workdir));
    return 0;
}

static int teardown_rocks(void **state)
{
    (void)state;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", workdir);
    if (system(cmd) != 0)
        return -1;
    if (chdir(origdir) != 0)
        return -1;
    return 0;
}

static void test_install_lock_reproduce_cycle(void **state)
{
    (void)state;

    /* 1. online install of two rocks in one shot: inspect (git-sourced,
     * locked by rockspec sha only) and ansicolors (ships a .src.rock,
     * cached + locked by both shas) */
    assert_int_equal(run_luna("install inspect ansicolors"), 0);
    char tree_inspect[512], tree_ansi[512];
    snprintf(tree_inspect, sizeof(tree_inspect),
             "%s/.luna/rocks/share/lua/5.5/inspect.lua", workdir);
    snprintf(tree_ansi, sizeof(tree_ansi),
             "%s/.luna/rocks/share/lua/5.5/ansicolors.lua", workdir);
    assert_file_exists(tree_inspect);
    assert_file_exists(tree_ansi);
    char lockpath[512];
    snprintf(lockpath, sizeof(lockpath), "%s/luna.lock", workdir);
    assert_file_exists(lockpath);
    char *lock1 = read_all(lockpath);
    assert_non_null(lock1);
    assert_non_null(strstr(lock1, "\"inspect\""));
    assert_non_null(strstr(lock1, "\"3.1.3-0\""));
    assert_non_null(strstr(lock1, "\"ansicolors\""));
    assert_non_null(strstr(lock1, "\"rockspec\"")); /* the sha entries */

    /* 2. wipe the tree, keep the lock (and the .luna/cache) */
    char treepath[512];
    snprintf(treepath, sizeof(treepath), "%s/.luna/rocks", workdir);
    char rm[600];
    snprintf(rm, sizeof(rm), "rm -rf '%s'", treepath);
    assert_int_equal(system(rm), 0);
    struct stat st;
    assert_int_not_equal(stat(treepath, &st), 0);

    /* 3. reproduce from the lock: cached ansicolors comes back offline
     * (sha256-checked), inspect by name+version (rockspec-checked);
     * the lock itself is untouched */
    assert_int_equal(run_luna("install --from-lock"), 0);
    assert_file_exists(tree_inspect);
    assert_file_exists(tree_ansi);
    char *lock2 = read_all(lockpath);
    assert_non_null(lock2);
    assert_string_equal(lock2, lock1);
    free(lock1);
    free(lock2);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_install_lock_reproduce_cycle,
                                        setup_rocks, teardown_rocks),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
