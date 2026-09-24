/* luna_test_loop.c — the opt-in event loop: timers, immediates, keep-
 * alive semantics, the prepare hook's serve poll, ^C interruption, the
 * loop.fs async file operations, loop.net client sockets against
 * real pthread echo servers (TCP on an ephemeral port + a unix path),
 * and loop.process aggregate child processes (capture, exit codes,
 * kill-by-signal, synchronous spawn failure).
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
#include <stdlib.h>
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

static void test_fs_stat_of_a_missing_path_yields_error(void **state)
{
    (void)state;
    /* regression: stat's callback ignored req->result, so a failed
     * stat was delivered as success with a zeroed statbuf */
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.stat('/tmp/luna-loop-fs-no-such-path', function(e, st)\n"
        "  out = tostring(e ~= nil) .. '|' .. tostring(st)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "true|nil");
}

static void test_fs_append_file_extends(void **state)
{
    (void)state;
    (void)system("rm -f /tmp/luna-loop-fs-append.txt"); /* O_APPEND accumulates across runs */
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.appendFile('/tmp/luna-loop-fs-append.txt', 'x', function(e)\n"
        "  assert(e == nil, e)\n"
        "  fs.appendFile('/tmp/luna-loop-fs-append.txt', 'y', function(e2)\n"
        "    assert(e2 == nil, e2)\n"
        "    fs.readFile('/tmp/luna-loop-fs-append.txt', function(e3, d)\n"
        "      out = tostring(e3 == nil) .. ':' .. tostring(d)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "true:xy");
}

static void test_fs_readdir_lists_entries(void **state)
{
    (void)state;
    (void)system("rm -rf /tmp/luna-loop-fs-dir");
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.mkdir('/tmp/luna-loop-fs-dir', function(e)\n"
        "  assert(e == nil, e)\n"
        "  fs.writeFile('/tmp/luna-loop-fs-dir/alpha.txt', 'a', function(e2)\n"
        "    assert(e2 == nil, e2)\n"
        "    fs.writeFile('/tmp/luna-loop-fs-dir/beta.md', 'b', function(e3)\n"
        "      assert(e3 == nil, e3)\n"
        "      fs.readdir('/tmp/luna-loop-fs-dir', function(e4, names)\n"
        "        assert(e4 == nil, e4)\n"
        "        table.sort(names)\n"
        "        out = table.concat(names, ',')\n"
        "        fs.readdir('/tmp/luna-loop-fs-no-such-dir', function(e5)\n"
        "          out = out .. '|' .. tostring(e5 ~= nil)\n"
        "        end)\n"
        "      end)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "alpha.txt,beta.md|true");
}

static void test_fs_mkdir_rmdir_roundtrip(void **state)
{
    (void)state;
    (void)system("rm -rf /tmp/luna-loop-fs-mk");
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.mkdir('/tmp/luna-loop-fs-mk', function(e)\n"
        "  assert(e == nil, e)\n"
        "  fs.stat('/tmp/luna-loop-fs-mk', function(e2, st)\n"
        "    assert(e2 == nil, e2)\n"
        "    local was_there = st.mode ~= nil\n"
        "    fs.rmdir('/tmp/luna-loop-fs-mk', function(e3)\n"
        "      assert(e3 == nil, e3)\n"
        "      fs.stat('/tmp/luna-loop-fs-mk', function(e4)\n"
        "        out = tostring(was_there) .. '|' .. tostring(e4 ~= nil)\n"
        "      end)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "true|true");
}

static void test_fs_unlink_removes_file(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.writeFile('/tmp/luna-loop-fs-unlink.txt', 'bye', function(e)\n"
        "  assert(e == nil, e)\n"
        "  fs.unlink('/tmp/luna-loop-fs-unlink.txt', function(e2)\n"
        "    assert(e2 == nil, e2)\n"
        "    fs.readFile('/tmp/luna-loop-fs-unlink.txt', function(e3)\n"
        "      out = tostring(e3 ~= nil)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "true");
}

static void test_fs_rename_moves_file(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.writeFile('/tmp/luna-loop-fs-src.txt', 'moved-payload', function(e)\n"
        "  assert(e == nil, e)\n"
        "  fs.rename('/tmp/luna-loop-fs-src.txt', '/tmp/luna-loop-fs-dst.txt',\n"
        "    function(e2)\n"
        "    assert(e2 == nil, e2)\n"
        "    fs.readFile('/tmp/luna-loop-fs-dst.txt', function(e3, data)\n"
        "      assert(e3 == nil, e3)\n"
        "      out = tostring(data)\n"
        "      fs.readFile('/tmp/luna-loop-fs-src.txt', function(e4)\n"
        "        out = out .. '|' .. tostring(e4 ~= nil)\n"
        "      end)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "moved-payload|true");
}

static void test_udp_send_recv_loopback(void **state)
{
    (void)state;
    /* two datagrams from one unbound sender: both arrive in order,
     * the sender's ephemeral port rides along in rinfo */
    assert_string_equal(eval_string(
        "local udp = loop.udp\n"
        "out = 'none'\n"
        "local got = {}\n"
        "local r, snd\n"
        "r = udp.bind('127.0.0.1', 0, function(e, data, ri)\n"
        "  if e then out = 'ERR:' .. e return end\n"
        "  got[#got + 1] = data\n"
        "  if #got == 2 then\n"
        "    out = table.concat(got, '|') .. '|' .. ri.addr .. '|' ..\n"
        "          tostring(ri.port > 0)\n"
        "    r:close()\n"
        "  end\n"
        "end)\n"
        "local port = r:port()\n"
        "snd = udp.socket()\n"
        "snd:send('x', '127.0.0.1', port, function(e2)\n"
        "  assert(e2 == nil, e2)\n"
        "  snd:send('y', '127.0.0.1', port, function(e3)\n"
        "    assert(e3 == nil, e3)\n"
        "    snd:close()\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "x|y|127.0.0.1|true");
}

static void test_udp_bind_conflict_throws(void **state)
{
    (void)state;
    /* the port is taken while r is still open: the second bind throws
     * synchronously, like listen */
    assert_string_equal(eval_string(
        "local udp = loop.udp\n"
        "out = 'none'\n"
        "local r\n"
        "r = udp.bind('127.0.0.1', 0, function() end)\n"
        "local ok, err = pcall(udp.bind, '127.0.0.1', r:port(), function() end)\n"
        "out = tostring(ok) .. '|' ..\n"
        "      tostring((tostring(err):find('in use')) ~= nil)\n"
        "r:close()\n"
        "assert(loop.run())\n"
        "return out"), "false|true");
}

static void test_udp_send_without_callback_drains(void **state)
{
    (void)state;
    /* cb-less send, auto-bind on the first send: the loop still
     * drains once both sockets close */
    assert_string_equal(eval_string(
        "local udp = loop.udp\n"
        "out = 'none'\n"
        "local r, snd\n"
        "r = udp.bind('127.0.0.1', 0, function(e, data)\n"
        "  out = tostring(data)\n"
        "  snd:close()\n"
        "  r:close()\n"
        "end)\n"
        "snd = udp.socket()\n"
        "snd:send('no-cb', '127.0.0.1', r:port())\n"
        "assert(loop.run())\n"
        "return out"), "no-cb");
}

static void test_fs_watch_reports_events(void **state)
{
    (void)state;
    /* a file written from an immediate lands in the watched directory:
     * the callback sees its name plus a rename/change event, and the
     * watch keeps the loop alive until close */
    (void)system("rm -rf /tmp/luna-loop-fs-watch && mkdir -p /tmp/luna-loop-fs-watch");
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "local got = {}\n"
        "local w\n"
        "w = fs.watch('/tmp/luna-loop-fs-watch', function(e, name, ev)\n"
        "  if e then out = 'ERR:' .. e return end\n"
        "  got[#got + 1] = tostring(name) .. '|' .. ev\n"
        "end)\n"
        "loop.setImmediate(function()\n"
        "  fs.writeFile('/tmp/luna-loop-fs-watch/note.txt', 'x', function() end)\n"
        "end)\n"
        "loop.setTimeout(function()\n"
        "  local ok = #got >= 1\n"
        "  for _, s in ipairs(got) do\n"
        "    local n, ev = s:match('^(.*)|(.*)$')\n"
        "    if n ~= 'note.txt' or (ev ~= 'rename' and ev ~= 'change') then\n"
        "      ok = false\n"
        "    end\n"
        "  end\n"
        "  out = tostring(ok)\n"
        "  w:close()\n"
        "end, 300)\n"
        "assert(loop.run())\n"
        "return out"), "true");
}

static void test_fs_watch_missing_path_throws(void **state)
{
    (void)state;
    /* a watch on a path that does not exist throws synchronously, like
     * listen on a taken port — nothing dangles afterwards */
    (void)system("rm -rf /tmp/luna-loop-fs-watch-no-such");
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "local ok, err = pcall(fs.watch, '/tmp/luna-loop-fs-watch-no-such',\n"
        "  function() end)\n"
        "local drained = loop.run()   -- nothing dangles after the throw\n"
        "return tostring(ok) .. '|' ..\n"
        "       tostring((tostring(err):find('no such')) ~= nil) .. '|' ..\n"
        "       tostring(drained)"), "false|true|true");
}

static void test_fs_watch_close_is_idempotent(void **state)
{
    (void)state;
    /* double close is a no-op, and a closed watch never delivers: the
     * late write lands, the callback stays silent, the loop drains */
    (void)system("rm -rf /tmp/luna-loop-fs-watch && mkdir -p /tmp/luna-loop-fs-watch");
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "local hit = false\n"
        "local w = fs.watch('/tmp/luna-loop-fs-watch', function() hit = true end)\n"
        "w:close()\n"
        "w:close()\n"
        "loop.setImmediate(function()\n"
        "  fs.writeFile('/tmp/luna-loop-fs-watch/late.txt', 'x', function() end)\n"
        "end)\n"
        "loop.setTimeout(function()\n"
        "  out = tostring(hit == false)\n"
        "end, 100)\n"
        "assert(loop.run())\n"
        "return out"), "true");
}

static void test_immediate_beats_timer_with_io_watchers(void **state)
{
    (void)state;
    /* regression: with an I/O watcher around, the poll phase used to
     * sleep until the next timer and starve the immediate (a bare check
     * handle cannot wake it) — the kick sentinel must run the immediate
     * first, so the order is imm,timer */
    (void)system("rm -rf /tmp/luna-loop-fs-watch && mkdir -p /tmp/luna-loop-fs-watch");
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "local order = {}\n"
        "local w\n"
        "w = fs.watch('/tmp/luna-loop-fs-watch', function() end)\n"
        "loop.setImmediate(function() order[#order + 1] = 'imm' end)\n"
        "loop.setTimeout(function()\n"
        "  order[#order + 1] = 'timer'\n"
        "  out = table.concat(order, ',')\n"
        "  w:close()\n"
        "end, 50)\n"
        "assert(loop.run())\n"
        "return out"), "imm,timer");
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

/* -- process: aggregate child processes -------------------------------- */

static void test_proc_run_echo_captures_stdout(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local res\n"
        "local process = loop.process\n"
        "process.run('echo', {'hello'}, function(e, r)\n"
        "  assert(e == nil)\n"
        "  res = r\n"
        "end)\n"
        "assert(loop.run())\n"
        "return res.status .. '|' .. tostring(res.signal) .. '|"
        "' .. res.stdout .. '|' .. res.stderr"), "0|nil|hello\n|");
}

static void test_proc_run_exit_code_and_stderr(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local res\n"
        "local process = loop.process\n"
        "process.run('sh', {'-c', 'echo to-err >&2; exit 3'},\n"
        "  function(e, r) res = r end)\n"
        "assert(loop.run())\n"
        "return res.status .. '|' .. tostring(res.signal) .. '|"
        "' .. res.stderr .. '|' .. res.stdout"), "3|nil|to-err\n|");
}

static void test_proc_run_cwd_option(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local out\n"
        "local process = loop.process\n"
        "process.run('pwd', {}, {cwd = '/tmp'},\n"
        "  function(e, r) out = r.stdout end)\n"
        "assert(loop.run())\n"
        "return out"), "/tmp\n");
}

static void test_proc_kill_reports_signal(void **state)
{
    (void)state;
    /* run() returns the handle at once: pid is readable and kill()
     * lands from a setImmediate inside the same run */
    assert_string_equal(eval_string(
        "local res\n"
        "local process = loop.process\n"
        "local p = process.run('sleep', {'30'},\n"
        "  function(e, r) res = r end)\n"
        "assert(p:pid() > 0)\n"
        "loop.setImmediate(function() assert(p:kill(15)) end)\n"
        "assert(loop.run())\n"
        "return tostring(res.signal)"), "15");
}

static void test_proc_spawn_failure_throws_and_drains(void **state)
{
    (void)state;
    /* spawn errors throw synchronously (like listen) AND must leave no
     * handle behind — the run below still drains and returns */
    assert_string_equal(eval_string(
        "local process = loop.process\n"
        "local ok, err = pcall(process.run, "
        "'luna-definitely-not-a-command-xyz', {}, function() end)\n"
        "assert(not ok)\n"
        "assert(loop.run())\n"
        "return (tostring(err):find('spawn failed') ~= nil) "
        "and 'threw-and-drained' or tostring(err)"),
        "threw-and-drained");
}

/* regression: a chunk delivery used to consume the read callback, so
 * only the FIRST chunk (and no EOF) ever reached Lua. Two writes with
 * a pause in between force two separate chunk events plus the EOF. */
static void *two_chunks_main(void *arg)
{
    int fd = (int)(intptr_t)arg;
    int c = accept(fd, NULL, NULL);
    if (c < 0) {
        return NULL;
    }
    ssize_t n = write(c, "a", 1);
    (void)n;
    usleep(200 * 1000);
    n = write(c, "b", 1);
    (void)n;
    close(c);
    return NULL;
}

static void test_net_read_survives_multiple_chunks(void **state)
{
    (void)state;
    int fd = tcp_listen_loopback();
    int port = tcp_port_of(fd);
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, two_chunks_main,
                                    (void *)(intptr_t)fd), 0);
    char code[512];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "local got, done = '', nil\n"
        "net.connect('127.0.0.1', %d, function(e, sock)\n"
        "  assert(e == nil)\n"
        "  sock:read(function(e2, chunk)\n"
        "    if chunk then got = got .. chunk\n"
        "    else done = 'eof' sock:close() end\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return got .. '|' .. tostring(done)", port);
    assert_string_equal(eval_string(code), "ab|eof");
    pthread_join(th, NULL);
    close(fd);
}

/* -- process.spawn: live stdio as ordinary socks ----------------------- */

static void test_proc_spawn_cat_roundtrip(void **state)
{
    (void)state;
    /* stdin write + half-close, stdout read to EOF: the full sock face
     * over a child's stdio pipes */
    assert_string_equal(eval_string(
        "local process = loop.process\n"
        "local got, code\n"
        "local p = process.spawn('cat', {}, function(e, r)\n"
        "  code = r.status .. '/' .. tostring(r.signal)\n"
        "end)\n"
        "p:stdout():read(function(e2, chunk)\n"
        "  if chunk then got = chunk else p:stdout():close() end\n"
        "end)\n"
        "p:stdin():write('ping', function(e3)\n"
        "  assert(e3 == nil)\n"
        "  p:stdin():shutdown(function() p:stdin():close() end)\n"
        "end)\n"
        "p:stderr():close()          -- unread streams must be closed\n"
        "assert(loop.run())\n"
        "return got .. '|' .. code"), "ping|0/nil");
}

static void test_proc_spawn_stderr_is_separate(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local process = loop.process\n"
        "local got\n"
        "local p = process.spawn('sh', {'-c', 'echo err-out >&2'},\n"
        "  function() end)\n"
        "local se = p:stderr()       -- hoist: sh exits at once, so the\n"
        "se:read(function(e, chunk)  -- proc may deliver before this pipe\n"
        "  if chunk then got = chunk else se:close() end  -- EOFs; by then\n"
        "end)                        -- p:stderr() is nil by contract\n"
        "p:stdin():close()\n"
        "p:stdout():close()\n"
        "assert(loop.run())\n"
        "return got"), "err-out\n");
}

static void test_proc_spawn_exit_fires_without_readers(void **state)
{
    (void)state;
    /* onExit mirrors Node 'exit': it fires on process exit even though
     * stdout was never read (its EOF can only be seen by a reader) —
     * and unread socks still close cleanly, so the run drains */
    assert_string_equal(eval_string(
        "local process = loop.process\n"
        "local sig\n"
        "local p = process.spawn('sleep', {'30'}, function(e, r)\n"
        "  sig = tostring(r.signal)\n"
        "end)\n"
        "loop.setImmediate(function() assert(p:kill(15)) end)\n"
        "p:stdin():close()\n"
        "p:stdout():close()\n"
        "p:stderr():close()\n"
        "assert(loop.run())\n"
        "return sig"), "15");
}

static void test_proc_spawn_stdio_become_nil_after_exit(void **state)
{
    (void)state;
    /* ownership is released at onExit: the accessors hand out the
     * socks while the child runs, nil afterwards */
    assert_string_equal(eval_string(
        "local process = loop.process\n"
        "local before, after\n"
        "local p                       -- split decl: the callback reads p\n"
        "p = process.spawn('echo', {'x'}, function()\n"
        "  after = tostring(p:stdout())\n"
        "end)\n"
        "before = tostring(p:stdout())\n"
        "p:stdin():close()\n"
        "p:stdout():close()\n"
        "p:stderr():close()\n"
        "assert(loop.run())\n"
        "return tostring(before:find('loop.sock') ~= nil) .. '|' .. after"),
        "true|nil");
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
        cmocka_unit_test_setup_teardown(test_fs_stat_of_a_missing_path_yields_error, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_append_file_extends, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_readdir_lists_entries, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_mkdir_rmdir_roundtrip, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_unlink_removes_file, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_rename_moves_file, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_udp_send_recv_loopback, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_udp_bind_conflict_throws, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_udp_send_without_callback_drains, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_watch_reports_events, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_watch_missing_path_throws, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_watch_close_is_idempotent, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_immediate_beats_timer_with_io_watchers, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_tcp_echo_then_eof, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_tcp_connect_refused_yields_error, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_pipe_echo, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_tcp_server_echo, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_pipe_server_echo, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_listen_on_taken_port_fails, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_read_survives_multiple_chunks, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_run_echo_captures_stdout, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_run_exit_code_and_stderr, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_run_cwd_option, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_kill_reports_signal, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_spawn_failure_throws_and_drains, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_spawn_cat_roundtrip, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_spawn_stderr_is_separate, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_spawn_exit_fires_without_readers, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_spawn_stdio_become_nil_after_exit, setup_loop, teardown_loop),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
