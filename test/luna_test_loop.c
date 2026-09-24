/* luna_test_loop.c — the opt-in event loop: timers, immediates, keep-
 * alive semantics, the prepare hook's serve poll, ^C interruption, the
 * loop.fs async file operations, and loop.net client sockets against
 * real pthread echo servers (TCP on an ephemeral port + a unix path).
 *
 * The uv loop is process-global, so every test leaves it empty: each
 * case clears what it scheduled, then runs the loop until the close
 * callbacks drain. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <cmocka.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "luna_loop.h"
#include "luna_kernel.h"

static lua_State *L;

/* eval(code) -> one string from the stack */
static const char *eval_string(const char *code)
{
    static char buf[512];
    if (luaL_dostring(L, code) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        lua_pop(L, 1);
        snprintf(buf, sizeof(buf), "ERR: %s", msg ? msg : "?");
        return buf;
    }
    const char *s = luaL_tolstring(L, -1, NULL); /* booleans too */
    lua_pop(L, 2);
    snprintf(buf, sizeof(buf), "%s", s ? s : "(nil)");
    return buf;
}

static int setup_loop(void **state)
{
    (void)state;
    L = luaL_newstate();
    assert_non_null(L);
    luaL_openlibs(L); /* the cases use assert/table/tostring */
    /* glb=1 only for the test harness: real scripts reach the module
     * through require "loop", which glb=0 already serves */
    luaL_requiref(L, "loop", luaopen_luna_loop, 1);
    lua_pop(L, 1);
    /* a previous case must not leave anything ticking */
    assert_string_equal(eval_string("return loop.run()"), "true");
    return 0;
}

static int teardown_loop(void **state)
{
    (void)state;
    lua_close(L);
    L = NULL;
    return 0;
}

static void test_run_with_nothing_scheduled_returns(void **state)
{
    (void)state;
    assert_string_equal(eval_string("return loop.run()"), "true");
}

static void test_settimeout_fires_in_order_with_args(void **state)
{
    (void)state;
    /* swapped deadlines: the shorter fires first; trailing args reach
     * the callback as ... */
    assert_string_equal(eval_string(
        "t = {}\n"
        "loop.setTimeout(function(...) t[#t+1] = {...} end, 30, 'late')\n"
        "loop.setTimeout(function(...) t[#t+1] = {...} end, 5, 'early', 42)\n"
        "assert(loop.run())\n"
        "return t[1][1] .. t[1][2] .. ',' .. t[2][1]"), "early42,late");
}

static void test_interval_runs_until_cleared(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "n = 0\n"
        "h = loop.setInterval(function() n = n + 1 end, 5)\n"
        "loop.setTimeout(function() loop.clearInterval(h) end, 40)\n"
        "assert(loop.run())\n"
        "return (n >= 2 and n <= 8) and 'bounded' or n"), "bounded");
}

static void test_immediate_runs_within_the_run(void **state)
{
    (void)state;
    /* both fire in one pass; libuv's timers phase precedes check, so
     * only membership is asserted (Node makes no order promise either) */
    assert_string_equal(eval_string(
        "t = {}\n"
        "loop.setImmediate(function() t[#t+1] = 'imm' end)\n"
        "loop.setTimeout(function() t[#t+1] = 'time' end, 10)\n"
        "assert(loop.run())\n"
        "table.sort(t)\n"
        "return table.concat(t, ',')"), "imm,time");
}

static void test_clear_of_a_fired_oneshot_is_harmless(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "fired = false\n"
        "h = loop.setTimeout(function() fired = true end, 5)\n"
        "assert(loop.run())\n"
        "loop.clearTimeout(h)  -- already closed: no-op, must not crash\n"
        "return tostring(fired)"), "true");
}

static void test_stop_ends_the_run_handles_stay_scheduled(void **state)
{
    (void)state;
    /* stop() ends the run; the interval stays scheduled; clearing and
     * draining then work. The interval is 100ms so the first pass
     * cannot fire it before the poll — but libuv >= 1.53 runs timers
     * after the check phase each pass, so an interval that expires
     * during the poll still ticks once even though stop() landed in
     * the check (older libuv ended with 0). Pin the range, not the
     * phase order: libuv and Node promise no ordering here. */
    assert_string_equal(eval_string(
        "n = 0\n"
        "h = loop.setInterval(function() n = n + 1 end, 100)\n"
        "loop.setImmediate(function() loop.stop() end)\n"
        "assert(loop.run())\n"
        "local after = n\n"
        "loop.clearInterval(h)\n"
        "assert(loop.run())  -- drain the closing interval\n"
        "return (after >= 0 and after <= 1) and 'stopped' or after"),
        "stopped");
}

static void test_prepare_hook_steps_serve(void **state)
{
    (void)state;
    /* the loop's keep-alive hook gives the attach socket its poll */
    assert_string_equal(eval_string(
        "served = 0\n"
        "__LUNA_SERVE_STEP = function() served = served + 1 end\n"
        "loop.setTimeout(function() end, 30)\n"
        "assert(loop.run())\n"
        "__LUNA_SERVE_STEP = nil\n"
        "return served > 0 and 'polled' or 'silent'"), "polled");
}

static void test_interrupt_stops_the_run(void **state)
{
    (void)state;
    /* a ^C pending while uv_run blocks must surface as the same
     * "interrupted" error a busy script produces (exit 130 upstream) */
    luna_kernel_request_interrupt();
    assert_string_equal(eval_string(
        "h = loop.setTimeout(function() end, 10000)\n"
        "local ok, err = pcall(loop.run)\n"
        "loop.clearTimeout(h)\n"
        "assert(loop.run())  -- drain the cleared handle\n"
        "return tostring(ok) .. ',' .. tostring(tostring(err):find('interrupted') ~= nil)"),
        "false,true");
}

static void test_fs_write_then_read_roundtrip(void **state)
{
    (void)state;
    /* the nested readFile starts inside the write callback: the
     * keep-alive count must survive dipping back to zero between ops */
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.writeFile('/tmp/luna-loop-fs-test.txt', 'payload', function(e)\n"
        "  assert(e == nil, e)\n"
        "  fs.readFile('/tmp/luna-loop-fs-test.txt', function(e2, data)\n"
        "    out = tostring(e2 == nil) .. ':' .. tostring(data)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "true:payload");
}

static void test_fs_read_of_a_missing_file_yields_error(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.readFile('/tmp/luna-loop-fs-no-such-file', function(e, data)\n"
        "  out = tostring(e ~= nil) .. ',' .. tostring(data)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "true,nil");
}

static void test_fs_stat_reports_size(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.writeFile('/tmp/luna-loop-fs-test.txt', '12345', function(e)\n"
        "  assert(e == nil, e)\n"
        "  fs.stat('/tmp/luna-loop-fs-test.txt', function(e2, st)\n"
        "    out = tostring(e2 == nil) .. ':' .. tostring(st.size)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "true:5");
}

static void test_fs_write_of_empty_data_roundtrips(void **state)
{
    (void)state;
    /* regression: writeFile's success path once left the pcall one
     * argument short, so the call frame landed on stack garbage —
     * the empty write is the smallest such call */
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.writeFile('/tmp/luna-loop-fs-test.txt', '', function(e)\n"
        "  assert(e == nil, e)\n"
        "  fs.readFile('/tmp/luna-loop-fs-test.txt', function(e2, data)\n"
        "    out = tostring(e2 == nil) .. ':' .. tostring(data == '')\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "true:true");
}

/* -- net: a one-shot echo server on a real socket --------------------- */
/* accept one connection, echo one read back, then close both ends —
 * so the client sees its chunk, then EOF */
static void echo_serve(int listener)
{
    int c = accept(listener, NULL, NULL);
    char buf[256];
    ssize_t n = read(c, buf, sizeof buf);
    if (n > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(c, buf + off, (size_t)(n - off));
            if (w <= 0) {
                break;
            }
            off += w;
        }
    }
    close(c);
    close(listener);
}

static void *echo_main(void *arg)
{
    echo_serve((int)(intptr_t)arg);
    return NULL;
}

/* loopback listener on an ephemeral port; caller spawns echo_main */
static int tcp_listen_loopback(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert_int_equal(bind(fd, (struct sockaddr *)&addr, sizeof addr), 0);
    assert_int_equal(listen(fd, 1), 0);
    return fd;
}

static int tcp_port_of(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    getsockname(fd, (struct sockaddr *)&addr, &len);
    return ntohs(addr.sin_port);
}

static void test_net_tcp_echo_then_eof(void **state)
{
    (void)state;
    int listener = tcp_listen_loopback();
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, echo_main,
                                    (void *)(intptr_t)listener), 0);

    char code[768];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "log = {}\n"
        "net.connect('127.0.0.1', %d, function(e, sock)\n"
        "  if e then log[1] = 'connect:' .. e return end\n"
        "  sock:write('ping', function(e2)\n"
        "    if e2 then log[1] = 'write:' .. e2 return end\n"
        "    sock:read(function(e3, chunk)\n"
        "      log[1] = tostring(chunk)\n"
        "      sock:read(function(e4, eof)\n"
        "        log[2] = tostring(e4) .. '/' .. tostring(eof)\n"
        "        sock:close()\n"
        "      end)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return table.concat(log, ',')", tcp_port_of(listener));
    /* the echo comes back as one chunk; the server then closed, so the
     * second read is the EOF signal (err=nil, chunk=nil) */
    assert_string_equal(eval_string(code), "ping,nil/nil");
    pthread_join(th, NULL);
}

static void test_net_tcp_connect_refused_yields_error(void **state)
{
    (void)state;
    /* grab a free port, then release it: nothing is listening there */
    int fd = tcp_listen_loopback();
    int port = tcp_port_of(fd);
    close(fd);
    char code[320];
    snprintf(code, sizeof code,
        "out = 'none'\n"
        "loop.net.connect('127.0.0.1', %d, function(e, sock)\n"
        "  out = tostring(e ~= nil) .. ',' .. tostring(sock)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out", port);
    /* err in slot one, no sock; the failed socket closes itself and the
     * loop must still drain to a natural return */
    assert_string_equal(eval_string(code), "true,nil");
}

static void test_net_pipe_echo(void **state)
{
    (void)state;
    const char *path = "/tmp/luna-loop-net-test.sock";
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
    assert_int_equal(bind(fd, (struct sockaddr *)&addr, sizeof addr), 0);
    assert_int_equal(listen(fd, 1), 0);
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, echo_main,
                                    (void *)(intptr_t)fd), 0);

    assert_string_equal(eval_string(
        "local net = loop.net\n"
        "log = 'none'\n"
        "net.connectPipe('/tmp/luna-loop-net-test.sock', function(e, sock)\n"
        "  if e then log = 'connect:' .. e return end\n"
        "  sock:write('hello', function(e2)\n"
        "    if e2 then log = 'write:' .. e2 return end\n"
        "    sock:read(function(e3, chunk)\n"
        "      log = tostring(chunk)\n"
        "      sock:close()\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return log"), "hello");
    pthread_join(th, NULL);
    unlink(path);
}

/* -- net: luna as the server, pthread as the client ------------------- */

/* a real client in a thread: connect (waiting for the TCP port file or
 * the unix path to come alive), send, read the echo back into got */
struct client_spec {
    int use_tcp;
    const char *path; /* tcp: port file; pipe: socket path */
    int rc;
    char got[256];
};

static void *client_main(void *arg)
{
    struct client_spec *c = arg;
    int fd;
    if (c->use_tcp) {
        int port = 0;
        for (int i = 0; i < 5000 && !port; i++) {
            FILE *f = fopen(c->path, "r");
            if (f) {
                if (fscanf(f, "%d", &port) != 1) {
                    port = 0;
                }
                fclose(f);
            }
            if (!port) {
                usleep(1000);
            }
        }
        if (!port) {
            c->rc = 1; /* port file never appeared */
            return NULL;
        }
        fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
            close(fd);
            c->rc = 2;
            return NULL;
        }
    } else {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a;
        memset(&a, 0, sizeof a);
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, c->path, sizeof a.sun_path - 1);
        int ok = 0;
        for (int i = 0; i < 5000 && !ok; i++) {
            if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
                ok = 1;
            } else {
                usleep(1000);
            }
        }
        if (!ok) {
            close(fd);
            c->rc = 2;
            return NULL;
        }
    }
    if (write(fd, "ping", 4) != 4) {
        close(fd);
        c->rc = 3;
        return NULL;
    }
    ssize_t n = read(fd, c->got, sizeof c->got - 1);
    if (n <= 0) {
        close(fd);
        c->rc = 4;
        return NULL;
    }
    c->got[n] = 0;
    close(fd);
    return NULL;
}

/* the Lua echo server closes itself after the first echo so the run
 * can drain; the client meanwhile learns the port from the file the
 * listen callback wrote */
static void test_net_tcp_server_echo(void **state)
{
    (void)state;
    unlink("/tmp/luna-loop-net-srv.port");
    struct client_spec c;
    memset(&c, 0, sizeof c);
    c.use_tcp = 1;
    c.path = "/tmp/luna-loop-net-srv.port";
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, client_main, &c), 0);
    char code[768];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "served = 'none'\n"
        "local srv          -- declared first: the callback below runs\n"
        "srv = net.listen('127.0.0.1', 0, function(e, sock)\n"
        "  if e then served = 'conn:' .. e return end\n"
        "  sock:read(function(e2, chunk)\n"
        "    if chunk then\n"
        "      served = chunk\n"
        "      sock:write(chunk, function()\n"
        "        sock:close()\n"
        "        srv:close()\n"
        "      end)\n"
        "    else\n"
        "      sock:close()\n"
        "    end\n"
        "  end)\n"
        "end)\n"
        "local f = io.open('/tmp/luna-loop-net-srv.port', 'w')\n"
        "f:write(tostring(srv:port()))\n"
        "f:close()\n"
        "assert(loop.run())\n"
        "return served");
    const char *r = eval_string(code);
    pthread_join(th, NULL);
    assert_int_equal(c.rc, 0);
    assert_string_equal(c.got, "ping");
    assert_string_equal(r, "ping");
    unlink("/tmp/luna-loop-net-srv.port");
}

static void test_net_pipe_server_echo(void **state)
{
    (void)state;
    const char *path = "/tmp/luna-loop-net-srv.sock";
    unlink(path);
    struct client_spec c;
    memset(&c, 0, sizeof c);
    c.use_tcp = 0;
    c.path = path;
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, client_main, &c), 0);
    char code[768];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "served = 'none'\n"
        "local srv          -- declared first, see the tcp test above\n"
        "srv = net.listenPipe('%s', function(e, sock)\n"
        "  if e then served = 'conn:' .. e return end\n"
        "  sock:read(function(e2, chunk)\n"
        "    if chunk then\n"
        "      served = chunk\n"
        "      sock:write(chunk, function()\n"
        "        sock:close()\n"
        "        srv:close()\n"
        "      end)\n"
        "    else\n"
        "      sock:close()\n"
        "    end\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return served", path);
    const char *r = eval_string(code);
    pthread_join(th, NULL);
    assert_int_equal(c.rc, 0);
    assert_string_equal(c.got, "ping");
    assert_string_equal(r, "ping");
    unlink(path);
}

static void test_net_listen_on_taken_port_fails(void **state)
{
    (void)state;
    /* bind/listen report synchronously: EADDRINUSE throws, it does not
     * take a callback */
    int fd = tcp_listen_loopback();
    int port = tcp_port_of(fd);
    char code[256];
    snprintf(code, sizeof code,
        "local ok, err = pcall(loop.net.listen, '127.0.0.1', %d, "
        "function() end)\n"
        "return tostring(ok) .. ',' .. "
        "tostring(tostring(err):find('listen failed') ~= nil)", port);
    assert_string_equal(eval_string(code), "false,true");
    close(fd);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_run_with_nothing_scheduled_returns, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_settimeout_fires_in_order_with_args, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_interval_runs_until_cleared, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_immediate_runs_within_the_run, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_clear_of_a_fired_oneshot_is_harmless, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_stop_ends_the_run_handles_stay_scheduled, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_prepare_hook_steps_serve, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_interrupt_stops_the_run, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_write_then_read_roundtrip, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_read_of_a_missing_file_yields_error, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_stat_reports_size, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_write_of_empty_data_roundtrips, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_tcp_echo_then_eof, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_tcp_connect_refused_yields_error, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_pipe_echo, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_tcp_server_echo, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_pipe_server_echo, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_listen_on_taken_port_fails, setup_loop, teardown_loop),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
