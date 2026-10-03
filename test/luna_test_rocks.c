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

/* The harness is POSIX end to end: sh command lines through system()
 * (cd 'dir' && ..., rm -rf, mkdir -p, the timeout guard), mkdtemp
 * scratch trees and wait-status macros - cmd.exe knows none of that.
 * Windows compiles the target to an empty suite instead of tripping
 * over one POSIX header at a time. */
#ifndef _WIN32

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

/* inner hang guard, from CMake: GNU timeout, or gtimeout where macOS
 * brew provides it, or empty (no guard — ctest TIMEOUT still bounds) */
#ifndef LUNA_TIMEOUT_CMD
#define LUNA_TIMEOUT_CMD "timeout"
#endif

static char workdir[256];
static char origdir[256];
static int keep_workdir = 0; /* set on failure: teardown keeps the scene */

static char *read_all(const char *path);

/* run `luna <args>` in the scratch dir; returns the exit status. On a
 * nonzero status the child's captured output is printed — the scratch
 * dir is also kept for post-mortem.
 *
 * The inner `timeout` is only a hang guard, not the budget: a plain
 * build installs in ~15s, but the Profiling tree's coverage hook makes
 * the Lua side ~10x slower, so the guard has to clear that too. The
 * ctest TIMEOUT on this group is the outer bound. */
static int run_luna(const char *args)
{
    char cmd[512];
    if (LUNA_TIMEOUT_CMD[0] != '\0')
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && %s 420 '%s' %s > out.log 2>&1",
                 workdir, LUNA_TIMEOUT_CMD, LUNA_BIN, args);
    else
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && '%s' %s > out.log 2>&1",
                 workdir, LUNA_BIN, args);
    int rc = system(cmd);
    if (rc == -1 || !WIFEXITED(rc)) {
        keep_workdir = 1;
        printf("luna crashed: cmd=[%s]\n", cmd);
        return -1;
    }
    int code = WEXITSTATUS(rc);
    if (code != 0) {
        keep_workdir = 1;
        printf("luna failed (%d): cmd=[%s] dir=[%s]\n--- out.log ---\n",
               code, cmd, workdir);
        char logpath[512];
        snprintf(logpath, sizeof(logpath), "%s/out.log", workdir);
        char *log = read_all(logpath);
        if (log) {
            printf("%s\n---\n", log);
            free(log);
        }
    }
    return code;
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
    if (keep_workdir) {
        printf("rocks-test scene kept at %s\n", workdir);
        return 0;
    }
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

    /* failed .src.rock fetches must leave no 0-byte remnant in the
     * cache: inspect ships no .src.rock (fetch fails by design), and a
     * leftover would be hashed into the NEXT relock as the empty
     * digest (e3b0c442...), which --from-lock would then happily
     * "verify" and hand luarocks a blank source */
    char junk[512];
    snprintf(junk, sizeof(junk), "%s/.luna/cache/inspect-3.1.3-0.src.rock", workdir);
    struct stat junk_st;
    assert_int_not_equal(stat(junk, &junk_st), 0);
    assert_null(strstr(lock1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

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
    assert_null(strstr(lock2, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    free(lock1);
    free(lock2);
}

/* The offline corners of the wrapper: usage errors, lock-file errors,
 * the sha refusal, path injection — none of these touch the network,
 * so they run everywhere the real install test does (and faster). */

static void write_file(const char *relpath, const char *text)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", workdir, relpath);
    FILE *f = fopen(path, "w");
    assert_non_null(f);
    assert_int_equal(fwrite(text, 1, strlen(text), f), strlen(text));
    assert_int_equal(fclose(f), 0);
}

static int run_luna_in(const char *dir, const char *args)
{
    char cmd[600];
    if (LUNA_TIMEOUT_CMD[0] != '\0')
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && %s 420 '%s' %s > out.log 2>&1",
                 dir, LUNA_TIMEOUT_CMD, LUNA_BIN, args);
    else
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && '%s' %s > out.log 2>&1",
                 dir, LUNA_BIN, args);
    int rc = system(cmd);
    if (rc == -1 || !WIFEXITED(rc))
        return -1;
    return WEXITSTATUS(rc);
}

static char *out_log(void)
{
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/out.log", workdir);
    return read_all(logpath);
}

static void test_install_usage_is_an_error(void **state)
{
    (void)state;
    assert_int_equal(run_luna("install"), 1);
    char *log = out_log();
    assert_non_null(log);
    assert_non_null(strstr(log, "usage: luna install"));
    free(log);
}

static void test_update_without_a_lock_is_an_error(void **state)
{
    (void)state;
    assert_int_equal(run_luna("update"), 1);
    char *log = out_log();
    assert_non_null(log);
    assert_non_null(strstr(log, "no luna.lock here"));
    free(log);
}

static void test_update_with_a_broken_lock_is_an_error(void **state)
{
    (void)state;
    write_file("luna.lock", "{oops");
    assert_int_equal(run_luna("update"), 1);
    char *log = out_log();
    assert_non_null(log);
    assert_non_null(strstr(log, "not valid JSON"));
    free(log);
}

static void test_update_on_an_empty_lock_relocks_offline(void **state)
{
    (void)state;
    write_file("luna.lock",
               "{\"version\":1,\"lua\":\"5.5\",\"rocks\":[]}\n");
    assert_int_equal(run_luna("update"), 0);
    char *log = out_log();
    assert_non_null(log);
    assert_non_null(strstr(log, "locked 0 rocks"));
    free(log);
}

static void test_from_lock_refuses_a_cached_sha_mismatch(void **state)
{
    (void)state;
    /* a cached source whose bytes do not hash to the locked sha is
     * refused before luarocks ever runs — offline */
    char mkcache[600];
    snprintf(mkcache, sizeof(mkcache), "mkdir -p '%s/.luna/cache'", workdir);
    assert_int_equal(system(mkcache), 0);
    write_file(".luna/cache/fakerock-1.0-1.src.rock", "not the bytes\n");
    write_file("luna.lock",
               "{\"version\":1,\"lua\":\"5.5\",\"rocks\":["
               "{\"name\":\"fakerock\",\"version\":\"1.0-1\","
               "\"sha256\":\"0000\"}]}\n");
    assert_int_equal(run_luna("install --from-lock"), 1);
    char *log = out_log();
    assert_non_null(log);
    assert_non_null(strstr(log, "sha256 mismatch for cached"));
    free(log);
}

static void test_rocks_tree_injects_module_paths(void **state)
{
    (void)state;
    /* a .luna/rocks tree in the cwd (and in a parent of the cwd)
     * joins package.path/cpath at startup */
    char lunadir[512], tree[512], deep[512];
    snprintf(lunadir, sizeof(lunadir), "%s/.luna", workdir);
    assert_int_equal(mkdir(lunadir, 0755), 0);
    snprintf(tree, sizeof(tree), "%s/.luna/rocks", workdir);
    assert_int_equal(mkdir(tree, 0755), 0);
    snprintf(deep, sizeof(deep), "%s/deep", workdir);
    assert_int_equal(mkdir(deep, 0755), 0);
    assert_int_equal(run_luna_in(
        workdir, "-e 'print(package.path:find(\".luna/rocks\", 1, true) "
                 "~= nil, package.cpath:find(\".luna/rocks\", 1, true) "
                 "~= nil)'"), 0);
    char *log = out_log();
    assert_non_null(log);
    assert_non_null(strstr(log, "true\ttrue"));
    free(log);
    /* and from a subdirectory the walk-up finds the same tree */
    assert_int_equal(run_luna_in(
        deep, "-e 'print(package.path:find(\".luna/rocks\", 1, true) "
              "~= nil)'"), 0);
    char deeplog[512];
    snprintf(deeplog, sizeof(deeplog), "%s/out.log", deep);
    char *dlog = read_all(deeplog);
    assert_non_null(dlog);
    assert_non_null(strstr(dlog, "true"));
    free(dlog);
}

static void test_list_on_an_empty_tree_is_quiet_offline(void **state)
{
    (void)state;
    char lunadir[512], tree[512];
    snprintf(lunadir, sizeof(lunadir), "%s/.luna", workdir);
    assert_int_equal(mkdir(lunadir, 0755), 0);
    snprintf(tree, sizeof(tree), "%s/.luna/rocks", workdir);
    assert_int_equal(mkdir(tree, 0755), 0);
    assert_int_equal(run_luna("list"), 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_install_lock_reproduce_cycle,
                                        setup_rocks, teardown_rocks),
        cmocka_unit_test_setup_teardown(test_install_usage_is_an_error,
                                        setup_rocks, teardown_rocks),
        cmocka_unit_test_setup_teardown(test_update_without_a_lock_is_an_error,
                                        setup_rocks, teardown_rocks),
        cmocka_unit_test_setup_teardown(test_update_with_a_broken_lock_is_an_error,
                                        setup_rocks, teardown_rocks),
        cmocka_unit_test_setup_teardown(test_update_on_an_empty_lock_relocks_offline,
                                        setup_rocks, teardown_rocks),
        cmocka_unit_test_setup_teardown(test_from_lock_refuses_a_cached_sha_mismatch,
                                        setup_rocks, teardown_rocks),
        cmocka_unit_test_setup_teardown(test_rocks_tree_injects_module_paths,
                                        setup_rocks, teardown_rocks),
        cmocka_unit_test_setup_teardown(test_list_on_an_empty_tree_is_quiet_offline,
                                        setup_rocks, teardown_rocks),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}

#else /* _WIN32: empty suite, see the note at the top of the file */

int main(void)
{
    return 0;
}

#endif
