/* Attach channel: unix socket lifecycle, framed replies (OK/ERR ... \30),
 * evaluation in the live state, error isolation and shutdown cleanup.
 * The test is one Lua state acting as both target and client, stepping
 * serve.step() by hand where the kernel count hook would normally sit.
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

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "luna_kernel.h"
#include "luna_cov.h"
#include "luna_lua.h"

int luaopen_lpeg(lua_State *L);        /* introspect's pattern use */
int luaopen_socket_unix(lua_State *L); /* luasocket unix transport */

#ifndef SERVE_FIXTURES
#define SERVE_FIXTURES "fixtures"
#endif

static lua_State *L;
static char sockdir[] = "/tmp/luna-test-serve-XXXXXX";

static void run(lua_State *l, const char *code)
{
    if (luaL_dostring(l, code) != LUA_OK) {
        fail_msg("lua error: %s", lua_tostring(l, -1));
    }
}

/* run(code) returning one string from the stack */
static const char *eval_string(const char *code)
{
    static char buf[16384]; /* %whos frames carry the whole globals list */
    run(L, code);
    const char *s = lua_tostring(L, -1);
    snprintf(buf, sizeof(buf), "%s", s ? s : "(nil)");
    lua_pop(L, 1);
    return buf;
}

static int setup_serve(void **state)
{
    (void)state;
    assert_non_null(mkdtemp(sockdir));
    setenv("LUNA_SOCK_DIR", sockdir, 1);

    L = luaL_newstate();
    assert_non_null(L);
    luaL_openlibs(L);
    luaL_requiref(L, "kernel", luaopen_luna_kernel, 1);
    lua_pop(L, 1);
    luna_cov_setup(L);
    luaL_requiref(L, "lpeg", luaopen_lpeg, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "socket.unix", luaopen_socket_unix, 0);
    lua_pop(L, 1);

    /* the embedded policy modules the serve chain requires */
    lua_pushlstring(L, LUNA_LUA_INTROSPECT, sizeof(LUNA_LUA_INTROSPECT) - 1);
    lua_setglobal(L, "__LUNA_INTROSPECT_SRC");
    lua_pushlstring(L, LUNA_LUA_SERVE, sizeof(LUNA_LUA_SERVE) - 1);
    lua_setglobal(L, "__LUNA_SERVE_SRC");
    lua_pushlstring(L, LUNA_LUA_MAGIC, sizeof(LUNA_LUA_MAGIC) - 1);
    lua_setglobal(L, "__LUNA_MAGIC_SRC");
    lua_pushlstring(L, LUNA_LUA_COMPLETE, sizeof(LUNA_LUA_COMPLETE) - 1);
    lua_setglobal(L, "__LUNA_COMPLETE_SRC");
    run(L,
        "package.preload['luna.introspect'] = assert(load(__LUNA_INTROSPECT_SRC, luna_chunkname('introspect')))\n"
        "package.preload['luna.serve'] = assert(load(__LUNA_SERVE_SRC, luna_chunkname('serve')))\n"
        "package.preload['luna.magic'] = assert(load(__LUNA_MAGIC_SRC, luna_chunkname('magic')))\n"
        "package.preload['luna.complete'] = assert(load(__LUNA_COMPLETE_SRC, luna_chunkname('complete')))\n"
        "S = require('luna.serve')\n"
        "assert(S.start())\n"
        "C = require('socket.unix')()\n"
        "assert(C:connect(S.path_for(kernel.pid())))\n"
        "C:settimeout(2)\n");

    /* one framed exchange: send a line, step the server once, gather
     * the reply up to the \30 terminator */
    run(L,
        "function exchange(line)\n"
        "  assert(C:send(line .. '\\n'))\n"
        "  S.step()\n"
        "  local parts = {}\n"
        "  while true do\n"
        "    local part = assert(C:receive('*l'))\n"
        "    if part == '\\30' then break end\n"
        "    parts[#parts + 1] = part\n"
        "  end\n"
        "  return table.concat(parts, '|')\n"
        "end\n");
    return 0;
}

static int teardown_serve(void **state)
{
    (void)state;
    luna_cov_teardown(L);
    run(L, "S.stop()");
    lua_close(L);
    L = NULL;
    unsetenv("LUNA_SOCK_DIR");
    assert_int_equal(rmdir(sockdir), 0);
    return 0;
}

static void test_socket_file_created(void **state)
{
    (void)state;
    char expected[256];
    snprintf(expected, sizeof(expected), "%s/luna-%d.sock", sockdir,
             (int)getpid()); /* kernel.pid() is this process */
    assert_string_equal(eval_string("return S.path_for(kernel.pid())"),
                        expected);
    /* fopen on a socket file fails (ENXIO); rename proves existence */
    assert_string_equal(eval_string(
                            "return tostring(os.rename(S.path_for(kernel.pid()),"
                            " S.path_for(kernel.pid())))"),
                        "true");
    /* and it is owner-only from creation: the umask wraps the bind */
    struct stat st;
    assert_int_equal(stat(expected, &st), 0);
    assert_int_equal(st.st_mode & 0777, 0600);
}

static void test_statement_gets_nil_reply(void **state)
{
    (void)state;
    assert_string_equal(eval_string("return exchange('answer = 6 * 7')"),
                        "OK|nil");
}

static void test_expression_evaluates_in_live_state(void **state)
{
    (void)state;
    /* the variable set by the previous statement is still there */
    assert_string_equal(eval_string("return exchange('return answer')"),
                        "OK|42");
    assert_string_equal(eval_string("return tostring(answer)"), "42");
}

static void test_error_reply_is_framed_and_isolated(void **state)
{
    (void)state;
    const char *reply = eval_string("return exchange('error(\"boom\")')");
    assert_memory_equal(reply, "ERR|", 4);
    assert_non_null(strstr(reply, "boom"));
    /* the state (and the client) survive a raising command */
    assert_string_equal(eval_string("return exchange('return 1 + 1')"),
                        "OK|2");
}

static void test_print_output_is_captured_into_frame(void **state)
{
    (void)state;
    /* kernel output during the command rides in the frame body, with
     * the result repr after it */
    const char *reply =
        eval_string("return exchange('print(\"hello\") return 7')");
    assert_non_null(strstr(reply, "hello"));
    assert_non_null(strstr(reply, "7"));
}

static void test_magic_runs_against_live_state(void **state)
{
    (void)state;
    run(L, "attachgx = 5");
    const char *reply = eval_string("return exchange('%whos')");
    assert_memory_equal(reply, "OK|", 3);
    assert_non_null(strstr(reply, "attachgx"));
}

static void test_exit_magic_detaches_and_target_survives(void **state)
{
    (void)state;
    const char *reply = eval_string("return exchange('%exit')");
    assert_memory_equal(reply, "EXIT|", 5);
    /* the target process is still alive and serving */
    assert_string_equal(eval_string("return exchange('return 1 + 1')"),
                        "OK|2");
}

static void test_completion_meta_line(void **state)
{
    (void)state;
    const char *reply =
        eval_string("return exchange('\1complete attachg')");
    assert_memory_equal(reply, "OK|", 3);
    assert_non_null(strstr(reply, "attachgx"));
}

static void test_stop_clears_socket_and_poll_noops(void **state)
{
    (void)state;
    run(L,
        "S.stop()\n"
        "S.step()\n" /* must not raise */
        "assert(os.remove(S.path_for(kernel.pid())) == nil, 'socket gone')");
}

/* Drain the pty master for up to timeout_ms, collecting whatever
 * arrived into buf. Returns bytes collected (NUL-terminated). */
static int pty_collect(int fd, char *buf, size_t cap, int timeout_ms)
{
    size_t got = 0;
    int waited = 0;
    while (got < cap - 1 && waited < timeout_ms) {
        struct pollfd p = { fd, POLLIN, 0 };
        int pr = poll(&p, 1, 100);
        if (pr > 0 && (p.revents & POLLIN)) {
            ssize_t n = read(fd, buf + got, cap - 1 - got);
            if (n <= 0)
                break;
            got += (size_t)n;
            continue; /* drain eagerly; only idle time counts */
        }
        waited += 100;
    }
    buf[got] = '\0';
    return (int)got;
}

/* SIGUSR1 must release a blocked line editor on a real pty: the wake
 * thread's synthetic Enter commits the empty line, the prompt is
 * repainted (In[n] intentionally not advanced), and the REPL loop is
 * free to poll the attach socket. */
static void test_sigusr1_releases_idle_editor(void **state)
{
    (void)state;
    int master;
    pid_t pid = forkpty(&master, NULL, NULL, NULL);
    assert_int_not_equal(pid, -1);
    if (pid == 0) {
        char *argv[] = { LUNA_BINARY, NULL };
        setenv("TERM", "xterm", 1);
        execv(LUNA_BINARY, argv); /* inherits LUNA_SOCK_DIR */
        _exit(127);
    }
    char buf[4096];
    int n = pty_collect(master, buf, sizeof(buf), 10000);
    assert_int_not_equal(n, 0);
    assert_non_null(strstr(buf, "In [1]"));

    assert_int_equal(kill(pid, SIGUSR1), 0);
    n = pty_collect(master, buf, sizeof(buf), 3000);
    assert_int_not_equal(n, 0);
    /* the synthetic Enter: newline + repainted prompt */
    assert_non_null(strstr(buf, "\r\n"));
    assert_non_null(strstr(buf, "In [1]"));

    kill(pid, SIGTERM);
    int wstatus;
    waitpid(pid, &wstatus, 0);
    close(master);
    /* the killed REPL cannot clean up after itself */
    char path[256];
    snprintf(path, sizeof(path), "%s/luna-%d.sock", sockdir, (int)pid);
    unlink(path);
}

/* The same bell, followed by leaving through the front door: ^D on the
 * idle prompt ends the session with status 0 and serve.stop() unlinks
 * the attach socket on the way out — unlike the SIGTERM case above,
 * the clean exit also lets the process flush its own coverage data. */
static void test_sigusr1_then_clean_exit(void **state)
{
    (void)state;
    int master;
    pid_t pid = forkpty(&master, NULL, NULL, NULL);
    assert_int_not_equal(pid, -1);
    if (pid == 0) {
        char *argv[] = { LUNA_BINARY, NULL };
        setenv("TERM", "xterm", 1);
        execv(LUNA_BINARY, argv); /* inherits LUNA_SOCK_DIR */
        _exit(127);
    }
    char buf[4096];
    int n = pty_collect(master, buf, sizeof(buf), 10000);
    assert_int_not_equal(n, 0);
    assert_non_null(strstr(buf, "In [1]"));

    assert_int_equal(kill(pid, SIGUSR1), 0);
    n = pty_collect(master, buf, sizeof(buf), 3000);
    assert_int_not_equal(n, 0);
    assert_non_null(strstr(buf, "\r\n")); /* synthetic Enter + repaint */

    /* let the repaint settle before the key: a ^D typed while the
     * console is still drawing is eaten by the tty line discipline */
    usleep(300 * 1000);
    assert_int_equal(write(master, "\x04", 1), 1);

    int wstatus = 0;
    int waited = 0;
    while (waited < 5000) {
        if (waitpid(pid, &wstatus, WNOHANG) == pid)
            break;
        usleep(20 * 1000);
        waited += 20;
    }
    close(master);
    if (waited >= 5000) {
        kill(pid, SIGKILL);
        waitpid(pid, &wstatus, 0);
        fail_msg("the REPL did not leave on ^D");
    }
    assert_true(WIFEXITED(wstatus));
    assert_int_equal(WEXITSTATUS(wstatus), 0);

    /* the clean path removed the socket itself, no test-side unlink */
    char path[256];
    snprintf(path, sizeof(path), "%s/luna-%d.sock", sockdir, (int)pid);
    assert_int_equal(access(path, F_OK), -1);
}

/* The attach channel's reason to exist: a busy — even looping — script
 * stays reachable from `luna --attach`. No line editor is involved, so
 * the only poll points are the kernel's count hook (every 200k
 * instructions) and the SIGUSR1 the client rings after each send. The
 * target then leaves through a marker file instead of being killed,
 * which also lets it flush its own coverage data. */
static void test_attach_reaches_a_busy_script(void **state)
{
    (void)state;
    char marker[512], infile[512];
    snprintf(marker, sizeof(marker), "%s/busy.done", sockdir);
    snprintf(infile, sizeof(infile), "%s/attach.in", sockdir);
    FILE *in = fopen(infile, "w");
    assert_non_null(in);
    fputs("return 40 + 2\n%detach\n", in);
    fclose(in);

    int pipes[2];
    assert_int_equal(pipe(pipes), 0);
    pid_t pid = fork();
    assert_int_not_equal(pid, -1);
    if (pid == 0) {
        close(pipes[0]);
        dup2(pipes[1], STDOUT_FILENO);
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[1]);
        char *argv[] = { LUNA_BINARY, SERVE_FIXTURES "/busy.lua", marker, NULL };
        execv(LUNA_BINARY, argv);
        _exit(127);
    }
    close(pipes[1]);

    /* serve.start() opens the socket before the script's first
     * instruction, so the wait is only about process startup */
    char path[256];
    snprintf(path, sizeof(path), "%s/luna-%d.sock", sockdir, (int)pid);
    int waited = 0;
    while (waited < 10000 && access(path, F_OK) != 0) {
        usleep(20 * 1000);
        waited += 20;
    }
    assert_int_equal(access(path, F_OK), 0);

    /* the client runs headless (piped stdin): replies land on stdout */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s --attach %d < %s 2>&1", LUNA_BINARY, (int)pid,
             infile);
    FILE *p = popen(cmd, "r");
    assert_non_null(p);
    char reply[4096];
    size_t n = fread(reply, 1, sizeof(reply) - 1, p);
    reply[n] = '\0';
    int status = pclose(p);
    assert_int_equal(WIFEXITED(status) ? WEXITSTATUS(status) : -1, 0);
    /* the framed answer, straight after the client's own prompt */
    assert_non_null(strstr(reply, "attach> 42"));

    /* hand the script its marker: it exits on its own and takes the
     * socket with it, no test-side unlink */
    FILE *m = fopen(marker, "w");
    assert_non_null(m);
    fclose(m);
    waited = 0;
    while (waited < 10000) {
        if (waitpid(pid, &status, WNOHANG) == pid)
            break;
        usleep(20 * 1000);
        waited += 20;
    }
    close(pipes[0]);
    if (waited >= 10000) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        fail_msg("the busy script never saw its marker");
    }
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 0);
    assert_int_equal(access(path, F_OK), -1);
    unlink(infile);
    unlink(marker);
}

/* The attach plumbing reports its failures instead of swallowing them:
 * a wake at a pid that cannot exist, a mode string strtol cannot read,
 * and a chmod at a path that is not there all come back as errors that
 * name the failing call and the syscall's own reason. */
static void test_wake_and_chmod_report_errors(void **state)
{
    (void)state;
    run(L,
        "local ok, err = pcall(kernel.wake, 2147483647)\n"
        "assert(not ok, 'a pid that large cannot exist')\n"
        "assert(tostring(err):find('wake:', 1, true), err)\n"
        "ok, err = pcall(kernel.chmod, '/nonexistent-luna-socket', 'xyz')\n"
        "assert(not ok and tostring(err):find('chmod: bad mode', 1, true), err)\n"
        "ok, err = pcall(kernel.chmod, '/nonexistent-luna-socket', '644')\n"
        "assert(not ok and tostring(err):find('chmod: ', 1, true), err)\n"
        "assert(tostring(err):find('No such file', 1, true), err)\n");
}

/* The count hook pcall's __LUNA_SERVE_STEP every ~200k instructions
 * while a chunk runs, and a poll that raises must be swallowed — "a
 * failing poll must not kill the chunk". The fixture installs one that
 * always raises, then spins until its marker file appears: if the
 * error escaped, the script would die instead of reaching "done". */
static void test_a_broken_attach_poll_never_kills_the_chunk(void **state)
{
    (void)state;
    char marker[512];
    snprintf(marker, sizeof(marker), "%s/poll.ok", sockdir);

    int pipes[2];
    assert_int_equal(pipe(pipes), 0);
    pid_t pid = fork();
    assert_int_not_equal(pid, -1);
    if (pid == 0) {
        close(pipes[0]);
        dup2(pipes[1], STDOUT_FILENO);
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[1]);
        char *argv[] = { LUNA_BINARY, "--no-serve",
                         SERVE_FIXTURES "/badpoll.lua", marker, NULL };
        execv(LUNA_BINARY, argv);
        _exit(127);
    }
    close(pipes[1]);

    /* "spin" comes off stderr (unbuffered into a pipe) once the poll is
     * installed and the loop is running; ticks fail from then on */
    char out[4096];
    size_t got = 0;
    out[0] = '\0';
    int waited = 0;
    while (waited < 10000 && !strstr(out, "spin")) {
        struct pollfd p = { pipes[0], POLLIN, 0 };
        if (poll(&p, 1, 100) > 0 && (p.revents & POLLIN)) {
            ssize_t n = read(pipes[0], out + got, sizeof(out) - 1 - got);
            if (n <= 0)
                break;
            got += (size_t)n;
            out[got] = '\0';
            continue;
        }
        waited += 100;
    }
    assert_non_null(strstr(out, "spin"));

    /* let several failing ticks go by, then release the fixture */
    usleep(300 * 1000);
    FILE *m = fopen(marker, "w");
    assert_non_null(m);
    fclose(m);

    int status = 0;
    waited = 0;
    while (waited < 10000) {
        if (waitpid(pid, &status, WNOHANG) == pid)
            break;
        usleep(20 * 1000);
        waited += 20;
    }
    if (waited >= 10000) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        fail_msg("the fixture never saw its marker");
    }
    while (got < sizeof(out) - 1) {
        struct pollfd p = { pipes[0], POLLIN, 0 };
        if (poll(&p, 1, 200) <= 0)
            break;
        ssize_t n = read(pipes[0], out + got, sizeof(out) - 1 - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    out[got] = '\0';
    close(pipes[0]);
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 0); /* the poll never killed it */
    assert_non_null(strstr(out, "done"));
    unlink(marker);
}

int main(void)
{
    /* a closed peer must yield EPIPE from socket writes, not a
     * process-killing signal (same policy as luna_main.c) */
    signal(SIGPIPE, SIG_IGN);

    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_socket_file_created),
        cmocka_unit_test(test_statement_gets_nil_reply),
        cmocka_unit_test(test_expression_evaluates_in_live_state),
        cmocka_unit_test(test_error_reply_is_framed_and_isolated),
        cmocka_unit_test(test_print_output_is_captured_into_frame),
        cmocka_unit_test(test_magic_runs_against_live_state),
        cmocka_unit_test(test_exit_magic_detaches_and_target_survives),
        cmocka_unit_test(test_completion_meta_line),
        cmocka_unit_test(test_wake_and_chmod_report_errors),
        /* destructive for the shared serve state: keep it late */
        cmocka_unit_test(test_stop_clears_socket_and_poll_noops),
        cmocka_unit_test(test_sigusr1_releases_idle_editor),
        cmocka_unit_test(test_sigusr1_then_clean_exit),
        cmocka_unit_test(test_attach_reaches_a_busy_script),
        cmocka_unit_test(test_a_broken_attach_poll_never_kills_the_chunk),
    };
    /* group-level setup: one shared state, tests build on each other
     * (statement then expression), order as declared */
    return cmocka_run_group_tests(tests, setup_serve, teardown_serve);
}
