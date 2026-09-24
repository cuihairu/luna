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
#include "luna_lua.h"

int luaopen_lpeg(lua_State *L);        /* introspect's pattern use */
int luaopen_socket_unix(lua_State *L); /* luasocket unix transport */

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
    static char buf[512];
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
    luaL_requiref(L, "lpeg", luaopen_lpeg, 0);
    lua_pop(L, 1);
    luaL_requiref(L, "socket.unix", luaopen_socket_unix, 0);
    lua_pop(L, 1);

    /* the embedded policy modules the serve chain requires */
    lua_pushlstring(L, LUNA_LUA_INTROSPECT, sizeof(LUNA_LUA_INTROSPECT) - 1);
    lua_setglobal(L, "__LUNA_INTROSPECT_SRC");
    lua_pushlstring(L, LUNA_LUA_SERVE, sizeof(LUNA_LUA_SERVE) - 1);
    lua_setglobal(L, "__LUNA_SERVE_SRC");
    run(L,
        "package.preload['luna.introspect'] = assert(load(__LUNA_INTROSPECT_SRC, '=(luna/introspect)'))\n"
        "package.preload['luna.serve'] = assert(load(__LUNA_SERVE_SRC, '=(luna/serve)'))\n"
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

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_socket_file_created),
        cmocka_unit_test(test_statement_gets_nil_reply),
        cmocka_unit_test(test_expression_evaluates_in_live_state),
        cmocka_unit_test(test_error_reply_is_framed_and_isolated),
        cmocka_unit_test(test_stop_clears_socket_and_poll_noops),
        cmocka_unit_test(test_sigusr1_releases_idle_editor),
    };
    /* group-level setup: one shared state, tests build on each other
     * (statement then expression), order as declared */
    return cmocka_run_group_tests(tests, setup_serve, teardown_serve);
}
