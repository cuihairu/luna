/* luna_test_loop.c — the opt-in event loop: timers, immediates, keep-
 * alive semantics, the prepare hook's serve poll, ^C interruption, the
 * loop.fs async file operations, loop.net client sockets against
 * real pthread echo servers (TCP on an ephemeral port + a unix path),
 * the sock:peer/sockname and server:address reporting face,
 * TLS client and server sockets against a baked-in self-signed
 * certificate, loop.process aggregate child processes (capture,
 * exit codes, kill-by-signal, synchronous spawn failure), and the
 * pure-Lua loop.http client (plain + TLS roundtrips, chunked framing,
 * request wiring, redirect chains with method folding, relative
 * Location, the whole-request timeout, url validation) and its server
 * face (self-served roundtrips, handler errors as 500, malformed
 * requests as 400), plus streaming bodies through onData/onHead
 * (content-length, unframed EOF and redirect hops staying internal).
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
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <cmocka.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "luna_loop.h"
#include "luna_cov.h"
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
    /* the binary ignores SIGPIPE (see luna_main.c) so a write to a dead
     * peer surfaces as EPIPE; the harness bypasses main(), so it adopts
     * that protection itself — the tls cases write to a closed conn */
    signal(SIGPIPE, SIG_IGN);
    L = luaL_newstate();
    assert_non_null(L);
    luaL_openlibs(L); /* the cases use assert/table/tostring */
    luna_cov_setup(L);
    /* glb=1 only for the test harness: real scripts reach the module
     * through require "loop", which glb=0 already serves */
    luaL_requiref(L, "loop", luaopen_luna_loop, 1);
    lua_pop(L, 1);
    /* the staged module tree serves require "loop.http" */
    assert_string_equal(eval_string(
        "package.path = '" LUNA_TEST_MODULES_DIR "/?.lua;"
        LUNA_TEST_MODULES_DIR "/?/init.lua;' .. package.path\n"
        "return type(require('loop.http').get)"), "function");
    /* a previous case must not leave anything ticking */
    assert_string_equal(eval_string("return loop.run()"), "true");
    return 0;
}

static int teardown_loop(void **state)
{
    (void)state;
    luna_cov_teardown(L);
    /* drain pending closes (uv_close finishes only when the loop turns)
     * while THIS state is still alive: a closing callback touching a
     * freed VM from the next case's run() is heap corruption */
    lua_getglobal(L, "loop");
    lua_getfield(L, -1, "run");
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
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
    /* count-driven, not wall-clock-driven: the interval counts itself
     * to 3, clears, and a setTimeout(0) verifier — which can only run
     * in a later timers phase — proves the ticks stopped. All three
     * steps hold no matter how slowly the loop is scheduled. */
    assert_string_equal(eval_string(
        "n = 0\n"
        "h = loop.setInterval(function()\n"
        "  n = n + 1\n"
        "  if n == 3 then\n"
        "    loop.clearInterval(h)\n"
        "    loop.setTimeout(function() assert(n == 3) end, 0)\n"
        "  end\n"
        "end, 5)\n"
        "assert(loop.run())\n"
        "return (n == 3) and 'stopped' or n"), "stopped");
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

static void test_fs_copyfile_copies_and_overwrites(void **state)
{
    (void)state;
    (void)system("rm -f /tmp/luna-loop-fs-cp-src.txt /tmp/luna-loop-fs-cp-dst.txt");
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.writeFile('/tmp/luna-loop-fs-cp-src.txt', 'copy-me', function(e)\n"
        "  assert(e == nil, e)\n"
        "  fs.copyFile('/tmp/luna-loop-fs-cp-src.txt', '/tmp/luna-loop-fs-cp-dst.txt', function(e2)\n"
        "    assert(e2 == nil, e2)\n"
        "    fs.readFile('/tmp/luna-loop-fs-cp-dst.txt', function(e3, d)\n"
        "      assert(e3 == nil, e3)\n"
        "      out = tostring(d)\n"
        "      fs.writeFile('/tmp/luna-loop-fs-cp-src.txt', 'other', function(e4)\n"
        "        assert(e4 == nil, e4)\n"
        "        fs.copyFile('/tmp/luna-loop-fs-cp-src.txt', '/tmp/luna-loop-fs-cp-dst.txt', function(e5)\n"
        "          assert(e5 == nil, e5)\n"
        "          fs.readFile('/tmp/luna-loop-fs-cp-dst.txt', function(e6, d2)\n"
        "            out = out .. '|' .. tostring(d2)\n"
        "          end)\n"
        "        end)\n"
        "      end)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "copy-me|other");
}

static void test_fs_access_probes_existence(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.writeFile('/tmp/luna-loop-fs-test.txt', 'x', function(e)\n"
        "  assert(e == nil, e)\n"
        "  fs.access('/tmp/luna-loop-fs-test.txt', function(e2)\n"
        "    out = tostring(e2 == nil)\n"
        "    fs.access('/tmp/luna-loop-fs-no-such-path', function(e3, v)\n"
        "      out = out .. '|' .. tostring(e3 ~= nil) .. '|' .. tostring(v)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "true|true|nil");
}

static void test_fs_realpath_resolves(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.writeFile('/tmp/luna-loop-fs-real.txt', 'x', function(e)\n"
        "  assert(e == nil, e)\n"
        "  fs.realpath('/tmp/../tmp/luna-loop-fs-real.txt', function(e2, p)\n"
        "    out = tostring(e2 == nil) .. ':' .. tostring(p == '/tmp/luna-loop-fs-real.txt')\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "true:true");
}

static void test_fs_truncate_shortens_and_defaults_to_zero(void **state)
{
    (void)state;
    (void)system("rm -f /tmp/luna-loop-fs-trunc.txt");
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.writeFile('/tmp/luna-loop-fs-trunc.txt', 'hello world', function(e)\n"
        "  assert(e == nil, e)\n"
        "  fs.truncate('/tmp/luna-loop-fs-trunc.txt', 5, function(e2)\n"
        "    assert(e2 == nil, e2)\n"
        "    fs.readFile('/tmp/luna-loop-fs-trunc.txt', function(e3, d)\n"
        "      assert(e3 == nil, e3)\n"
        "      out = tostring(d)\n"
        "      fs.truncate('/tmp/luna-loop-fs-trunc.txt', function(e4)\n"
        "        assert(e4 == nil, e4)\n"
        "        fs.readFile('/tmp/luna-loop-fs-trunc.txt', function(e5, d2)\n"
        "          out = out .. '|' .. tostring(d2 == '')\n"
        "        end)\n"
        "      end)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "hello|true");
}

static void test_fs_stat_and_lstat_report_types(void **state)
{
    (void)state;
    (void)system("rm -rf /tmp/luna-loop-fs-tydir /tmp/luna-loop-fs-ty.txt /tmp/luna-loop-fs-ty.link");
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.writeFile('/tmp/luna-loop-fs-ty.txt', 'x', function(e)\n"
        "  assert(e == nil, e)\n"
        "  fs.mkdir('/tmp/luna-loop-fs-tydir', function(e2)\n"
        "    assert(e2 == nil, e2)\n"
        "    os.execute('ln -sf /tmp/luna-loop-fs-ty.txt /tmp/luna-loop-fs-ty.link')\n"
        "    fs.stat('/tmp/luna-loop-fs-ty.txt', function(e3, st)\n"
        "      assert(e3 == nil, e3)\n"
        "      fs.stat('/tmp/luna-loop-fs-tydir', function(e4, sd)\n"
        "        assert(e4 == nil, e4)\n"
        "        fs.lstat('/tmp/luna-loop-fs-ty.link', function(e5, ls)\n"
        "          assert(e5 == nil, e5)\n"
        "          fs.stat('/tmp/luna-loop-fs-ty.link', function(e6, fs_st)\n"
        "            out = st.type .. ',' .. sd.type .. ',' .. ls.type .. ',' .. fs_st.type\n"
        "          end)\n"
        "        end)\n"
        "      end)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "file,dir,link,file");
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

static void test_udp_sockname_reports_bind(void **state)
{
    (void)state;
    /* sockname: a bound socket reports its bind; a sender reports the
     * ephemeral wildcard uv picked on its first send */
    assert_string_equal(eval_string(
        "local udp = loop.udp\n"
        "out = 'none'\n"
        "local r, snd\n"
        "r = udp.bind('127.0.0.1', 0, function() end)\n"
        "local a = r:sockname()\n"
        "local base = a.address .. '|' .. a.family .. '|'\n"
        "          .. tostring(a.port == r:port())\n"
        "snd = udp.socket()\n"
        "snd:send('x', '127.0.0.1', r:port(), function(e2)\n"
        "  assert(e2 == nil, e2)\n"
        "  local b = snd:sockname()\n"
        "  out = base .. '|' .. tostring(b.port > 0) .. '|'\n"
        "      .. b.family\n"
        "  snd:close()\n"
        "  r:close()\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "127.0.0.1|inet|true|true|inet");
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
    /* A file written from an immediate lands in the watched directory:
     * the callback sees its name plus a rename/change event, and the
     * watch keeps the loop alive until close.
     *
     * The verdict is computed by the event itself, not by a fixed
     * deadline — a 300ms window loses the race on a loaded box (the
     * inotify event is what arrives late, not the write). The timer is
     * only the bail-out that keeps a lost event a failure instead of a
     * hang, and it is cleared the moment the watch has its say. */
    (void)system("rm -rf /tmp/luna-loop-fs-watch && mkdir -p /tmp/luna-loop-fs-watch");
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "local got = {}\n"
        "local bail\n"
        "local function judge()\n"
        "  local ok = #got >= 1\n"
        "  for _, s in ipairs(got) do\n"
        "    local n, ev = s:match('^(.*)|(.*)$')\n"
        "    if n ~= 'note.txt' or (ev ~= 'rename' and ev ~= 'change') then\n"
        "      ok = false\n"
        "    end\n"
        "  end\n"
        "  return tostring(ok)\n"
        "end\n"
        "local w\n"
        "local function finish(v)\n"
        "  out = v\n"
        "  w:close()\n"
        "  loop.clearTimeout(bail)\n"
        "end\n"
        "w = fs.watch('/tmp/luna-loop-fs-watch', function(e, name, ev)\n"
        "  if e then finish('ERR:' .. e) return end\n"
        "  got[#got + 1] = tostring(name) .. '|' .. ev\n"
        "  if name == 'note.txt' then finish(judge()) end\n"
        "end)\n"
        "bail = loop.setTimeout(function() finish(judge()) end, 2000)\n"
        "loop.setImmediate(function()\n"
        "  fs.writeFile('/tmp/luna-loop-fs-watch/note.txt', 'x', function() end)\n"
        "end)\n"
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

static void test_signal_self_delivery(void **state)
{
    (void)state;
    /* register a USR2 watcher, raise the signal while the loop is not
     * running, then let run() deliver it — the resident callback sees
     * the signal number and closes the watcher */
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "out = 'none'\n"
        "local w\n"
        "w = loop.signal(loop.sig.USR2, function(num)\n"
        "  out = 'got:' .. num\n"
        "  w:close()\n"
        "end)\n"
        "return 'armed'"), "armed");
    kill(getpid(), SIGUSR2);
    assert_string_equal(eval_string(
        "assert(loop.run())\n"
        "return out"), "got:12");
}

static void test_signal_reserved_refused(void **state)
{
    (void)state;
    /* SIGINT stays the loop's ^C interrupt and SIGUSR1 is the attach
     * doorbell: loop.signal refuses both synchronously */
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "local ok1, e1 = pcall(loop.signal, loop.sig.INT, function() end)\n"
        "local ok2, e2 = pcall(loop.signal, loop.sig.USR1, function() end)\n"
        "return tostring(ok1) .. '|' .. tostring(ok2)"),
        "false|false");
}

static void test_signal_multiple_watchers_fanout(void **state)
{
    (void)state;
    /* libuv delivers one signal to every watcher watching it —
     * delivery ORDER is not part of the contract (it flipped to
     * "ba" under CI load), so sort before comparing */
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "out = 'none'\n"
        "local hits = {}\n"
        "local wa, wb\n"
        "wa = loop.signal(loop.sig.USR2, function()\n"
        "  hits[#hits + 1] = 'a'\n"
        "  if #hits == 2 then\n"
        "    table.sort(hits)\n"
        "    out = table.concat(hits, '')\n"
        "    wa:close()\n"
        "    wb:close()\n"
        "  end\n"
        "end)\n"
        "wb = loop.signal(loop.sig.USR2, function()\n"
        "  hits[#hits + 1] = 'b'\n"
        "  if #hits == 2 then\n"
        "    table.sort(hits)\n"
        "    out = table.concat(hits, '')\n"
        "    wa:close()\n"
        "    wb:close()\n"
        "  end\n"
        "end)\n"
        "return 'armed'"), "armed");
    kill(getpid(), SIGUSR2);
    assert_string_equal(eval_string(
        "assert(loop.run())\n"
        "return out"), "ab");
}

static void test_signal_close_stops_delivery(void **state)
{
    (void)state;
    /* a closed watcher never delivers, but one watcher must stay on the
     * signal: libuv restores SIG_DFL when the LAST watcher closes, and
     * the default disposition for SIGUSR2 kills us mid-test */
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "out = 'none'\n"
        "local hits = {}\n"
        "local wb\n"
        "wb = loop.signal(loop.sig.USR2, function()\n"
        "  hits[#hits + 1] = 'b'\n"
        "  out = table.concat(hits, ',')\n"
        "  wb:close()\n"
        "end)\n"
        "local wa = loop.signal(loop.sig.USR2, function()\n"
        "  hits[#hits + 1] = 'a'\n"
        "end)\n"
        "wa:close()               -- wb stays: disposition stays installed\n"
        "loop.setTimeout(function()\n"
        "  if out == 'none' then out = 'TIMEOUT' end\n"
        "  wb:close()\n"
        "end, 150)\n"
        "return 'armed'"), "armed");
    kill(getpid(), SIGUSR2);
    assert_string_equal(eval_string(
        "assert(loop.run())\n"
        "return out"), "b");
}

static void test_dns_lookup_resolves_localhost(void **state)
{
    (void)state;
    /* /etc/hosts names resolve on the threadpool; lookup promises the
     * system's first address, which for localhost is 127.0.0.1 here
     * but ::1 on runners where glibc orders v6 first (RFC 6724) —
     * pin "resolves to a loopback address", not the family */
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "out = 'none'\n"
        "loop.dns.lookup('localhost', function(e, addr)\n"
        "  local ok = addr == '127.0.0.1' or addr == '::1'\n"
        "  out = tostring(e == nil) .. ':' .. tostring(ok)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "true:true");
}

static void test_dns_lookup_reports_failure_through_the_callback(void **state)
{
    (void)state;
    /* A >63-char label fails while the query is being BUILT, before any
     * packet leaves the machine. RFC 2606's .invalid was the old choice
     * here, but DNS-capturing proxies (faux-IP mode) resolve even that
     * — so the injection has to fail below the resolver's reach. */
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "out = 'none'\n"
        "loop.dns.lookup(string.rep('a', 70) .. '.invalid', function(e, addr)\n"
        "  out = tostring(e ~= nil) .. '|' .. tostring(addr)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "true|nil");
}

/* libuv refuses an over-long hostname synchronously: lookup throws and
 * its cleanup path unpins the callback without the loop ever turning */
static void test_dns_lookup_of_an_overlong_host_throws(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "local ok, err = pcall(loop.dns.lookup, string.rep('a', 300) .. '.invalid', function() end)\n"
        "return tostring(ok) .. '|' .. tostring(tostring(err):find('invalid argument') ~= nil)"),
        "false|true");
}

/* reverse only takes numeric addresses; a name throws before anything
 * is pinned or the loop starts */
static void test_dns_reverse_of_a_non_address_throws(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "local ok, err = pcall(loop.dns.reverse, 'no-such-host.invalid', function() end)\n"
        "return tostring(ok) .. '|' .. tostring(tostring(err):find('not a numeric address') ~= nil)"),
        "false|true");
}

static void test_dns_reverse_maps_loopback(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "out = 'none'\n"
        "loop.dns.reverse('127.0.0.1', function(e, name)\n"
        "  out = tostring(e == nil) .. ':' .. tostring(name)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"), "true:localhost");
}

static void test_os_basics_report_sane_values(void **state)
{
    (void)state;
    /* loop.os is synchronous — no run() needed, values come straight
     * from libuv's info calls */
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "local os = loop.os\n"
        "local la = os.loadavg()\n"
        "return tostring(os.home():sub(1, 1) == '/') .. ',' ..\n"
        "  tostring(os.tmpdir():sub(1, 1) == '/') .. ',' ..\n"
        "  tostring(#os.hostname() > 0) .. ',' ..\n"
        "  tostring(os.type() == 'Linux') .. ',' ..\n"
        "  tostring(os.uptime() > 0) .. ',' ..\n"
        "  tostring(#la == 3) .. ',' ..\n"
        "  tostring(os.freemem() > 0 and os.totalmem() > 0)"), "true,true,true,true,true,true,true");
}

static void test_os_cpus_lists_each_with_times(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "local cpus = loop.os.cpus()\n"
        "local c = cpus[1]\n"
        "return tostring(#cpus > 0) .. ',' ..\n"
        "  tostring(type(c.model) == 'string') .. ',' ..\n"
        "  tostring(type(c.speed) == 'number') .. ',' ..\n"
        "  tostring(type(c.times) == 'table' and c.times.idle >= 0)"), "true,true,true,true");
}

static void test_os_network_interfaces_lists_loopback(void **state)
{
    (void)state;
    /* the loopback interface is always there; its first address is a
     * loopback address, and internal is true — family may be either
     * v4 or v6 depending on ordering */
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "local ni = loop.os.networkInterfaces()\n"
        "local lo = ni.lo and ni.lo[1]\n"
        "if not lo then return 'no-lo' end\n"
        "local ok = lo.address == '127.0.0.1' or lo.address == '::1'\n"
        "return tostring(ok) .. ',' .. tostring(lo.internal) .. ',' ..\n"
        "  tostring(lo.family == 'IPv4' or lo.family == 'IPv6') .. ',' ..\n"
        "  tostring(#lo.mac == 17)"), "true,true,true,true");
}

static void test_unref_interval_does_not_keep_loop_alive(void **state)
{
    (void)state;
    (void)state;
    /* an unref'd interval runs while the loop turns but does not keep
     * it alive: run() drains on the ref'd timer and returns */
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "local iv\n"
        "iv = loop.setInterval(function() end, 40)\n"
        "iv:unref()\n"
        "loop.setTimeout(function()\n"
        "  loop.clearInterval(iv)\n"
        "  out = 'drained'\n"
        "end, 120)\n"
        "assert(loop.run())\n"
        "return 'drained'"), "drained");
}

static void test_unref_server_runs_but_does_not_keep(void **state)
{
    (void)state;
    /* an unref'd server is alive and reachable from a later callback,
     * yet the loop drains on the ref'd timer alone */
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "local net = loop.net\n"
        "out = 'none'\n"
        "local srv\n"
        "srv = net.listen('127.0.0.1', 0, function() end)\n"
        "srv:unref()\n"
        "loop.setTimeout(function()\n"
        "  out = tostring(srv ~= nil)\n"
        "  srv:close()\n"
        "end, 60)\n"
        "assert(loop.run())\n"
        "return out"), "true");
}

static void test_ref_restores_keepalive(void **state)
{
    (void)state;
    /* ref after unref restores the keep-alive: the timer fires and the
     * loop waits for it */
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "out = 'none'\n"
        "local t = loop.setTimeout(function() out = 'fired' end, 80)\n"
        "t:unref()\n"
        "t:ref()\n"
        "assert(loop.run())\n"
        "return out"), "fired");
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

#ifdef LUNA_LOOP_HAVE_OPENSSL
/* -- net: connectTls against a one-shot TLS echo server --------------- */

#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/pem.h>

/* a throwaway self-signed certificate (CN=localhost, SAN carrying
 * DNS:localhost and IP:127.0.0.1) baked in at build time, so the TLS
 * cases need no openssl binary and no fixture files at runtime */
static const char *TLS_TEST_CERT =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDJzCCAg+gAwIBAgIUTicMsl/o/ZyMM0t14vgKkSe6h7owDQYJKoZIhvcNAQEL\n"
    "BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MDkyNDIzNTUxNloYDzIxMjYw\n"
    "ODMxMjM1NTE2WjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEB\n"
    "AQUAA4IBDwAwggEKAoIBAQDbf+KLpqIq2JdldVkIZMg745ANHF6PRFK3trVbG20U\n"
    "wp+flm726jVJDGTTcAHuP/DzD4afzJ842geUvq9KXDRvuQvgpRHpsddr1B/VryVK\n"
    "FZG5d/Gddo5M2r5P+q4tZ1boCoWB0YtplP0lTq62DSNKkop7+TYey+ls5/TZ099V\n"
    "F2DFNByp1jydQ9Fqk2nDk+atZCrXbyAI3BZ+cIzfA9Ftqg3FYF424+5HaD8MIvdO\n"
    "PYOXuFi82Wcn9cznUEBjf6b1vzIm6T3LVzEyybTQaeENpYb1xKRrTDMxvs2vgluh\n"
    "m2ZJI7Jk6SmgBB/6y8Zf4atmOkQ+1Hg2TXSjgQragzmlAgMBAAGjbzBtMB0GA1Ud\n"
    "DgQWBBTMVTU1snr9UXgI1ZQzbT8w0Xi31jAfBgNVHSMEGDAWgBTMVTU1snr9UXgI\n"
    "1ZQzbT8w0Xi31jAPBgNVHRMBAf8EBTADAQH/MBoGA1UdEQQTMBGCCWxvY2FsaG9z\n"
    "dIcEfwAAATANBgkqhkiG9w0BAQsFAAOCAQEAc0MbjFbEHeX8YRb2s37hyTNiCE81\n"
    "FK+RK/5a/q9P+7ac11qZnZCA2O1WgZp3SDpkYT4wQ36pOJO3v3T4Q4zVrEeG67EQ\n"
    "JOVRSjjaXpe7WpEuHUfFJcmxE5A0p7uAJ1xuGaOm8hn43e3wwiUy/nLbC/GzbQX3\n"
    "tyorwLVK036JiMM7YuflKbDqb8G1YXvinPjIXl57Y/fPJFwUtGjVyU5kgP0hn3ag\n"
    "XWfR+Xo+ijTE9mNFXH0EB3xPTfgBflCaaPL9pSVdhZq54c6Tps8nZEhFnOmV2gUz\n"
    "f3LRiUhn6v+3hF5HdaQB3KuffDQMvzZeef1ToRNNU4SzBJGlLaAOjeu3PQ==\n"
    "-----END CERTIFICATE-----\n";
static const char *TLS_TEST_KEY =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDbf+KLpqIq2Jdl\n"
    "dVkIZMg745ANHF6PRFK3trVbG20Uwp+flm726jVJDGTTcAHuP/DzD4afzJ842geU\n"
    "vq9KXDRvuQvgpRHpsddr1B/VryVKFZG5d/Gddo5M2r5P+q4tZ1boCoWB0YtplP0l\n"
    "Tq62DSNKkop7+TYey+ls5/TZ099VF2DFNByp1jydQ9Fqk2nDk+atZCrXbyAI3BZ+\n"
    "cIzfA9Ftqg3FYF424+5HaD8MIvdOPYOXuFi82Wcn9cznUEBjf6b1vzIm6T3LVzEy\n"
    "ybTQaeENpYb1xKRrTDMxvs2vgluhm2ZJI7Jk6SmgBB/6y8Zf4atmOkQ+1Hg2TXSj\n"
    "gQragzmlAgMBAAECggEALVrKryP+mL9Z4yFBBRq8BCH0mzzsLgrGU8cpSJrNao9/\n"
    "h6yAH72Lxp0MvWLEx1vHeBXScbUIhmkIzXusQT91p0szcNby8VipxFJXxKHU4O69\n"
    "hncKAgkkBK3jSqfn8yJKAxbfeNBZT/b06s9MCvqCew92FYFMZUcpo7L3NUZR/KT+\n"
    "xy00Dd0fH87OZdScKKOriCSc99IvkrmCCrxBB9y4S/NnXST1bqPoJPZeWZjcBbCz\n"
    "VaJUt/z2NSX7P2mWgqsmqy0V/nFmYCfgg+v83OBMGcugg+KWF8eOLxbAJMSgHPYI\n"
    "K+AIjv2hQgS17baZ4km7VkRG/FoeMWdVDRpwBD5soQKBgQDuhJKxjDhq/3BndAa9\n"
    "Qo18LiOViTVXmogc8hmS8sKyfWJtXQ0EfNTq+27j6zqOqx8VGMD4yudpvhk083fc\n"
    "x4IC6o2FgdVJHmFxnjhrEEJCJfFbFTV3d1+w50vXbZE9bl2/3iE3uRXK4sGeo/c+\n"
    "Fpe7QK2SdCA8/OfbRnvG54ZlxQKBgQDrlnZIj80SIlkCFI0BP4doWahu2Hnj3PNt\n"
    "4I2/PINvUTpMNfBiTYa3K7twFbeXIIvHja9gA8Jk5Qj87tMS29BlXUln4RGTxSXx\n"
    "MIZdiDHF2ybRC6oSiBb88MM8SfigE8CstdIDl19F2UWpzQyGkW0bhx5HKwGdtiWt\n"
    "2PyuNnCiYQKBgQCZQqh722zZG9+fKge2jtAY7hDBYlPbQZmad9oE+WYviK+5NCRM\n"
    "MOYjQ4KCg0CyMbScOrasZryBzrulsZfgTnX058Ad/EoPXK0ic5cu/FiG5piKfTtI\n"
    "03SyWDz8ZRQBVCx7QAE6K/ybzE67YAJba+r9UFb3lxSr+5oD5Otd6KEMPQKBgQCa\n"
    "Azc4oKnT3RiLP5we4MVI9rQiIussh9msT0zbZFgYgeW2xNxtp3kWbkSPNeNbrS80\n"
    "OfAYuNxw0TpbAFaE0acfXSkL/Btdm1j+oFZ29v5y9p4ds55vlwBQQ6We2EzteXxQ\n"
    "bFzrdB4Yr73XD/HMV24YvPCSXg8kZ1uil2Q5D+X6gQKBgB7jiCXPl60mIXYuGQmo\n"
    "ChJ1FvqpJH54zaNwuZFNgCNR1BUhvG3u6Ufcej8EUoAbk4/+yJoemAFvSpAM8hNP\n"
    "N54KQmT52ZOkudMtIKB4Ts9XU21Zg2WLWAed4A8dPH7hKs23oI2gwA4Yr/o05Ge6\n"
    "aMfnJPJezhfdbsWXbhB5woVp\n"
    "-----END PRIVATE KEY-----\n";

/* accept one TLS connection, echo one record back, then send
 * close_notify — the client sees its chunk, then EOF */
static void tls_serve(int listener)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (ctx) {
        BIO *cb = BIO_new_mem_buf(TLS_TEST_CERT, -1);
        BIO *kb = BIO_new_mem_buf(TLS_TEST_KEY, -1);
        X509 *x = PEM_read_bio_X509(cb, NULL, 0, NULL);
        EVP_PKEY *k = PEM_read_bio_PrivateKey(kb, NULL, NULL, NULL);
        SSL_CTX_use_certificate(ctx, x);
        SSL_CTX_use_PrivateKey(ctx, k);
        int c = accept(listener, NULL, NULL);
        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, c);
        SSL_accept(ssl); /* rejects on its own when the client fails */
        char buf[256];
        int n = SSL_read(ssl, buf, sizeof buf);
        if (n > 0) {
            SSL_write(ssl, buf, n);
        }
        /* close_notify: the client's next read is the EOF signal */
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(c);
        X509_free(x);
        EVP_PKEY_free(k);
        BIO_free(cb);
        BIO_free(kb);
        SSL_CTX_free(ctx);
    }
    close(listener);
}

static void *tls_main(void *arg)
{
    tls_serve((int)(intptr_t)arg);
    return NULL;
}

/* opts.ca points at a file: stage the baked-in certificate once */
static const char *tls_cert_file(void)
{
    static const char *path = "/tmp/luna-loop-tls-cert.pem";
    FILE *f = fopen(path, "w");
    assert_non_null(f);
    fputs(TLS_TEST_CERT, f);
    fclose(f);
    return path;
}

/* listenTls needs the key on disk too */
static const char *tls_key_file(void)
{
    static const char *path = "/tmp/luna-loop-tls-key.pem";
    FILE *f = fopen(path, "w");
    assert_non_null(f);
    fputs(TLS_TEST_KEY, f);
    fclose(f);
    return path;
}

/* connect + handshake + one write/read round trip, then the server's
 * close_notify arrives as the EOF signal; opts is the Lua table text */
static void tls_roundtrip_case(const char *opts, int port)
{
    char code[896];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "log = {}\n"
        "net.connectTls('127.0.0.1', %d, %s, function(e, sock)\n"
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
        "return table.concat(log, ',')", port, opts);
    assert_string_equal(eval_string(code), "ping,nil/nil");
}

static void test_tls_insecure_echo_then_eof(void **state)
{
    (void)state;
    int listener = tcp_listen_loopback();
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, tls_main,
                                    (void *)(intptr_t)listener), 0);
    tls_roundtrip_case("{insecure = true}", tcp_port_of(listener));
    pthread_join(th, NULL);
}

static void test_tls_custom_ca_accepts_self_signed(void **state)
{
    (void)state;
    int listener = tcp_listen_loopback();
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, tls_main,
                                    (void *)(intptr_t)listener), 0);
    char opts[80];
    snprintf(opts, sizeof opts, "{ca = '%s'}", tls_cert_file());
    tls_roundtrip_case(opts, tcp_port_of(listener));
    pthread_join(th, NULL);
    unlink("/tmp/luna-loop-tls-cert.pem");
}

static void test_tls_default_verify_rejects_self_signed(void **state)
{
    (void)state;
    int listener = tcp_listen_loopback();
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, tls_main,
                                    (void *)(intptr_t)listener), 0);
    char code[384];
    snprintf(code, sizeof code,
        "out = 'none'\n"
        "loop.net.connectTls('127.0.0.1', %d, function(e, sock)\n"
        "  out = tostring(e ~= nil) .. ',' .. tostring(sock)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out", tcp_port_of(listener));
    /* no opts: the system store has no trust anchor for this cert, so
     * the handshake fails into cb(err) with no sock */
    assert_string_equal(eval_string(code), "true,nil");
    pthread_join(th, NULL);
}

static void test_tls_connect_refused_yields_error(void **state)
{
    (void)state;
    /* grab a free port, then release it: nothing is listening there */
    int fd = tcp_listen_loopback();
    int port = tcp_port_of(fd);
    close(fd);
    char code[320];
    snprintf(code, sizeof code,
        "out = 'none'\n"
        "loop.net.connectTls('127.0.0.1', %d, function(e, sock)\n"
        "  out = tostring(e ~= nil) .. ',' .. tostring(sock)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out", port);
    /* the failure surfaces in slot one, the loop drains to a natural
     * return, and the half-built sock cleaned itself up */
    assert_string_equal(eval_string(code), "true,nil");
}

/* -- net.listenTls: the server side, clients over connectTls ----------- */

/* one case runs listener and client in the same VM: listenTls echoes
 * one record back then closes (close_notify), connectTls reads it and
 * then the EOF signal, trusting the server via opts.ca */
static void tls_listen_case(void)
{
    const char *cert = tls_cert_file();
    const char *key = tls_key_file();
    char code[1152];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "log = {}\n"
        "srv = net.listenTls('127.0.0.1', 0,"
        " {cert = '%s', key = '%s'}, function(e, c)\n"
        "  if e then log[1] = 'conn:' .. e return end\n"
        "  c:read(function(e2, chunk)\n"
        "    if chunk then\n"
        "      c:write('pong:' .. chunk, function() c:close() end)\n"
        "    end\n"
        "  end)\n"
        "end)\n"
        "net.connectTls('127.0.0.1', srv:port(), {ca = '%s'}, function(e, s)\n"
        "  if e then log[1] = 'connect:' .. e return end\n"
        "  s:write('ping', function(e3)\n"
        "    if e3 then log[1] = 'write:' .. e3 return end\n"
        "    s:read(function(e4, chunk)\n"
        "      log[1] = tostring(chunk)\n"
        "      s:read(function(e5, eof)\n"
        "        log[2] = tostring(e5) .. '/' .. tostring(eof)\n"
        "        s:close()\n"
        "        srv:close()\n"
        "      end)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return table.concat(log, ',')", cert, key, cert);
    assert_string_equal(eval_string(code), "pong:ping,nil/nil");
}

static void test_tls_listen_with_custom_ca_roundtrips(void **state)
{
    (void)state;
    /* the client trusts the server's self-signed cert via opts.ca and
     * the hostname check passes on the baked-in IP SAN */
    tls_listen_case();
}

static void test_tls_listen_default_verify_rejected(void **state)
{
    (void)state;
    const char *cert = tls_cert_file();
    const char *key = tls_key_file();
    char code[896];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "out = 'none'\n"
        "srv = net.listenTls('127.0.0.1', 0,"
        " {cert = '%s', key = '%s'}, function(e, c) end)\n"
        "net.connectTls('127.0.0.1', srv:port(), function(e, s)\n"
        "  out = tostring(e ~= nil) .. ',' .. tostring(s)\n"
        "  if s then s:close() end\n"
        "  srv:close()\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out", cert, key);
    /* no opts: the self-signed server cert has no anchor in the system
     * store, the handshake fails into cb(err, nil), the server never
     * sees the peer, and the loop still drains */
    assert_string_equal(eval_string(code), "true,nil");
}

static void test_tls_listen_bad_cert_throws(void **state)
{
    (void)state;
    /* a missing certificate file is a setup error: it throws instead
     * of waiting for the loop to run */
    assert_string_equal(eval_string(
        "local ok, err = pcall(function()\n"
        "  return loop.net.listenTls('127.0.0.1', 0,"
        " {cert = '/nonexistent.pem', key = '/nonexistent.pem'},\n"
        "  function() end)\n"
        "end)\n"
        "return tostring(ok) .. ',' .. tostring(err ~= nil)"),
        "false,true");
}

static void test_tls_sock_addr_and_server_address(void **state)
{
    (void)state;
    /* both TLS ends and the listener report {address, port, family};
     * both callbacks must be live before either closes — the connect
     * callback can land first and an early close makes the server's
     * fd ENOTCONN, which is kernel truth, not a defect */
    const char *cert = tls_cert_file();
    const char *key = tls_key_file();
    char code[2048];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "out = 'none'\n"
        "local tconn, tcli, srv\n"
        "local function finish()\n"
        "  if not (tconn and tcli) then return end\n"
        "  local sp, cp = tconn:peer(), tcli:peer()\n"
        "  local a = srv:address()\n"
        "  out = table.concat({\n"
        "    tostring(a.address == '127.0.0.1'),\n"
        "    tostring(a.port == srv:port()),\n"
        "    a.family,\n"
        "    tostring(sp.address == '127.0.0.1'),\n"
        "    tostring(sp.port > 0),\n"
        "    tostring(cp.address == '127.0.0.1'),\n"
        "    tostring(cp.port == srv:port()),\n"
        "    tcli:sockname().family,\n"
        "  }, ',')\n"
        "  tconn:close()\n"
        "  tcli:close()\n"
        "  srv:close()\n"
        "end\n"
        "srv = net.listenTls('127.0.0.1', 0,\n"
        "  {cert = '%s', key = '%s'}, function(e, c)\n"
        "  if e then out = 'ERR:' .. e return end\n"
        "  tconn = c\n"
        "  c:read(function() end)\n"
        "  finish()\n"
        "end)\n"
        "net.connectTls('127.0.0.1', srv:port(), {ca = '%s'},\n"
        "  function(e, s)\n"
        "  if e then out = 'ERR:' .. e return end\n"
        "  tcli = s\n"
        "  finish()\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out", cert, key, cert);
    assert_string_equal(eval_string(code),
        "true,true,inet,true,true,true,true,inet");
}
#endif /* LUNA_LOOP_HAVE_OPENSSL */

/* -- loop.http: the pure-Lua client, served by net.listen in-process ---
 *
 * The same-VM trick keeps the fixture tiny: a net.listen (or
 * listenTls) callback plays the HTTP server for one request, the
 * client callback captures err/status/body, then both shut down. */

static void test_http_get_plain_roundtrip(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  c:read(function(e2, chunk)\n"
        "    if chunk then\n"
        "      c:write('HTTP/1.1 200 OK\\r\\nContent-Type: text/plain"
        "\\r\\nContent-Length: 5\\r\\n\\r\\nhello',\n"
        "        function() c:close() end)\n"
        "    end\n"
        "  end)\n"
        "end)\n"
        "http.get('http://127.0.0.1:' .. srv:port() .. '/x',\n"
        "  function(err, res)\n"
        "    out = tostring(err) .. ',' .. tostring(res and res.status)"
        " .. ',' .. tostring(res and res.headers['content-type'])\n"
        "        .. ',' .. tostring(res and res.body)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "nil,200,text/plain,hello");
}

static void test_http_get_chunked_body(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  c:read(function(e2, chunk)\n"
        "    if chunk then\n"
        "      c:write('HTTP/1.1 201 Created\\r\\nTransfer-Encoding: chunked"
        "\\r\\n\\r\\n5\\r\\nhello\\r\\n3\\r\\n wo\\r\\n2\\r\\nrl\\r\\n0\\r\\n\\r\\n',\n"
        "        function() c:close() end)\n"
        "    end\n"
        "  end)\n"
        "end)\n"
        "http.get('http://127.0.0.1:' .. srv:port() .. '/c',\n"
        "  function(err, res)\n"
        "    out = tostring(err) .. ',' .. tostring(res and res.status)\n"
        "        .. ',' .. tostring(res and res.body)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "nil,201,hello worl");
}

static void test_http_post_sends_method_headers_body(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  local buf = ''\n"
        "  c:read(function(e2, chunk)\n"
        "    if not chunk then return end\n"
        "    buf = buf .. chunk\n"
        "    local sp = buf:find('\\r\\n\\r\\n', 1, true)\n"
        "    if not sp then return end\n"
        "    local cl = tonumber(buf:match('Content%-Length: (%d+)')) or 0\n"
        "    if #buf < sp + 3 + cl then return end\n"
        "    local ok = buf:find('POST /p HTTP/1.1', 1, true)\n"
        "        and buf:find('X-Test: 1', 1, true)\n"
        "        and buf:find('data!', 1, true)\n"
        "    c:write(ok and 'HTTP/1.1 201 Created\\r\\nContent-Length: 0"
        "\\r\\n\\r\\n' or 'HTTP/1.1 400 Bad\\r\\nContent-Length: 0\\r\\n\\r\\n',\n"
        "      function() c:close() end)\n"
        "  end)\n"
        "end)\n"
        "http.request({url = 'http://127.0.0.1:' .. srv:port() .. '/p',\n"
        "  method = 'POST', body = 'data!',\n"
        "  headers = {['X-Test'] = '1'}}, function(err, res)\n"
        "    out = tostring(err) .. ',' .. tostring(res and res.status)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "nil,201");
}

static void test_http_follows_redirect_and_folds_post(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  local buf = ''\n"
        "  c:read(function(e2, chunk)\n"
        "    if not chunk then return end\n"
        "    buf = buf .. chunk\n"
        "    if not buf:find('\\r\\n\\r\\n', 1, true) then return end\n"
        "    local p = buf:match('^%S+ (%S+) HTTP')\n"
        "    local m = buf:match('^(%S+) ')\n"
        "    local body = nil\n"
        "    if p == '/r1' then\n"
        "      body = 'HTTP/1.1 302 Found\\r\\nLocation: /r2\\r\\n"
        "Content-Length: 0\\r\\n\\r\\n'\n"
        "    elseif p == '/r2' then\n"
        "      body = 'HTTP/1.1 200 OK\\r\\nContent-Length: 5\\r\\n\\r\\nfinal'\n"
        "    elseif p == '/a' then\n"
        "      body = 'HTTP/1.1 301 Moved\\r\\nLocation: /m\\r\\n"
        "Content-Length: 0\\r\\n\\r\\n'\n"
        "    elseif p == '/m' then\n"
        "      body = 'HTTP/1.1 200 OK\\r\\nContent-Length: 3\\r\\n\\r\\n' .. m\n"
        "    end\n"
        "    if body then c:write(body, function() c:close() end) end\n"
        "  end)\n"
        "end)\n"
        "local base = 'http://127.0.0.1:' .. srv:port()\n"
        "out = ''\n"
        "http.get(base .. '/r1', function(e1, r1)\n"
        "  out = out .. tostring(r1 and r1.status) .. '='\n"
        "      .. tostring(r1 and r1.body) .. ';'\n"
        "  http.request({url = base .. '/a', method = 'POST',\n"
        "    body = 'x'}, function(e2, r2)\n"
        "    out = out .. tostring(r2 and r2.body) .. ';'\n"
        "    http.get(base .. '/r1', {maxRedirects = 0}, function(e3, r3)\n"
        "      out = out .. tostring(r3 and r3.status) .. ','\n"
        "          .. tostring(r3 and r3.headers.location)\n"
        "      srv:close()\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"),
        "200=final;GET;302,/r2");
}

static void test_http_relative_location(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  local buf = ''\n"
        "  c:read(function(e2, chunk)\n"
        "    if not chunk then return end\n"
        "    buf = buf .. chunk\n"
        "    if not buf:find('\\r\\n\\r\\n', 1, true) then return end\n"
        "    local p = buf:match('^%S+ (%S+) HTTP')\n"
        "    if p == '/d/r1' then\n"
        "      c:write('HTTP/1.1 302 Found\\r\\nLocation: r2x\\r\\n"
        "Content-Length: 0\\r\\n\\r\\n', function() c:close() end)\n"
        "    elseif p == '/d/r2x' then\n"
        "      c:write('HTTP/1.1 200 OK\\r\\nContent-Length: 4\\r\\n\\r\\nDEEP',\n"
        "        function() c:close() end)\n"
        "    end\n"
        "  end)\n"
        "end)\n"
        "http.get('http://127.0.0.1:' .. srv:port() .. '/d/r1',\n"
        "  function(err, res)\n"
        "    out = tostring(err) .. ',' .. tostring(res and res.status)\n"
        "        .. ',' .. tostring(res and res.body)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "nil,200,DEEP");
}

static void test_http_redirect_budget_and_timeout(void **state)
{
    (void)state;
    /* a redirect loop exhausts the budget with an error; a silent
     * server fires the whole-request timeout — and the loop still
     * drains, which proves the timer was cleared */
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  local buf = ''\n"
        "  c:read(function(e2, chunk)\n"
        "    if not chunk then c:close() return end\n"
        "    buf = buf .. chunk\n"
        "    if not buf:find('\\r\\n\\r\\n', 1, true) then return end\n"
        "    local p = buf:match('^%S+ (%S+) HTTP')\n"
        "    if p == '/loop' then\n"
        "      c:write('HTTP/1.1 302 Loop\\r\\nLocation: /loop\\r\\n"
        "Content-Length: 0\\r\\n\\r\\n', function() c:close() end)\n"
        "    end\n"
        "    -- /silent: accepted, read, never answered\n"
        "  end)\n"
        "end)\n"
        "local base = 'http://127.0.0.1:' .. srv:port()\n"
        "out = ''\n"
        "http.get(base .. '/loop', {maxRedirects = 2}, function(e1)\n"
        "  out = out .. tostring(e1) .. ';'\n"
        "  http.get(base .. '/silent', {timeoutMs = 1000}, function(e2)\n"
        "    out = out .. tostring(e2)\n"
        "    srv:close()\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"),
        "loop.http: too many redirects;"
        "loop.http: timed out after 1000ms");
}

static void test_http_server_roundtrip(void **state)
{
    (void)state;
    /* the server face serves the client face in the same VM: one GET,
     * one POST whose body aggregates by content-length, custom status
     * and headers through res.send */
    assert_string_equal(eval_string(
        "local http = require('loop.http')\n"
        "out = 'none'\n"
        "local step\n"
        "local srv = http.listen('127.0.0.1', 0, function(req, res)\n"
        "  if req.path == '/echo' then\n"
        "    res.send(201, 'len=' .. #req.body .. ';' .. req.method,\n"
        "             {['X-Srv'] = 'yes'})\n"
        "  else\n"
        "    res.send('hello ' .. req.path)\n"
        "  end\n"
        "end)\n"
        "local base = 'http://127.0.0.1:' .. srv:port()\n"
        "out = ''\n"
        "http.get(base .. '/x', function(e1, r1)\n"
        "  out = tostring(r1 and r1.status) .. '='\n"
        "      .. tostring(r1 and r1.body) .. ';'\n"
        "  http.request({url = base .. '/echo', method = 'POST',\n"
        "    body = 'abcde'}, function(e2, r2)\n"
        "    out = out .. tostring(r2 and r2.status) .. '='\n"
        "        .. tostring(r2 and r2.body) .. ','\n"
        "        .. tostring(r2 and r2.headers['x-srv'])\n"
        "    srv:close()\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"),
        "200=hello /x;201=len=5;POST,yes");
}

static void test_http_server_error_paths(void **state)
{
    (void)state;
    /* a raising handler turns into a 500 when nothing was sent; a raw
     * non-HTTP request gets a 400 and a close */
    assert_string_equal(eval_string(
        "local http = require('loop.http')\n"
        "local net = loop.net\n"
        "out = 'none'\n"
        "local srv = http.listen('127.0.0.1', 0, function(req, res)\n"
        "  if req.path == '/boom' then error('blew up') end\n"
        "  res.send('nope')\n"
        "end)\n"
        "local base = 'http://127.0.0.1:' .. srv:port()\n"
        "out = ''\n"
        "http.get(base .. '/boom', function(e1, r1)\n"
        "  out = tostring(e1 == nil) .. ',' .. tostring(r1 and r1.status)\n"
        "      .. ';'\n"
        "  net.connect('127.0.0.1', srv:port(), function(e2, s)\n"
        "    s:read(function(e3, chunk)\n"
        "      if chunk then\n"
        "        out = out .. tostring(\n"
        "            (chunk:find('400', 1, true)) ~= nil)\n"
        "        s:close()\n"
        "        srv:close()\n"
        "      end\n"
        "    end)\n"
        "    s:write('NOT-HTTP at all\\r\\n\\r\\n', function() end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"),
        "true,500;true");
}

static void test_http_stream_content_length(void **state)
{
    (void)state;
    /* streaming by content-length: every body byte flows through
     * onData exactly once, res.body comes back empty, onHead sees the
     * final head (status + content-length) */
    assert_string_equal(eval_string(
        "local http = require('loop.http')\n"
        "local big = ('x'):rep(50000)\n"
        "local srv = http.listen('127.0.0.1', 0, function(req, res)\n"
        "  res.send(big)\n"
        "end)\n"
        "local base = 'http://127.0.0.1:' .. srv:port()\n"
        "out = ''\n"
        "local n, total\n"
        "http.request({ url = base .. '/big',\n"
        "  onHead = function(h)\n"
        "    out = out .. tostring(h.status) .. ',' ..\n"
        "        tostring(h.headers['content-length']) .. ';'\n"
        "  end,\n"
        "  onData = function(c)\n"
        "    n = (n or 0) + 1\n"
        "    total = (total or 0) + #c\n"
        "  end },\n"
        "  function(err, res)\n"
        "    out = out .. tostring(err) .. ',' .. tostring(total) ..\n"
        "        ',' .. tostring(n >= 1) .. ',' ..\n"
        "        tostring(res.body == '')\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "200,50000;nil,50000,true,true");
}

static void test_http_stream_eof_mode(void **state)
{
    (void)state;
    /* streaming with no framing at all: a raw origin server that reads
     * the request first, answers with an unframed body and closes —
     * the client streams the body through onData and ends by EOF */
    assert_string_equal(eval_string(
        "local http = require('loop.http')\n"
        "local net = loop.net\n"
        "local srv = net.listen('127.0.0.1', 0, function(e, s)\n"
        "  if e then return end\n"
        "  local answered = false\n"
        "  s:read(function(err, chunk)\n"
        "    if err or not chunk then s:close() return end\n"
        "    if answered then return end\n"
        "    answered = true\n"
        "    s:write('HTTP/1.1 200 OK\\r\\nX-Raw: yes\\r\\n\\r\\n'\n"
        "        .. 'streamed-by-eof', function() s:close() end)\n"
        "  end)\n"
        "end)\n"
        "out = ''\n"
        "local n, total\n"
        "http.request({ url = 'http://127.0.0.1:' .. srv:port() .. '/',\n"
        "  onHead = function(h)\n"
        "    out = out .. tostring(h.headers['x-raw']) .. ';'\n"
        "  end,\n"
        "  onData = function(c)\n"
        "    n = (n or 0) + 1\n"
        "    total = (total or 0) + #c\n"
        "  end },\n"
        "  function(err, res)\n"
        "    out = out .. tostring(err) .. ',' .. tostring(total) ..\n"
        "        ',' .. tostring(n >= 1) .. ',' ..\n"
        "        tostring(res and res.body == '') .. ',' ..\n"
        "        tostring(res and res.headers['x-raw'])\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "yes;nil,15,true,true,yes");
}

static void test_http_stream_onhead_once_after_redirect(void **state)
{
    (void)state;
    /* onHead is the final head only: a redirect hop stays internal and
     * never reaches it; streaming onData stays empty-bodied too */
    assert_string_equal(eval_string(
        "local http = require('loop.http')\n"
        "local srv = http.listen('127.0.0.1', 0, function(req, res)\n"
        "  if req.path == '/hop' then\n"
        "    res.send(302, '', { Location = '/dst' })\n"
        "  else\n"
        "    res.send('arrived')\n"
        "  end\n"
        "end)\n"
        "local base = 'http://127.0.0.1:' .. srv:port()\n"
        "out = ''\n"
        "local heads = 0\n"
        "http.request({ url = base .. '/hop', maxRedirects = 5,\n"
        "  onHead = function() heads = heads + 1 end,\n"
        "  onData = function() end },\n"
        "  function(err, res)\n"
        "    out = tostring(err) .. ',heads=' .. heads .. ',' ..\n"
        "        tostring(res and res.status) .. ',' ..\n"
        "        tostring(res and res.body == '')\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "nil,heads=1,200,true");
}

static void test_http_bad_url_throws(void **state)
{
    (void)state;
    /* unparseable urls and missing callbacks throw before the loop
     * ever runs: setup errors are the caller's to see */
    assert_string_equal(eval_string(
        "local http = require('loop.http')\n"
        "local a = not pcall(function() http.get('notaurl', function() end) end)\n"
        "local b = not pcall(function() http.get('http://127.0.0.1:1/x') end)\n"
        "local c = not pcall(function() http.get(123, function() end) end)\n"
        "local d = not pcall(function()"
        " http.get('http:///nohost', function() end) end)\n"
        "return tostring(a) .. ',' .. tostring(b) .. ','\n"
        "    .. tostring(c) .. ',' .. tostring(d)"),
        "true,true,true,true");
}

/* -- http corners: framing edges, redirect shapes, error surfaces ------ */

static void test_http_bare_url_defaults_the_path(void **state)
{
    (void)state;
    /* a url with no path asks for "/", and a non-default port joins
     * the Host header; the whole-request timer is set and must be
     * cleared by the clean delivery, or the loop would not drain */
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  c:read(function(e2, chunk)\n"
        "    if chunk then\n"
        "      local line = chunk:match('^(%S+ %S+ %S+)')\n"
        "      local host = chunk:match('Host: (%S+)')\n"
        "      local body = line .. '|' .. host\n"
        "      c:write('HTTP/1.1 200 OK\\r\\nContent-Length: ' .. #body\n"
        "        .. '\\r\\n\\r\\n' .. body,\n"
        "        function() c:close() end)\n"
        "    end\n"
        "  end)\n"
        "end)\n"
        "local port = srv:port()\n"
        "http.get('http://127.0.0.1:' .. port, { timeoutMs = 5000 },\n"
        "  function(err, res)\n"
        "    out = tostring(err) .. ',' .. tostring(res and res.body)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out == ('nil,GET / HTTP/1.1|' .. '127.0.0.1:' .. port)"),
        "true");
}

static void test_http_head_and_chunked_arrive_in_pieces(void **state)
{
    (void)state;
    /* head, chunk-size line and chunk body each land in their own
     * read: the client holds the pieces until each frame is whole */
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  c:read(function() end)\n"
        "  c:write('HTTP/1.1 200 OK\\r\\nTransfer-Encoding: chunked')\n"
        "  loop.setTimeout(function() c:write('\\r\\n\\r\\n5') end, 20)\n"
        "  loop.setTimeout(function() c:write('\\r\\nhe') end, 40)\n"
        "  loop.setTimeout(function()\n"
        "    c:write('llo\\r\\n0\\r\\n\\r\\n', function() c:close() end)\n"
        "  end, 60)\n"
        "end)\n"
        "http.get('http://127.0.0.1:' .. srv:port() .. '/c',\n"
        "  function(err, res)\n"
        "    out = tostring(err) .. ',' .. tostring(res and res.body)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "nil,hello");
}

static void test_http_chunk_body_split_across_reads(void **state)
{
    (void)state;
    /* the chunk-size line arrives with only part of its body: the
     * decoder must hold the fragment and wait for the rest */
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  c:read(function() end)\n"
        "  c:write('HTTP/1.1 200 OK\\r\\nTransfer-Encoding: chunked"
        "\\r\\n\\r\\n5\\r\\nhe')\n"
        "  loop.setTimeout(function()\n"
        "    c:write('llo\\r\\n0\\r\\n\\r\\n', function() c:close() end)\n"
        "  end, 60)\n"
        "end)\n"
        "http.get('http://127.0.0.1:' .. srv:port() .. '/s',\n"
        "  function(err, res)\n"
        "    out = tostring(err) .. ',' .. tostring(res and res.body)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "nil,hello");
}

static void test_http_body_split_across_reads(void **state)
{
    (void)state;
    /* a content-length body split behind its head: the second chunk
     * flows through the post-head body path */
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  c:read(function() end)\n"
        "  c:write('HTTP/1.1 200 OK\\r\\nContent-Length: 3\\r\\n\\r\\na')\n"
        "  loop.setTimeout(function()\n"
        "    c:write('bc', function() c:close() end) end, 20)\n"
        "end)\n"
        "http.get('http://127.0.0.1:' .. srv:port() .. '/s',\n"
        "  function(err, res)\n"
        "    out = tostring(err) .. ',' .. tostring(res and res.body)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "nil,abc");
}

static void test_http_bad_chunk_size_is_an_error(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  c:read(function() end)\n"
        "  c:write('HTTP/1.1 200 OK\\r\\nTransfer-Encoding: chunked"
        "\\r\\n\\r\\nzz\\r\\n', function() c:close() end)\n"
        "end)\n"
        "http.get('http://127.0.0.1:' .. srv:port() .. '/z',\n"
        "  function(err)\n"
        "    out = tostring(err)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "loop.http: bad chunk size");
}

static void test_http_malformed_head_is_an_error(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  c:read(function() end)\n"
        "  c:write('NOT HTTP AT ALL\\r\\n\\r\\n', function() c:close() end)\n"
        "end)\n"
        "http.get('http://127.0.0.1:' .. srv:port() .. '/m',\n"
        "  function(err)\n"
        "    out = tostring(err)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "loop.http: malformed response head");
}

static void test_http_duplicate_response_headers_join(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  c:read(function() end)\n"
        "  c:write('HTTP/1.1 200 OK\\r\\nX-Dup: a\\r\\nX-Dup: b"
        "\\r\\nContent-Length: 0\\r\\n\\r\\n', function() c:close() end)\n"
        "end)\n"
        "http.get('http://127.0.0.1:' .. srv:port() .. '/d',\n"
        "  function(err, res)\n"
        "    out = tostring(err) .. ',' .. tostring(res and res.headers['x-dup'])\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "nil,a, b");
}

static void test_http_redirect_location_shapes(void **state)
{
    (void)state;
    /* absolute, //host/path and ../-collapsing relative Locations all
     * resolve; one that cannot parse ends the chain with its error */
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  c:read(function(e2, chunk)\n"
        "    if not chunk then c:close() return end\n"
        "    local p = chunk:match('^%S+ (%S+) HTTP')\n"
        "    local port = srv:port()\n"
        "    local body = false\n"
        "    if p == '/abs' then\n"
        "      body = 'HTTP/1.1 302 Found\\r\\nLocation: http://127.0.0.1:'\n"
        "          .. port .. '/dst\\r\\nContent-Length: 0\\r\\n\\r\\n'\n"
        "    elseif p == '/slashes' then\n"
        "      body = 'HTTP/1.1 302 Found\\r\\nLocation: //127.0.0.1:'\n"
        "          .. port .. '/dst\\r\\nContent-Length: 0\\r\\n\\r\\n'\n"
        "    elseif p == '/a/b/up' then\n"
        "      body = 'HTTP/1.1 302 Found\\r\\nLocation: ../dst"
        "\\r\\nContent-Length: 0\\r\\n\\r\\n'\n"
        "    elseif p == '/a/dst' then\n"
        "      body = 'HTTP/1.1 200 OK\\r\\nContent-Length: 4\\r\\n\\r\\nDEEP'\n"
        "    elseif p == '/dst' then\n"
        "      body = 'HTTP/1.1 200 OK\\r\\nContent-Length: 4\\r\\n\\r\\nHERE'\n"
        "    elseif p == '/bad' then\n"
        "      body = 'HTTP/1.1 302 Found\\r\\nLocation: http://"
        "\\r\\nContent-Length: 0\\r\\n\\r\\n'\n"
        "    end\n"
        "    if body then c:write(body, function() c:close() end) end\n"
        "  end)\n"
        "end)\n"
        "local base = 'http://127.0.0.1:' .. srv:port()\n"
        "out = ''\n"
        "http.get(base .. '/abs', function(e1, r1)\n"
        "  out = out .. tostring(r1 and r1.body) .. ','\n"
        "  http.get(base .. '/slashes', function(e2, r2)\n"
        "    out = out .. tostring(r2 and r2.body) .. ','\n"
        "    http.get(base .. '/a/b/up', function(e3, r3)\n"
        "      out = out .. tostring(r3 and r3.body) .. ','\n"
        "      http.get(base .. '/bad', function(e4)\n"
        "        out = out .. tostring(e4 == nil and 'ERR?' or 'ERR')\n"
        "        srv:close()\n"
        "      end)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"),
        "HERE,HERE,DEEP,ERR");
}

static void test_http_204_head_is_the_whole_response(void **state)
{
    (void)state;
    /* bodiless by definition: the head lands and the request is done */
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  c:read(function() end)\n"
        "  c:write('HTTP/1.1 204 No Content\\r\\nX-Keep: 1\\r\\n\\r\\n',\n"
        "    function() c:close() end)\n"
        "end)\n"
        "http.get('http://127.0.0.1:' .. srv:port() .. '/n',\n"
        "  function(err, res)\n"
        "    out = tostring(err) .. ',' .. tostring(res and res.status) .. ','\n"
        "        .. tostring(res and res.body) .. ','\n"
        "        .. tostring(res and res.headers['x-keep'])\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "nil,204,,1");
}

static void test_http_onhead_may_raise(void **state)
{
    (void)state;
    /* an observer that raises turns into the request's error */
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  c:read(function() end)\n"
        "  c:write('HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\n\\r\\nok',\n"
        "    function() c:close() end)\n"
        "end)\n"
        "http.request({ url = 'http://127.0.0.1:' .. srv:port() .. '/h',\n"
        "  onHead = function() error('oh') end },\n"
        "  function(err)\n"
        "    out = tostring(tostring(err):find('oh', 1, true) ~= nil)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "true");
}

static void test_http_stream_chunked_hands_one_piece(void **state)
{
    (void)state;
    /* chunked + onData: the decoded body flows through the observer
     * in one piece and res.body comes back empty */
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  c:read(function() end)\n"
        "  c:write('HTTP/1.1 200 OK\\r\\nTransfer-Encoding: chunked"
        "\\r\\n\\r\\n3\\r\\nabc\\r\\n0\\r\\n\\r\\n',\n"
        "    function() c:close() end)\n"
        "end)\n"
        "http.request({ url = 'http://127.0.0.1:' .. srv:port() .. '/k',\n"
        "  onData = function(c) got = c end },\n"
        "  function(err, res)\n"
        "    out = tostring(err) .. ',' .. tostring(got) .. ','\n"
        "        .. tostring(res and res.body == '')\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "nil,abc,true");
}

static void test_http_stream_ondata_may_raise_chunked(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  c:read(function() end)\n"
        "  c:write('HTTP/1.1 200 OK\\r\\nTransfer-Encoding: chunked"
        "\\r\\n\\r\\n3\\r\\nabc\\r\\n0\\r\\n\\r\\n',\n"
        "    function() c:close() end)\n"
        "end)\n"
        "http.request({ url = 'http://127.0.0.1:' .. srv:port() .. '/k',\n"
        "  onData = function() error('cbboom') end },\n"
        "  function(err)\n"
        "    out = tostring(tostring(err):find('cbboom', 1, true) ~= nil)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "true");
}

static void test_http_stream_ondata_may_raise_content_length(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  c:read(function() end)\n"
        "  c:write('HTTP/1.1 200 OK\\r\\nContent-Length: 5\\r\\n\\r\\nhello',\n"
        "    function() c:close() end)\n"
        "end)\n"
        "http.request({ url = 'http://127.0.0.1:' .. srv:port() .. '/l',\n"
        "  onData = function() error('clboom') end },\n"
        "  function(err)\n"
        "    out = tostring(tostring(err):find('clboom', 1, true) ~= nil)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "true");
}

static void test_http_unframed_eof_body(void **state)
{
    (void)state;
    /* neither content-length nor chunked: the peer's close is the end
     * marker and everything after the head is the body */
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  local buf, answered = '', false\n"
        "  c:read(function(e2, chunk)\n"
        "    if not chunk then c:close() return end\n"
        "    buf = buf .. chunk\n"
        "    if answered or not buf:find('\\r\\n\\r\\n', 1, true) then return end\n"
        "    answered = true\n"
        "    c:write('HTTP/1.1 200 OK\\r\\nX-Raw: 1\\r\\n\\r\\nplain-eof',\n"
        "      function() c:close() end)\n"
        "  end)\n"
        "end)\n"
        "http.get('http://127.0.0.1:' .. srv:port() .. '/u',\n"
        "  function(err, res)\n"
        "    out = tostring(err) .. ',' .. tostring(res and res.body) .. ','\n"
        "        .. tostring(res and res.headers['x-raw'])\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "nil,plain-eof,1");
}

static void test_http_eof_before_head(void **state)
{
    (void)state;
    /* the peer takes the request, then hangs up without answering */
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  local buf = ''\n"
        "  c:read(function(e2, chunk)\n"
        "    if not chunk then c:close() return end\n"
        "    buf = buf .. chunk\n"
        "    if buf:find('\\r\\n\\r\\n', 1, true) then c:close() end\n"
        "  end)\n"
        "end)\n"
        "http.get('http://127.0.0.1:' .. srv:port() .. '/q',\n"
        "  function(err)\n"
        "    out = tostring(err)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "loop.http: connection closed before a response");
}

static void test_http_eof_mid_body(void **state)
{
    (void)state;
    /* a declared length the body never reaches is an error, not a
     * short read */
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  local buf, answered = '', false\n"
        "  c:read(function(e2, chunk)\n"
        "    if not chunk then c:close() return end\n"
        "    buf = buf .. chunk\n"
        "    if answered or not buf:find('\\r\\n\\r\\n', 1, true) then return end\n"
        "    answered = true\n"
        "    c:write('HTTP/1.1 200 OK\\r\\nContent-Length: 100\\r\\n\\r\\nabc',\n"
        "      function() c:close() end)\n"
        "  end)\n"
        "end)\n"
        "http.get('http://127.0.0.1:' .. srv:port() .. '/t',\n"
        "  function(err)\n"
        "    out = tostring(err)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "loop.http: connection closed mid-body");
}

static void test_http_reset_mid_response_surfaces_an_error(void **state)
{
    (void)state;
    /* the origin closes with the request still unread, so the kernel
     * answers with RST: whichever callback it surfaces in — the read
     * or the in-flight write — the request ends with that error */
    assert_string_equal(eval_string(
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then return end\n"
        "  loop.setTimeout(function() c:close() end, 100)\n"
        "end)\n"
        "http.get('http://127.0.0.1:' .. srv:port() .. '/r',\n"
        "  function(err)\n"
        "    out = tostring(type(err) == 'string')\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "true");
}

static void test_http_connect_refused_is_the_error(void **state)
{
    (void)state;
    /* port 1 on loopback: nothing listens there, the connect fails and
     * the failure is the request's callback error */
    assert_string_equal(eval_string(
        "local http = require('loop.http')\n"
        "out = 'none'\n"
        "http.get('http://127.0.0.1:1/x', function(err)\n"
        "  out = tostring(type(err) == 'string' and #err > 0)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"),
        "true");
}

/* -- the server face's corners ----------------------------------------- */

static void test_http_server_res_send_is_idempotent(void **state)
{
    (void)state;
    /* a second send is dropped: the first response is the wire's truth */
    assert_string_equal(eval_string(
        "local http = require('loop.http')\n"
        "out = 'none'\n"
        "local srv = http.listen('127.0.0.1', 0, function(req, res)\n"
        "  res.send('first')\n"
        "  res.send('second')\n"
        "end)\n"
        "http.get('http://127.0.0.1:' .. srv:port() .. '/i',\n"
        "  function(err, res)\n"
        "    out = tostring(err) .. ',' .. tostring(res and res.body)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"),
        "nil,first");
}

static void test_http_server_client_vanishes_then_recovers(void **state)
{
    (void)state;
    /* a client that hangs up before its request lands is closed out
     * quietly; the server keeps serving the next one */
    assert_string_equal(eval_string(
        "local http = require('loop.http')\n"
        "local net = loop.net\n"
        "out = 'none'\n"
        "local srv = http.listen('127.0.0.1', 0, function(req, res)\n"
        "  res.send('still here')\n"
        "end)\n"
        "local base = 'http://127.0.0.1:' .. srv:port()\n"
        "net.connect('127.0.0.1', srv:port(), function(e, s)\n"
        "  if not e then s:close() end\n"
        "end)\n"
        "http.get(base .. '/after', function(err, res)\n"
        "  out = tostring(err) .. ',' .. tostring(res and res.body)\n"
        "  srv:close()\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"),
        "nil,still here");
}

static void test_http_server_head_runaway_gets_400(void **state)
{
    (void)state;
    /* a head that never ends grows past 64KiB: one 400 and a close */
    assert_string_equal(eval_string(
        "local http = require('loop.http')\n"
        "local net = loop.net\n"
        "out = 'none'\n"
        "local srv = http.listen('127.0.0.1', 0, function(req, res)\n"
        "  res.send('unreachable')\n"
        "end)\n"
        "net.connect('127.0.0.1', srv:port(), function(e, s)\n"
        "  local got = ''\n"
        "  s:read(function(e2, chunk)\n"
        "    if chunk then\n"
        "      got = got .. chunk\n"
        "    else\n"
        "      out = tostring(got:find('400', 1, true) ~= nil)\n"
        "      s:close()\n"
        "      srv:close()\n"
        "    end\n"
        "  end)\n"
        "  s:write(('A'):rep(70000))\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"),
        "true");
}

static void test_http_server_dup_headers_and_slow_body(void **state)
{
    (void)state;
    /* repeated request headers join with a comma, and a body split
     * behind its content-length holds the handler until it is whole */
    assert_string_equal(eval_string(
        "local http = require('loop.http')\n"
        "local net = loop.net\n"
        "out = 'none'\n"
        "local srv = http.listen('127.0.0.1', 0, function(req, res)\n"
        "  res.send('OK ' .. tostring(req.headers['x-dup']) .. ' '\n"
        "      .. tostring(req.body))\n"
        "end)\n"
        "net.connect('127.0.0.1', srv:port(), function(e, s)\n"
        "  local got = ''\n"
        "  s:read(function(e2, chunk)\n"
        "    if chunk then\n"
        "      got = got .. chunk\n"
        "    else\n"
        "      out = tostring(got:find('OK a, b abcdef', 1, true) ~= nil)\n"
        "      s:close()\n"
        "      srv:close()\n"
        "    end\n"
        "  end)\n"
        "  s:write('POST /slow HTTP/1.1\\r\\nHost: x\\r\\nX-Dup: a\\r\\n'\n"
        "      .. 'X-Dup: b\\r\\nContent-Length: 6\\r\\n\\r\\nabc')\n"
        "  loop.setTimeout(function() s:write('def') end, 30)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"),
        "true");
}

static void test_http_listen_requires_a_handler(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local http = require('loop.http')\n"
        "local ok, err = pcall(function() return http.listen('127.0.0.1', 0) end)\n"
        "return tostring(not ok) .. '|' ..\n"
        "    tostring(tostring(err):find('handler required') ~= nil)"),
        "true|true");
}

#ifdef LUNA_LOOP_HAVE_OPENSSL
static void test_http_get_over_tls(void **state)
{
    (void)state;
    const char *cert = tls_cert_file();
    const char *key = tls_key_file();
    char code[1024];
    snprintf(code, sizeof code,
        "local net, http = loop.net, require('loop.http')\n"
        "out = 'none'\n"
        "srv = net.listenTls('127.0.0.1', 0,"
        " {cert = '%s', key = '%s'}, function(e, c)\n"
        "  c:read(function(e2, chunk)\n"
        "    if chunk then\n"
        "      c:write('HTTP/1.1 200 OK\\r\\nContent-Length: 5\\r\\n\\r\\nhello',\n"
        "        function() c:close() end)\n"
        "    end\n"
        "  end)\n"
        "end)\n"
        "http.get('https://127.0.0.1:' .. srv:port() .. '/x',"
        " {ca = '%s'}, function(err, res)\n"
        "    out = tostring(err) .. ',' .. tostring(res and res.status)\n"
        "        .. ',' .. tostring(res and res.body)\n"
        "    srv:close()\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out", cert, key, cert);
    assert_string_equal(eval_string(code), "nil,200,hello");
}
#endif /* LUNA_LOOP_HAVE_OPENSSL */

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

/* -- sock:peer / sock:sockname / server:address ------------------------- */

static void test_net_sock_addr_tcp(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net = loop.net\n"
        "out = 'none'\n"
        "local cshared\n"
        "srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  local p = c:peer()   -- the server sees the client's address\n"
        "  gport = p.port > 0\n"
        "  gaddr = p.address\n"
        "  cshared = c\n"
        "end)\n"
        "local a = srv:address()\n"
        "net.connect('127.0.0.1', srv:port(), function(e, s)\n"
        "  local p, sn = s:peer(), s:sockname()\n"
        "  s:close()\n"
        "  local ok = pcall(function() return s:peer() end)\n"
        "  out = table.concat({\n"
        "    tostring(a.address == '127.0.0.1'),\n"
        "    tostring(a.port == srv:port()),\n"
        "    a.family,\n"
        "    tostring(p.address == '127.0.0.1'),\n"
        "    tostring(p.port == srv:port()),\n"
        "    sn.family,\n"
        "    tostring(sn.port > 0),\n"
        "    tostring(gaddr == '127.0.0.1'),\n"
        "    tostring(gport),\n"
        "    tostring(not ok),\n"
        "  }, ',')\n"
        "  cshared:close()\n"
        "  srv:close()\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"),
        "true,true,inet,true,true,inet,true,true,true,true");
}

static void test_net_sock_addr_pipe(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net = loop.net\n"
        "local path = '/tmp/luna-test-addrpipe.sock'\n"
        "os.remove(path)\n"
        "out = 'none'\n"
        "local cshared\n"
        "psrv = net.listenPipe(path, function(e, c)\n"
        "  cshared = c\n"
        "  gfamily = c:peer().family  -- anonymous peer is still unix\n"
        "end)\n"
        "local a = psrv:address()\n"
        "net.connectPipe(path, function(e, s)\n"
        "  local p, sn = s:peer(), s:sockname()\n"
        "  out = table.concat({\n"
        "    tostring(a.family == 'unix'),\n"
        "    tostring(a.address == path),\n"
        "    tostring(a.port == nil),\n"
        "    tostring(p.family == 'unix'),\n"
        "    tostring(p.address == path),\n"
        "    tostring(sn.family == 'unix'),\n"
        "    gfamily,\n"
        "  }, ',')\n"
        "  s:close()\n"
        "  cshared:close()\n"
        "  psrv:close()\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"),
        "true,true,true,true,true,true,unix");
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

/* -- failure injections: the libuv callback error/edge paths ---------- */

static void test_run_rejects_an_unknown_mode(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local ok, err = pcall(loop.run, 'bogus')\n"
        "return tostring(ok) .. '|' .. tostring(tostring(err):find('bad run mode') ~= nil)"),
        "false|true");
}

static void test_timer_handles_report_tostring(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local t = loop.setTimeout(function() end, 10)\n"
        "local i = loop.setImmediate(function() end)\n"
        "local a = tostring(t):find('loop%.timer') ~= nil\n"
        "local b = tostring(i):find('loop%.immediate') ~= nil\n"
        "loop.clearTimeout(t)\n"
        "assert(loop.run())  -- drain the closing handles\n"
        "return tostring(a) .. ',' .. tostring(b)"),
        "true,true");
}

static void test_throwing_timer_callback_does_not_stop_time(void **state)
{
    (void)state;
    /* the first callback throws, a later one still fires: the error is
     * reported to stderr and the run carries on */
    assert_string_equal(eval_string(
        "out = 'none'\n"
        "loop.setTimeout(function() error('timer boom') end, 1)\n"
        "loop.setTimeout(function() out = 'survived' end, 50)\n"
        "assert(loop.run())\n"
        "return out"),
        "survived");
}

static void test_fs_errors_report_through_the_callback(void **state)
{
    (void)state;
    /* one error injection per untested op: every cb's first argument
     * must carry the failure instead of throwing */
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "log = {}\n"
        "fs.writeFile('/no-such-dir/lx', 'y', function(e)\n"
        "  log[1] = e ~= nil\n"
        "  fs.appendFile('/no-such-dir/lx', 'y', function(e)\n"
        "    log[2] = e ~= nil\n"
        "    fs.readdir('/no-such-dir', function(e)\n"
        "      log[3] = e ~= nil\n"
        "      fs.mkdir('/tmp', function(e)          -- already there\n"
        "        log[4] = e ~= nil\n"
        "        fs.rmdir('/no-such-dir', function(e)\n"
        "          log[5] = e ~= nil\n"
        "          fs.unlink('/no-such-dir/lx', function(e)\n"
        "            log[6] = e ~= nil\n"
        "            fs.rename('/no-such-dir/a', '/no-such-dir/b', function(e)\n"
        "              log[7] = e ~= nil\n"
        "              fs.copyFile('/no-such-dir/a', '/tmp/luna-loop-cp', function(e)\n"
        "                log[8] = e ~= nil\n"
        "                fs.realpath('/no-such-dir/a', function(e)\n"
        "                  log[9] = e ~= nil\n"
        "                  fs.lstat('/no-such-dir/a', function(e)\n"
        "                    log[10] = e ~= nil\n"
        "                    fs.truncate('/no-such-dir/a', function(e)\n"
        "                      log[11] = e ~= nil\n"
        "                    end)\n"
        "                  end)\n"
        "                end)\n"
        "              end)\n"
        "            end)\n"
        "          end)\n"
        "        end)\n"
        "      end)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return #log == 11 and 'all-errored' or table.concat(log, ',')"),
        "all-errored");
}

static void test_fs_read_of_a_directory_fails_mid_chain(void **state)
{
    (void)state;
    /* the open succeeds, the read does not (EISDIR): exercises the
     * read-error leg of the readFile chain, not just open errors */
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "fs.readFile('/tmp', function(e, data)\n"
        "  out = tostring(e ~= nil) .. '|' .. tostring(data)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out"),
        "true|nil");
}

static void test_fs_stat_reports_other_for_a_fifo(void **state)
{
    (void)state;
    const char *fifo = "/tmp/luna-loop-fifo";
    assert_int_equal(mkfifo(fifo, 0600), 0);
    char code[256];
    snprintf(code, sizeof code,
        "loop.fs.stat('%s', function(e, st)\n"
        "  out = tostring(e) .. '|' .. tostring(st and st.type)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out", fifo);
    assert_string_equal(eval_string(code), "nil|other");
    unlink(fifo);
}

static void test_throwing_fs_callback_does_not_stop_time(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "out = 'none'\n"
        "loop.fs.readFile('/no-such-file', function() error('fs boom') end)\n"
        "loop.setTimeout(function() out = 'survived' end, 50)\n"
        "assert(loop.run())\n"
        "return out"),
        "survived");
}

static void test_throwing_fs_watch_callback_keeps_loop_alive(void **state)
{
    (void)state;
    /* the watcher throws on its first event and closes itself inside
     * the callback: run() returns normally, so the throw was eaten */
    assert_string_equal(eval_string(
        "out = 'none'\n"
        "io.open('/tmp/luna-loop-watch-t.txt', 'w'):close()\n"
        "local w\n"
        "w = loop.fs.watch('/tmp/luna-loop-watch-t.txt', function()\n"
        "  out = 'thrown'\n"
        "  w:close()\n"
        "  error('watcher boom')\n"
        "end)\n"
        "loop.fs.appendFile('/tmp/luna-loop-watch-t.txt', 'x', function() end)\n"
        "assert(loop.run())\n"
        "return out"),
        "thrown");
}

static void test_throwing_signal_callback_keeps_loop_alive(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "out = 'none'\n"
        "local w\n"
        "w = loop.signal(loop.sig.USR2, function(num)\n"
        "  out = 'thrown'\n"
        "  w:close()\n"
        "  error('signal boom')\n"
        "end)\n"
        "return 'armed'"),
        "armed");
    kill(getpid(), SIGUSR2);
    assert_string_equal(eval_string(
        "assert(loop.run())\n"
        "return out"),
        "thrown");
}

static void test_watchers_report_tostring(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "io.open('/tmp/luna-loop-watch-t.txt', 'w'):close()\n"
        "local forms = {}\n"
        "local w = loop.fs.watch('/tmp/luna-loop-watch-t.txt', function() end)\n"
        "forms[1] = tostring(tostring(w):find('loop%.fswatch') ~= nil)\n"
        "w:close()\n"
        "local s = loop.signal(loop.sig.USR2, function() end)\n"
        "forms[2] = tostring(tostring(s):find('loop%.sigwatch') ~= nil)\n"
        "s:close()\n"
        "local u = loop.udp.socket()\n"
        "forms[3] = tostring(tostring(u):find('loop%.udpsock') ~= nil)\n"
        "u:close()\n"
        "assert(loop.run())  -- drain the closing handles\n"
        "return table.concat(forms, ',')"),
        "true,true,true");
}

static void test_sock_read_before_connect_throws(void **state)
{
    (void)state;
    /* read() right after connect(), before the loop has run: the
     * socket is neither connected nor closed yet */
    int listener = tcp_listen_loopback();
    char code[512];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "out = 'none'\n"
        "local sock\n"
        "sock = net.connect('127.0.0.1', %d, function(e, s)\n"
        "  sock:close()\n"
        "end)\n"
        "local ok, err = pcall(sock.read, sock, function() end)\n"
        "out = tostring(ok) .. '|' .. tostring(tostring(err):find('not connected') ~= nil)\n"
        "assert(loop.run())\n"
        "return out", tcp_port_of(listener));
    assert_string_equal(eval_string(code), "false|true");
    close(listener);
}

static void test_sock_read_after_eof_delivers_eof_again(void **state)
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
        "  sock:write('ping', function()\n"
        "    sock:read(function(_, chunk)\n"
        "      log[1] = tostring(chunk)\n"
        "      sock:read(function(_, eof)\n"
        "        log[2] = tostring(eof)\n"
        "        sock:read(function(e2, eof2)  -- already at EOF\n"
        "          log[3] = tostring(e2) .. '/' .. tostring(eof2)\n"
        "          sock:close()\n"
        "        end)\n"
        "      end)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return table.concat(log, ',')", tcp_port_of(listener));
    /* the second read() after EOF hands back the EOF signal at once */
    assert_string_equal(eval_string(code), "ping,nil,nil/nil");
    pthread_join(th, NULL);
}

static void test_write_after_close_is_silently_dropped(void **state)
{
    (void)state;
    int listener = tcp_listen_loopback();
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, echo_main,
                                    (void *)(intptr_t)listener), 0);
    char code[512];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "out = 'none'\n"
        "net.connect('127.0.0.1', %d, function(e, sock)\n"
        "  sock:write('x', function(e2) out = 'cb:' .. tostring(e2) end)\n"
        "  sock:close()  -- the write never reports back\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out", tcp_port_of(listener));
    assert_string_equal(eval_string(code), "none");
    pthread_join(th, NULL);
}

static void test_end_closes_only_the_write_half(void **state)
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
        "  sock:write('ping', function()\n"
        "    sock:read(function(_, chunk)\n"
        "      log[1] = tostring(chunk)\n"
        "      sock:shutdown(function(e2)\n"
        "        log[2] = tostring(e2)\n"
        "        sock:read(function(_, eof)  -- server's close: EOF\n"
        "          log[3] = tostring(eof)\n"
        "          sock:close()\n"
        "        end)\n"
        "      end)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return table.concat(log, ',')", tcp_port_of(listener));
    assert_string_equal(eval_string(code), "ping,nil,nil");
    pthread_join(th, NULL);
}

static void test_net_listen_pipe_on_taken_path_fails(void **state)
{
    (void)state;
    const char *path = "/tmp/luna-loop-taken.sock";
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    assert_int_equal(bind(fd, (struct sockaddr *)&sa, sizeof sa), 0);
    assert_int_equal(listen(fd, 1), 0);

    char code[320];
    snprintf(code, sizeof code,
        "local ok, err = pcall(loop.net.listenPipe, '%s', function() end)\n"
        "return tostring(ok) .. '|' .. tostring(tostring(err):find('listen failed') ~= nil)",
        path);
    assert_string_equal(eval_string(code), "false|true");
    close(fd);
    unlink(path);
    assert_string_equal(eval_string("assert(loop.run()) return 'drained'"),
                        "drained");
}

#ifdef LUNA_LOOP_HAVE_OPENSSL
static void test_tls_handshake_fails_against_a_plain_server(void **state)
{
    (void)state;
    /* the echo server answers the ClientHello with the same bytes:
     * no TLS record stream can come of that, so the handshake fails
     * into cb(err) with no sock */
    int listener = tcp_listen_loopback();
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, echo_main,
                                    (void *)(intptr_t)listener), 0);
    char code[384];
    snprintf(code, sizeof code,
        "out = 'none'\n"
        "loop.net.connectTls('127.0.0.1', %d, {insecure = true},\n"
        "  function(e, sock)\n"
        "    out = tostring(sock == nil)\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out", tcp_port_of(listener));
    assert_string_equal(eval_string(code), "true");
    pthread_join(th, NULL);
}

static void test_tls_server_drops_a_plain_client_and_keeps_serving(void **state)
{
    (void)state;
    tls_cert_file();
    tls_key_file();
    char code[1400];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "log = {}\n"
        "local srv\n"
        "srv = net.listenTls('127.0.0.1', 0,"
        " {cert = '%s', key = '%s'}, function(e, c)\n"
        "  if e then log[1] = 'conn:' .. e return end\n"
        "  c:read(function(_, chunk)\n"
        "    if chunk then\n"
        "      log[1] = 'served'\n"
        "      c:write('pong:' .. chunk, function() c:close() end)\n"
        "    end\n"
        "  end)\n"
        "end)\n"
        "net.connect('127.0.0.1', srv:port(), function(e, raw)\n"
        "  if e then log[2] = 'plain:' .. tostring(e) return end\n"
        "  raw:write('not-a-clienthello', function()\n"
        "    raw:read(function(_, chunk)  -- dropped: never a chunk\n"
        "      log[2] = 'plain:' .. tostring(chunk)\n"
        "      raw:close()\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "net.connectTls('127.0.0.1', srv:port(), {ca = '%s'}, function(e, s)\n"
        "  if e then log[3] = 'tls:' .. e return end\n"
        "  s:write('ping', function()\n"
        "    s:read(function(_, chunk)\n"
        "      log[3] = tostring(chunk)\n"
        "      s:read(function(_, eof)\n"
        "        log[4] = tostring(eof)\n"
        "        s:close()\n"
        "        srv:close()\n"
        "      end)\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return table.concat(log, ',')",
        tls_cert_file(), tls_key_file(), tls_cert_file());
    assert_string_equal(eval_string(code), "served,plain:nil,pong:ping,nil");
    unlink("/tmp/luna-loop-tls-cert.pem");
    unlink("/tmp/luna-loop-tls-key.pem");
}
#endif

static void test_table_error_objects_are_also_swallowed(void **state)
{
    (void)state;
    /* error(obj) instead of error(str): the reporter must fall back to
     * lua_typename, and the run must still carry on */
    assert_string_equal(eval_string(
        "out = 'none'\n"
        "loop.setTimeout(function() error({code = 7}) end, 1)\n"
        "loop.fs.readFile('/no-such-file', function() error({code = 8}) end)\n"
        "loop.setTimeout(function() out = 'survived' end, 50)\n"
        "assert(loop.run())\n"
        "return out"),
        "survived");
}

static void test_watch_and_signal_callbacks_swallow_table_errors(void **state)
{
    (void)state;
    /* delivery ORDER between the fs and the signal watcher is not part
     * of the contract (it flips under load) — collect, sort, compare */
    assert_string_equal(eval_string(
        "hits = {}\n"
        "io.open('/tmp/luna-loop-watch-t.txt', 'w'):close()\n"
        "local w\n"
        "w = loop.fs.watch('/tmp/luna-loop-watch-t.txt', function()\n"
        "  hits[#hits + 1] = 'watch'\n"
        "  w:close()\n"
        "  error({code = 9})\n"
        "end)\n"
        "loop.fs.appendFile('/tmp/luna-loop-watch-t.txt', 'x', function() end)\n"
        "local s\n"
        "s = loop.signal(loop.sig.USR2, function()\n"
        "  hits[#hits + 1] = 'sig'\n"
        "  s:close()\n"
        "  error({code = 10})\n"
        "end)\n"
        "return 'armed'"),
        "armed");
    kill(getpid(), SIGUSR2);
    assert_string_equal(eval_string(
        "assert(loop.run())\n"
        "table.sort(hits)\n"
        "return table.concat(hits, '+')"),
        "sig+watch");
}

static void test_listen_rejects_bad_hosts_and_ports(void **state)
{
    (void)state;
    /* a >63-char label fails while the DNS query is built, so the host
     * leg is deterministic even behind a capturing resolver (see
     * test_dns_lookup_reports_failure_through_the_callback) */
    assert_string_equal(eval_string(
        "local net = loop.net\n"
        "local bad = string.rep('a', 70) .. '.invalid'\n"
        "local a = pcall(net.listen, bad, 0, function() end)\n"
        "local b = pcall(net.listen, '127.0.0.1', 99999, function() end)\n"
        "local c = pcall(loop.udp.bind, bad, 0, function() end)\n"
        "return tostring(a) .. tostring(b) .. tostring(c)"),
        "falsefalsefalse");
}

static void test_server_tostring_and_throwing_close_cb(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net = loop.net\n"
        "out = 'none'\n"
        "local srv = net.listen('127.0.0.1', 0, function() end)\n"
        "local form = tostring(tostring(srv):find('loop%.server') ~= nil)\n"
        "srv:close(function() out = 'close-cb-threw' error('boom') end)\n"
        "assert(loop.run())  -- drain the close\n"
        "return form .. '|' .. out"),
        "true|close-cb-threw");
}

static void test_ipv6_loopback_roundtrip_reports_inet6(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net = loop.net\n"
        "log = {}\n"
        "local srv\n"
        "srv = net.listen('::1', 0, function(e, c)\n"
        "  if e then log[1] = 'conn:' .. e return end\n"
        "  c:read(function(_, chunk)\n"
        "    if chunk then c:write(chunk, function() c:close() end) end\n"
        "  end)\n"
        "end)\n"
        "net.connect('::1', srv:port(), function(e, s)\n"
        "  if e then log[1] = 'err:' .. e return end\n"
        "  s:write('v6', function()\n"
        "    s:read(function(_, chunk)\n"
        "      log[1] = tostring(chunk)\n"
        "      log[2] = srv:address().family\n"
        "      s:close()\n"
        "      srv:close()\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return table.concat(log, ',')"),
        "v6,inet6");
}

#ifdef LUNA_LOOP_HAVE_OPENSSL
static void test_tls_onconn_and_close_cb_errors_are_swallowed(void **state)
{
    (void)state;
    tls_cert_file();
    tls_key_file();
    char code[1024];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        /* the client callback can land before the server's onConn (the
         * tls client finishes its handshake first), so collect flags
         * and order the report afterwards */
        "local seen = {}\n"
        "local srv\n"
        "srv = net.listenTls('127.0.0.1', 0,"
        " {cert = '%s', key = '%s'}, function(e, c)\n"
        "  if e then seen.onconn = 'err' return end\n"
        "  seen.onconn = tostring(tostring(c):find('tsock') ~= nil)\n"
        "  c:close()  -- the accepted conn is the callback's to close\n"
        "  error('onconn boom')\n"
        "end)\n"
        "net.connectTls('127.0.0.1', srv:port(), {insecure = true},\n"
        "  function(e, s)\n"
        "    if not s then seen.client = 'err' srv:close() return end\n"
        "    seen.client = tostring(tostring(s):find('tsock') ~= nil)\n"
        "    s:write('x', function()\n"
        "      s:close()\n"
        "      srv:close(function() seen.closed = 'yes' error('cb boom') end)\n"
        "    end)\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return table.concat({tostring(seen.onconn), tostring(seen.client),"
        " tostring(seen.closed)}, '/')\n",
        tls_cert_file(), tls_key_file());
    assert_string_equal(eval_string(code), "true/true/yes");
    unlink("/tmp/luna-loop-tls-cert.pem");
    unlink("/tmp/luna-loop-tls-key.pem");
}
#endif

static void test_proc_kill_after_exit_throws(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local process = loop.process\n"
        "local p = process.spawn('echo', {'x'}, function() end)\n"
        "p:stdin():close()\n"
        "p:stdout():close()\n"
        "p:stderr():close()\n"
        "assert(loop.run())\n"
        "local ok, err = pcall(function() return p:kill() end)\n"
        "return tostring(ok) .. '|' .. tostring(tostring(err):find('already exited') ~= nil)"),
        "false|true");
}

static void test_sock_throwing_callbacks_are_reported(void **state)
{
    (void)state;
    int listener = tcp_listen_loopback();
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, echo_main,
                                    (void *)(intptr_t)listener), 0);
    char code[512];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "out = 'none'\n"
        "net.connect('127.0.0.1', %d, function(e, sock)\n"
        "  if e then out = 'conn:' .. e return end\n"
        "  sock:read(function(_, chunk)\n"
        "    out = 'threw'\n"
        "    sock:close()\n"
        "    error('read boom')\n"
        "  end)\n"
        "  sock:write('ping')\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out", tcp_port_of(listener));
    /* the throw lands in the reporter; the run still drains */
    assert_string_equal(eval_string(code), "threw");
    pthread_join(th, NULL);
}

static void test_dns_throwing_callback_is_reported(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "out = 'none'\n"
        "loop.dns.lookup('127.0.0.1', function() error('dns boom') end)\n"
        "loop.setTimeout(function() out = 'survived' end, 200)\n"
        "assert(loop.run())\n"
        "return out"),
        "survived");
}

static void test_process_exit_callback_may_throw(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local process = loop.process\n"
        "out = 'none'\n"
        "local p = process.spawn('echo', {'x'}, function()\n"
        "  out = 'threw'\n"
        "  error('exit boom')\n"
        "end)\n"
        "p:stdin():close()\n"
        "p:stdout():close()\n"
        "p:stderr():close()\n"
        "loop.setTimeout(function() out = out .. '+drained' end, 300)\n"
        "assert(loop.run())\n"
        "return out"),
        "threw+drained");
}

/* -- round-8 dark-line sweep: whole-branch legs the suite never drove -- */

/* accepts and immediately closes: hands the client an EOF with no
 * payload, for the no-reader and close-before-EOF paths */
static void *closer_main(void *arg)
{
    int listener = (int)(intptr_t)arg;
    int c = accept(listener, NULL, NULL);
    close(c);
    close(listener);
    return NULL;
}

/* dual-stack loopback listener (V6ONLY off, bound in6addr_any):
 * 'localhost' may resolve to ::1 or 127.0.0.1 first, and both land
 * here — a specific ::1 bind would refuse the v4-mapped family */
static int tcp6_listen_loopback(void)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 addr;
    int off = 0;
    assert_int_equal(setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                                &off, sizeof off), 0);
    memset(&addr, 0, sizeof addr);
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = 0;
    assert_int_equal(bind(fd, (struct sockaddr *)&addr, sizeof addr), 0);
    assert_int_equal(listen(fd, 1), 0);
    return fd;
}

static void test_loop_now_is_monotonic_milliseconds(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "local t1 = loop.now()\n"
        "local t2\n"
        "loop.setTimeout(function() t2 = loop.now() end, 20)\n"
        "assert(loop.run())\n"
        "return tostring(type(t1) == 'number' and t2 >= t1)"), "true");
}

static void test_signal_start_failure_cleans_up_and_throws(void **state)
{
    (void)state;
    /* SIGKILL cannot have a handler: uv_signal_start fails, the
     * half-built watcher tears itself down, and the loop still drains
     * with nothing scheduled */
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "local ok, err = pcall(loop.signal, 9, function() end)\n"
        "local drained = loop.run()\n"
        "return tostring(ok) .. '|' ..\n"
        "       tostring((tostring(err):find('start failed')) ~= nil) .. '|' ..\n"
        "       tostring(drained)"), "false|true|true");
}

static void test_closed_watchers_tostring_and_double_close(void **state)
{
    (void)state;
    (void)system("rm -rf /tmp/luna-loop-tostring && mkdir -p /tmp/luna-loop-tostring");
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "out = 'none'\n"
        "local w = loop.fs.watch('/tmp/luna-loop-tostring', function() end)\n"
        "local f1 = tostring(w):find('open') ~= nil\n"
        "w:close() w:close()\n"
        "local f2 = tostring(w):find('closed') ~= nil\n"
        "local sig = loop.signal(15, function() end)\n"
        "local g1 = tostring(sig):find('open') ~= nil\n"
        "sig:close() sig:close()\n"
        "local g2 = tostring(sig):find('closed') ~= nil\n"
        "local u = loop.udp.socket()\n"
        "local h1 = tostring(u):find('open') ~= nil\n"
        "u:close() u:close()\n"
        "local h2 = tostring(u):find('closed') ~= nil\n"
        "loop.setTimeout(function()\n"
        "  out = tostring(f1) .. tostring(f2) .. tostring(g1) ..\n"
        "        tostring(g2) .. tostring(h1) .. tostring(h2)\n"
        "end, 20)\n"
        "assert(loop.run())\n"
        "return out"), "truetruetruetruetruetrue");
}

static void test_unref_ref_roundtrips_on_open_handles(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local loop = loop or require('loop')\n"
        "out = 'none'\n"
        "local u = loop.udp.bind('127.0.0.1', 0, function() end)\n"
        "local w = loop.fs.watch('/tmp/luna-loop-tostring', function() end)\n"
        "local sig = loop.signal(15, function() end)\n"
        "local srv = loop.net.listen('127.0.0.1', 0, function() end)\n"
        "u:unref() u:ref()\n"
        "w:unref() w:ref()\n"
        "sig:unref() sig:ref()\n"
        "srv:unref() srv:ref()\n"
        "u:close() w:close() sig:close() srv:close()\n"
        "loop.setTimeout(function() out = 'ok' end, 20)\n"
        "assert(loop.run())\n"
        "return out"), "ok");
}

static void test_unref_ref_on_a_live_process(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local process = loop.process\n"
        "out = 'none'\n"
        "local p = process.spawn('echo', {'x'}, function() out = 'exit' end)\n"
        "p:unref() p:ref()\n"
        "p:stdin():close()\n"
        "p:stdout():close()\n"
        "p:stderr():close()\n"
        "assert(loop.run())\n"
        "return out"), "exit");
}

static void test_process_tostring_reports_state(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local process = loop.process\n"
        "out = 'none'\n"
        "local p = process.spawn('echo', {'x'}, function() end)\n"
        "local running = tostring(p):find('running') ~= nil\n"
        "local pid = tostring(p):find('pid %d+') ~= nil\n"
        "p:stdin():close()\n"
        "p:stdout():close()\n"
        "p:stderr():close()\n"
        "assert(loop.run())\n"
        "local done = tostring(p):find('done') ~= nil\n"
        "return tostring(running) .. '|' .. tostring(pid) .. '|' ..\n"
        "       tostring(done)"), "true|true|true");
}

static void test_process_spawn_missing_binary_throws_and_drains(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local process = loop.process\n"
        "local ok, err = pcall(process.spawn, '/nonexistent-luna-zz',\n"
        "  {}, function() end)\n"
        "local drained = loop.run()\n"
        "return tostring(ok) .. '|' ..\n"
        "       tostring((tostring(err):find('spawn failed')) ~= nil) .. '|' ..\n"
        "       tostring(drained)"), "false|true|true");
}

static void test_process_run_large_stdout_grows_the_capture(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local process = loop.process\n"
        "out = 'none'\n"
        "process.run('sh', {'-c', 'head -c 100000 /dev/zero'},\n"
        "  function(e, r)\n"
        "    if e then out = 'ERR:' .. e return end\n"
        "    out = tostring(r.status) .. '|' .. tostring(#r.stdout)\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return out"), "0|100000");
}

static void test_udp_corners_throw_before_any_loop_turn(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local udp = loop.udp\n"
        "out = 'none'\n"
        "local snd = udp.socket()\n"
        "local p1 = select(2, pcall(function() return snd:port() end))\n"
        "local p2 = select(2, pcall(function() return snd:sockname() end))\n"
        "local p3 = select(2, pcall(snd.send, snd, 'x', 'localhost', 99))\n"
        "snd:close() snd:close()\n"
        "local closed = tostring(snd):find('closed') ~= nil\n"
        "loop.setTimeout(function()\n"
        "  out = tostring((tostring(p1):find('getsockname')) ~= nil) .. '|' ..\n"
        "        tostring((tostring(p2):find('getsockname')) ~= nil) .. '|' ..\n"
        "        tostring((tostring(p3):find('numeric')) ~= nil) .. '|' ..\n"
        "        tostring(closed)\n"
        "end, 20)\n"
        "assert(loop.run())\n"
        "return out"), "true|true|true|true");
}

static void test_udp_v6_roundtrip_reports_v6_names(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local udp = loop.udp\n"
        "out = 'none'\n"
        "local r, snd\n"
        "r = udp.bind('::1', 0, function(e, data, ri)\n"
        "  if e then out = 'ERR:' .. e return end\n"
        "  out = tostring(data) .. '|' .. ri.addr .. '|' ..\n"
        "        tostring(r:port() > 0) .. '|' .. r:sockname().address\n"
        "  snd:close() r:close()\n"
        "end)\n"
        "snd = udp.socket()\n"
        "snd:send('v6', '::1', r:port(), function(e2) assert(e2 == nil, e2) end)\n"
        "assert(loop.run())\n"
        "return out"), "v6|::1|true|::1");
}

static void test_udp_zero_length_datagram_delivers_empty_string(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local udp = loop.udp\n"
        "out = 'none'\n"
        "local r, snd\n"
        "r = udp.bind('127.0.0.1', 0, function(e, data, ri)\n"
        "  if e then out = 'ERR:' .. e return end\n"
        "  out = tostring(data) .. '|' .. tostring(ri.port > 0)\n"
        "  snd:close() r:close()\n"
        "end)\n"
        "snd = udp.socket()\n"
        "snd:send('', '127.0.0.1', r:port())\n"
        "assert(loop.run())\n"
        "return out"), "|true");
}

static void test_udp_send_completion_after_close_is_silent(void **state)
{
    (void)state;
    /* the send callback is dropped un-commissioned when the socket
     * closes while the datagram is still in flight; the datagram
     * itself vanishes into the dead port */
    assert_string_equal(eval_string(
        "local udp = loop.udp\n"
        "out = 'none'\n"
        "local snd = udp.socket()\n"
        "local r = udp.bind('127.0.0.1', 0, function() end)\n"
        "local port = r:port()\n"
        "r:close()               -- the target port dies first\n"
        "snd:send('x', '127.0.0.1', port, function()\n"
        "  out = 'sent-cb-ran'\n"
        "end)\n"
        "snd:close()\n"
        "loop.setTimeout(function()\n"
        "  if out == 'none' then out = 'silent' end\n"
        "end, 60)\n"
        "assert(loop.run())\n"
        "return out"), "silent");
}

static void test_udp_send_callback_may_raise(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local udp = loop.udp\n"
        "out = 'none'\n"
        "local r, snd\n"
        "r = udp.bind('127.0.0.1', 0, function() end)\n"
        "snd = udp.socket()\n"
        "snd:send('x', '127.0.0.1', r:port(), function()\n"
        "  out = 'threw'\n"
        "  snd:close() r:close()\n"
        "  error('send boom')\n"
        "end)\n"
        "loop.setTimeout(function()\n"
        "  if out == 'none' then out = 'lost' end\n"
        "end, 300)\n"
        "assert(loop.run())\n"
        "return out"), "threw");
}

static void test_udp_bind_callback_may_raise(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local udp = loop.udp\n"
        "out = 'none'\n"
        "local r, snd\n"
        "r = udp.bind('127.0.0.1', 0, function()\n"
        "  out = 'threw'\n"
        "  snd:close() r:close()\n"
        "  error('recv boom')\n"
        "end)\n"
        "snd = udp.socket()\n"
        "snd:send('x', '127.0.0.1', r:port())\n"
        "loop.setTimeout(function()\n"
        "  if out == 'none' then out = 'lost' end\n"
        "end, 300)\n"
        "assert(loop.run())\n"
        "return out"), "threw");
}

static void test_net_shutdown_semantics_on_a_live_socket(void **state)
{
    (void)state;
    /* one connection proves the whole shutdown surface: the half-close
     * delivers cb(nil), a write after it throws, a second shutdown
     * throws, and a cb-less shutdown still runs the machinery */
    int lsn1 = tcp_listen_loopback();
    int lsn2 = tcp_listen_loopback();
    int lsn3 = tcp_listen_loopback();
    pthread_t th1, th2, th3;
    assert_int_equal(pthread_create(&th1, NULL, echo_main,
                                    (void *)(intptr_t)lsn1), 0);
    assert_int_equal(pthread_create(&th2, NULL, closer_main,
                                    (void *)(intptr_t)lsn2), 0);
    assert_int_equal(pthread_create(&th3, NULL, echo_main,
                                    (void *)(intptr_t)lsn3), 0);
    char code[1536];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "out = {}\n"
        "net.connect('127.0.0.1', %d, function(e, s)\n"
        "  if e then out[1] = 'conn:' .. e return end\n"
        "  s:shutdown(function(e2) out[1] = 'shut:' .. tostring(e2) end)\n"
        "  local okw, errw = pcall(s.write, s, 'late')\n"
        "  out[2] = tostring(okw) .. '|' ..\n"
        "           tostring((tostring(errw):find('write failed')) ~= nil)\n"
        "  local oke, erre = pcall(s.shutdown, s)\n"
        "  out[3] = tostring(oke) .. '|' ..\n"
        "           tostring((tostring(erre):find('end failed')) ~= nil)\n"
        "  s:read(function() s:close() end)\n"
        "end)\n"
        "net.connect('127.0.0.1', %d, function(e, s)\n"
        "  if e then out[4] = 'conn:' .. e return end\n"
        "  s:shutdown()               -- cb-less: machinery, no callback\n"
        "  s:read(function() end)     -- the peer's EOF\n"
        "  s:close() s:close()       -- double close is a no-op\n"
        "  out[4] = 'closed-twice'\n"
        "end)\n"
        "net.connect('127.0.0.1', %d, function(e, s)\n"
        "  if e then out[5] = 'conn:' .. e return end\n"
        "  s:shutdown(function() out[5] = 'never' end)\n"
        "  s:close()                  -- close before the FIN flushes\n"
        "  out[5] = 'shut-then-close'\n"
        "end)\n"
        "assert(loop.run())\n"
        "return table.concat(out, ',', 1, 5)",
        tcp_port_of(lsn1), tcp_port_of(lsn2), tcp_port_of(lsn3));
    assert_string_equal(eval_string(code),
                        "shut:nil,false|true,false|true,closed-twice,shut-then-close");
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);
    pthread_join(th3, NULL);
}

static void test_net_connect_hostname_via_resolver(void **state)
{
    (void)state;
    /* 'localhost' may resolve to ::1 or 127.0.0.1 first; the dual-stack
     * listener accepts either, so the getaddrinfo path is deterministic */
    int listener = tcp6_listen_loopback();
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, echo_main,
                                    (void *)(intptr_t)listener), 0);
    char code[384];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "out = 'none'\n"
        "net.connect('localhost', %d, function(e, s)\n"
        "  if e then out = 'conn:' .. e return end\n"
        "  s:write('resolved', function()\n"
        "    s:read(function(_, chunk)\n"
        "      out = tostring(chunk)\n"
        "      s:close()\n"
        "    end)\n"
        "  end)\n"
        "end)\n"
        "assert(loop.run())\n"
        "return out", tcp_port_of(listener));
    assert_string_equal(eval_string(code), "resolved");
    pthread_join(th, NULL);
}

static void test_net_connect_closed_before_it_completes(void **state)
{
    (void)state;
    /* closing the sock while the dial is still in flight must keep the
     * callback silent — for both the resolver path and the direct dial */
    int fd = tcp_listen_loopback();
    int port = tcp_port_of(fd);
    close(fd);
    assert_string_equal(eval_string(
        "local net = loop.net\n"
        "out = 'none'\n"
        "local fired = false\n"
        "local a = net.connect('127.0.0.1', 1, function() fired = true end)\n"
        "a:close()\n"
        "local b = net.connect('localhost', 1, function() fired = true end)\n"
        "b:close()\n"
        "loop.setTimeout(function()\n"
        "  out = tostring(fired == false) .. '|drained'\n"
        "end, 80)\n"
        "assert(loop.run())\n"
        "return out"), "true|drained");
}

static void test_net_eof_paths_without_readers(void **state)
{
    (void)state;
    int lsn1 = tcp_listen_loopback();
    int lsn2 = tcp_listen_loopback();
    pthread_t th1, th2;
    assert_int_equal(pthread_create(&th1, NULL, closer_main,
                                    (void *)(intptr_t)lsn1), 0);
    assert_int_equal(pthread_create(&th2, NULL, closer_main,
                                    (void *)(intptr_t)lsn2), 0);
    char code[512];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "out = 'none'\n"
        "local a\n"
        "net.connect('127.0.0.1', %d, function(e, s)\n"
        "  if e then out = 'conn:' .. e return end\n"
        "  a = s   -- no read callback: the EOF lands unobserved\n"
        "end)\n"
        "net.connect('127.0.0.1', %d, function(e, s)\n"
        "  if e then out = 'conn2:' .. e return end\n"
        "  s:close()   -- close before the peer's FIN arrives\n"
        "end)\n"
        "loop.setTimeout(function()\n"
        "  if a then a:close() end   -- the peer already EOF'd us\n"
        "  out = 'eof-quiet'\n"
        "end, 80)\n"
        "assert(loop.run())\n"
        "return out", tcp_port_of(lsn1), tcp_port_of(lsn2));
    assert_string_equal(eval_string(code), "eof-quiet");
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);
}

static void test_server_conn_callback_may_raise(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net = loop.net\n"
        "out = 'none'\n"
        "local srv = net.listen('127.0.0.1', 0, function(e, c)\n"
        "  if e then out = 'conn:' .. e return end\n"
        "  c:close()\n"
        "  error('accept boom')\n"
        "end)\n"
        "net.connect('127.0.0.1', srv:port(), function(e, s)\n"
        "  if e then out = 'dial:' .. e return end\n"
        "  s:close()\n"
        "end)\n"
        "loop.setTimeout(function()\n"
        "  srv:close() srv:close()   -- double close is a no-op\n"
        "  if out == 'none' then out = 'survived' end\n"
        "end, 80)\n"
        "assert(loop.run())\n"
        "return out"), "survived");
}

static void test_fs_write_enospc_reports_through_the_callback(void **state)
{
    (void)state;
    /* /dev/full accepts opens and fails every write with ENOSPC: the
     * failure rides back through cb(err), not a throw */
    assert_string_equal(eval_string(
        "local fs = loop.fs\n"
        "out = 'none'\n"
        "local n = 0\n"
        "local function done(e)\n"
        "  n = n + 1\n"
        "  if e == nil or (tostring(e):find('space') == nil and\n"
        "                  tostring(e):find('ENOSPC') == nil) then\n"
        "    out = 'WRONG:' .. tostring(e)\n"
        "    return\n"
        "  end\n"
        "  if n == 2 then out = 'enospc-x2' end\n"
        "end\n"
        "fs.writeFile('/dev/full', 'x', done)\n"
        "fs.appendFile('/dev/full', 'y', done)\n"
        "assert(loop.run())\n"
        "return out"), "enospc-x2");
}

static void test_broken_attach_step_never_stops_time(void **state)
{
    (void)state;
    /* __LUNA_SERVE_STEP exists but is broken: the prepare hook reports
     * and moves on, and the run still drains to a natural return */
    assert_string_equal(eval_string(
        "__LUNA_SERVE_STEP = function() error('attach boom') end\n"
        "out = 'none'\n"
        "loop.setTimeout(function() out = 'survived' end, 50)\n"
        "assert(loop.run())\n"
        "return out"), "survived");
}

#ifdef LUNA_LOOP_HAVE_OPENSSL

/* -- round-8 TLS sweep: the tsock/tserver legs the suite never drove -- */

static void test_tls_listener_coordinates_then_closed_throws(void **state)
{
    (void)state;
    const char *cert = tls_cert_file();
    const char *key = tls_key_file();
    char code[1024];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "out = 'none'\n"
        "local srv = net.listenTls('127.0.0.1', 0,"
        " {cert = '%s', key = '%s'}, function(e, c)\n"
        "  if c then c:close() end\n"
        "end)\n"
        "local form1 = tostring(srv):find('tserver') ~= nil\n"
        "local port = srv:port()\n"
        "local addr = srv:address()\n"
        "srv:close(function() end)\n"
        "srv:close(function() end)   -- second pin releases the first\n"
        "local ok1, e1 = pcall(function() return srv:port() end)\n"
        "local ok2, e2 = pcall(function() return srv:address() end)\n"
        "local form2 = tostring(srv):find('closed') ~= nil\n"
        "loop.setTimeout(function()\n"
        "  out = tostring(form1) .. '|' .. tostring(port > 0) .. '|' ..\n"
        "        addr.address .. '|' .. addr.family .. '|' ..\n"
        "        tostring((tostring(e1):find('closed')) ~= nil) .. '|' ..\n"
        "        tostring((tostring(e2):find('closed')) ~= nil) .. '|' ..\n"
        "        tostring(form2)\n"
        "end, 20)\n"
        "assert(loop.run())\n"
        "return out", cert, key);
    assert_string_equal(eval_string(code),
                        "true|true|127.0.0.1|inet|true|true|true");
    unlink("/tmp/luna-loop-tls-cert.pem");
    unlink("/tmp/luna-loop-tls-key.pem");
}

static void test_tls_listen_rejects_bad_hosts_and_taken_ports(void **state)
{
    (void)state;
    const char *cert = tls_cert_file();
    const char *key = tls_key_file();
    char code[1024];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "out = 'none'\n"
        "local ok1, e1 = pcall(net.listenTls, 'bogus-host-zz', 0,\n"
        "  {cert = '%s', key = '%s'}, function() end)\n"
        "local srv = net.listenTls('127.0.0.1', 0,\n"
        "  {cert = '%s', key = '%s'}, function(e, c)\n"
        "    if c then c:close() end\n"
        "  end)\n"
        "local ok2, e2 = pcall(net.listenTls, '127.0.0.1', srv:port(),\n"
        "  {cert = '%s', key = '%s'}, function() end)\n"
        "srv:close()\n"
        "loop.setTimeout(function()\n"
        "  out = tostring(ok1 == false) .. '|' ..\n"
        "        tostring((tostring(e1):find('bind')) ~= nil) .. '|' ..\n"
        "        tostring(ok2 == false) .. '|' ..\n"
        "        tostring((tostring(e2):find('listen failed')) ~= nil)\n"
        "end, 20)\n"
        "assert(loop.run())\n"
        "return out", cert, key, cert, key, cert, key);
    assert_string_equal(eval_string(code), "true|true|true|true");
    unlink("/tmp/luna-loop-tls-cert.pem");
    unlink("/tmp/luna-loop-tls-key.pem");
}

static void test_tls_client_rejects_a_broken_ca_file(void **state)
{
    (void)state;
    assert_string_equal(eval_string(
        "local net = loop.net\n"
        "local ok, err = pcall(net.connectTls, '127.0.0.1', 443,\n"
        "  {ca = '/nonexistent-luna-zz.pem'}, function() end)\n"
        "local drained = loop.run()\n"
        "return tostring(ok) .. '|' ..\n"
        "       tostring((tostring(err):find('CA file')) ~= nil) .. '|' ..\n"
        "       tostring(drained)"), "false|true|true");
}

static void test_tls_client_dials_by_hostname(void **state)
{
    (void)state;
    /* the dial goes through the async resolver ('localhost'); insecure
     * keeps the handshake deterministic — the baked-in cert carries an
     * IP SAN, not a localhost DNS name */
    const char *cert = tls_cert_file();
    const char *key = tls_key_file();
    char code[1152];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "log = {}\n"
        "srv = net.listenTls('127.0.0.1', 0,"
        " {cert = '%s', key = '%s'}, function(e, c)\n"
        "  if e then log[1] = 'conn:' .. e return end\n"
        "  c:read(function(e2, chunk)\n"
        "    if chunk then\n"
        "      c:write('pong:' .. chunk, function() c:close() end)\n"
        "    end\n"
        "  end)\n"
        "end)\n"
        "net.connectTls('localhost', srv:port(), {insecure = true},\n"
        "  function(e, s)\n"
        "    if e then log[1] = 'connect:' .. e return end\n"
        "    s:write('ping', function(e3)\n"
        "      if e3 then log[1] = 'write:' .. e3 return end\n"
        "      s:read(function(e4, chunk)\n"
        "        log[1] = tostring(chunk)\n"
        "        s:read(function(e5, eof)\n"
        "          log[2] = tostring(eof)\n"
        "          s:close()\n"
        "          srv:close()\n"
        "        end)\n"
        "      end)\n"
        "    end)\n"
        "  end)\n"
        "assert(loop.run())\n"
        "return table.concat(log, ',')", cert, key);
    assert_string_equal(eval_string(code), "pong:ping,nil");
    unlink("/tmp/luna-loop-tls-cert.pem");
    unlink("/tmp/luna-loop-tls-key.pem");
}

static void test_tls_client_closed_before_resolution_answers(void **state)
{
    (void)state;
    /* closing while the resolver is still working answers the connect
     * callback with 'closed before handshake' at once, then the late
     * resolution lands on the closed tsock and is dropped quietly
     * (the half-built sock finishes inline — no poll existed) */
    assert_string_equal(eval_string(
        "local net = loop.net\n"
        "out = 'none'\n"
        "local t = net.connectTls('localhost', 443, function(e)\n"
        "  out = tostring(e)\n"
        "end)\n"
        "t:close()\n"
        "assert(out:find('closed before handshake') ~= nil)\n"
        "loop.setTimeout(function() out = out .. '|drained' end, 60)\n"
        "assert(loop.run())\n"
        "return out"), "tls: closed before handshake|drained");
}

static void test_tls_read_swap_and_tostring(void **state)
{
    (void)state;
    const char *cert = tls_cert_file();
    const char *key = tls_key_file();
    char code[1408];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "log = {}\n"
        "srv = net.listenTls('127.0.0.1', 0,"
        " {cert = '%s', key = '%s'}, function(e, c)\n"
        "  if e then return end\n"
        "  c:read(function(e2, chunk)\n"
        "    if chunk then\n"
        "      c:write('pong:' .. chunk, function() c:close() end)\n"
        "    end\n"
        "  end)\n"
        "end)\n"
        "net.connectTls('127.0.0.1', srv:port(), {insecure = true},\n"
        "  function(e, s)\n"
        "    if e then log[1] = 'connect:' .. e return end\n"
        "    local f1 = tostring(s):find('tsock') ~= nil\n"
        "    s:read(function() end)\n"
        "    s:read(function(e2, chunk)   -- swap: the first cb is dropped\n"
        "      log[1] = tostring(chunk)\n"
        "      s:write('ack', function()\n"
        "        local f2 = tostring(s):find('open') ~= nil\n"
        "        s:close()\n"
        "        local f3 = tostring(s):find('closed') ~= nil\n"
        "        log[2] = tostring(f1) .. tostring(f2) .. tostring(f3)\n"
        "        srv:close()\n"
        "      end)\n"
        "    end)\n"
        "    s:write('ping')   -- both readers are on already\n"
        "  end)\n"
        "loop.setTimeout(function()\n"
        "  if #log < 2 then log = {'bail'} end\n"
        "  srv:close()\n"
        "end, 1000)\n"
        "assert(loop.run())\n"
        "return table.concat(log, ',')", cert, key);
    assert_string_equal(eval_string(code), "pong:ping,truetruetrue");
    unlink("/tmp/luna-loop-tls-cert.pem");
    unlink("/tmp/luna-loop-tls-key.pem");
}

static void test_tls_write_callback_may_raise(void **state)
{
    (void)state;
    const char *cert = tls_cert_file();
    const char *key = tls_key_file();
    char code[1024];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "out = 'none'\n"
        "local srv = net.listenTls('127.0.0.1', 0,"
        " {cert = '%s', key = '%s'}, function(e, c)\n"
        "  if e then return end\n"
        "  c:read(function() c:close() end)\n"
        "end)\n"
        "net.connectTls('127.0.0.1', srv:port(), {insecure = true},\n"
        "  function(e, s)\n"
        "    if e then out = 'connect:' .. e return end\n"
        "    s:write('x', function()\n"
        "      out = 'threw'\n"
        "      s:close()\n"
        "      srv:close()\n"
        "      error('tls write boom')\n"
        "    end)\n"
        "  end)\n"
        "loop.setTimeout(function()\n"
        "  if out == 'none' then out = 'lost' end\n"
        "  srv:close()\n"
        "end, 500)\n"
        "assert(loop.run())\n"
        "return out", cert, key);
    assert_string_equal(eval_string(code), "threw");
    unlink("/tmp/luna-loop-tls-cert.pem");
    unlink("/tmp/luna-loop-tls-key.pem");
}

static void test_tls_big_write_pends_then_flushes(void **state)
{
    (void)state;
    /* a payload far past the sender's socket buffer: the first write
     * pends (SSL_write went partial), a second write throws 'in
     * flight'; the server drains continuously and hears EOF once the
     * fully-flushed client closes — then a close-with-callback hears
     * the close. (Closing mid-transfer would RST the peer and the
     * flush error is timing-dependent, so the server reads to EOF.) */
    const char *cert = tls_cert_file();
    const char *key = tls_key_file();
    char code[1408];
    snprintf(code, sizeof code,
        "local net = loop.net\n"
        "log = {}\n"
        "srv = net.listenTls('127.0.0.1', 0,"
        " {cert = '%s', key = '%s'}, function(e, c)\n"
        "  if e then return end\n"
        "  local function arm()   -- reads are one-shot: re-arm each chunk\n"
        "    c:read(function(_, chunk)\n"
        "      if chunk then arm() else c:close() srv:close() end\n"
        "    end)\n"
        "  end\n"
        "  arm()\n"
        "end)\n"
        "net.connectTls('127.0.0.1', srv:port(), {insecure = true},\n"
        "  function(e, s)\n"
        "    if e then log[1] = 'connect:' .. e return end\n"
        "    s:write(string.rep('x', 64000000), function(e2)\n"
        "      log[1] = tostring(e2)\n"
        "      s:close(function(e3) log[3] = tostring(e3) end)\n"
        "    end)\n"
        "    -- the first write pends (64MB outruns any socket buffer);\n"
        "    -- on a freak kernel that takes it whole, the sock is closed\n"
        "    -- by the callback already — either way write #2 is refused\n"
        "    local ok, err = pcall(s.write, s, 'second')\n"
        "    log[2] = tostring(ok) .. '|' ..\n"
        "            tostring((tostring(err):find('in flight') or\n"
        "                      tostring(err):find('closed')) ~= nil)\n"
        "  end)\n"
        "loop.setTimeout(function()\n"
        "  log[7] = 'bail'\n"
        "  srv:close()\n"
        "end, 8000)\n"
        "assert(loop.run())\n"
        "return table.concat(log, ',')", cert, key);
    assert_string_equal(eval_string(code), "nil,false|true,nil");
    unlink("/tmp/luna-loop-tls-cert.pem");
    unlink("/tmp/luna-loop-tls-key.pem");
}

#endif  /* LUNA_LOOP_HAVE_OPENSSL */

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
        cmocka_unit_test_setup_teardown(test_fs_copyfile_copies_and_overwrites, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_access_probes_existence, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_realpath_resolves, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_truncate_shortens_and_defaults_to_zero, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_stat_and_lstat_report_types, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_udp_send_recv_loopback, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_udp_sockname_reports_bind, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_udp_bind_conflict_throws, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_udp_send_without_callback_drains, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_watch_reports_events, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_watch_missing_path_throws, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_watch_close_is_idempotent, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_immediate_beats_timer_with_io_watchers, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_signal_self_delivery, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_signal_reserved_refused, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_signal_multiple_watchers_fanout, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_signal_close_stops_delivery, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_unref_interval_does_not_keep_loop_alive, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_unref_server_runs_but_does_not_keep, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_ref_restores_keepalive, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_dns_lookup_resolves_localhost, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_dns_lookup_reports_failure_through_the_callback, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_dns_lookup_of_an_overlong_host_throws, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_dns_reverse_of_a_non_address_throws, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_dns_reverse_maps_loopback, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_os_basics_report_sane_values, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_os_cpus_lists_each_with_times, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_os_network_interfaces_lists_loopback, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_tcp_echo_then_eof, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_tcp_connect_refused_yields_error, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_pipe_echo, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_tcp_server_echo, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_pipe_server_echo, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_listen_on_taken_port_fails, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_read_survives_multiple_chunks, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_sock_addr_tcp, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_sock_addr_pipe, setup_loop, teardown_loop),
#ifdef LUNA_LOOP_HAVE_OPENSSL
        cmocka_unit_test_setup_teardown(test_tls_insecure_echo_then_eof, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_custom_ca_accepts_self_signed, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_default_verify_rejects_self_signed, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_connect_refused_yields_error, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_listen_with_custom_ca_roundtrips, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_listen_default_verify_rejected, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_listen_bad_cert_throws, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_sock_addr_and_server_address, setup_loop, teardown_loop),
#endif
        cmocka_unit_test_setup_teardown(test_http_get_plain_roundtrip, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_get_chunked_body, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_post_sends_method_headers_body, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_follows_redirect_and_folds_post, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_relative_location, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_redirect_budget_and_timeout, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_server_roundtrip, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_server_error_paths, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_stream_content_length, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_stream_eof_mode, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_stream_onhead_once_after_redirect, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_bad_url_throws, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_bare_url_defaults_the_path, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_head_and_chunked_arrive_in_pieces, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_chunk_body_split_across_reads, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_body_split_across_reads, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_bad_chunk_size_is_an_error, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_malformed_head_is_an_error, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_duplicate_response_headers_join, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_redirect_location_shapes, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_204_head_is_the_whole_response, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_onhead_may_raise, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_stream_chunked_hands_one_piece, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_stream_ondata_may_raise_chunked, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_stream_ondata_may_raise_content_length, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_unframed_eof_body, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_eof_before_head, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_eof_mid_body, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_reset_mid_response_surfaces_an_error, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_connect_refused_is_the_error, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_server_res_send_is_idempotent, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_server_client_vanishes_then_recovers, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_server_head_runaway_gets_400, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_server_dup_headers_and_slow_body, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_http_listen_requires_a_handler, setup_loop, teardown_loop),
#ifdef LUNA_LOOP_HAVE_OPENSSL
        cmocka_unit_test_setup_teardown(test_http_get_over_tls, setup_loop, teardown_loop),
#endif
        cmocka_unit_test_setup_teardown(test_proc_run_echo_captures_stdout, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_run_exit_code_and_stderr, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_run_cwd_option, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_kill_reports_signal, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_spawn_failure_throws_and_drains, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_spawn_cat_roundtrip, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_spawn_stderr_is_separate, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_spawn_exit_fires_without_readers, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_proc_spawn_stdio_become_nil_after_exit, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_run_rejects_an_unknown_mode, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_timer_handles_report_tostring, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_throwing_timer_callback_does_not_stop_time, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_errors_report_through_the_callback, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_read_of_a_directory_fails_mid_chain, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_stat_reports_other_for_a_fifo, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_throwing_fs_callback_does_not_stop_time, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_throwing_fs_watch_callback_keeps_loop_alive, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_throwing_signal_callback_keeps_loop_alive, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_watchers_report_tostring, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_sock_read_before_connect_throws, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_sock_read_after_eof_delivers_eof_again, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_write_after_close_is_silently_dropped, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_end_closes_only_the_write_half, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_listen_pipe_on_taken_path_fails, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_table_error_objects_are_also_swallowed, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_watch_and_signal_callbacks_swallow_table_errors, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_listen_rejects_bad_hosts_and_ports, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_server_tostring_and_throwing_close_cb, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_ipv6_loopback_roundtrip_reports_inet6, setup_loop, teardown_loop),
#ifdef LUNA_LOOP_HAVE_OPENSSL
        cmocka_unit_test_setup_teardown(test_tls_handshake_fails_against_a_plain_server, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_server_drops_a_plain_client_and_keeps_serving, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_onconn_and_close_cb_errors_are_swallowed, setup_loop, teardown_loop),
#endif
        cmocka_unit_test_setup_teardown(test_proc_kill_after_exit_throws, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_sock_throwing_callbacks_are_reported, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_dns_throwing_callback_is_reported, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_process_exit_callback_may_throw, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_loop_now_is_monotonic_milliseconds, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_signal_start_failure_cleans_up_and_throws, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_closed_watchers_tostring_and_double_close, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_unref_ref_roundtrips_on_open_handles, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_unref_ref_on_a_live_process, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_process_tostring_reports_state, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_process_spawn_missing_binary_throws_and_drains, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_process_run_large_stdout_grows_the_capture, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_udp_corners_throw_before_any_loop_turn, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_udp_v6_roundtrip_reports_v6_names, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_udp_zero_length_datagram_delivers_empty_string, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_udp_send_completion_after_close_is_silent, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_udp_send_callback_may_raise, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_udp_bind_callback_may_raise, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_shutdown_semantics_on_a_live_socket, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_connect_hostname_via_resolver, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_connect_closed_before_it_completes, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_net_eof_paths_without_readers, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_server_conn_callback_may_raise, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_fs_write_enospc_reports_through_the_callback, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_broken_attach_step_never_stops_time, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_listener_coordinates_then_closed_throws, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_listen_rejects_bad_hosts_and_taken_ports, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_client_rejects_a_broken_ca_file, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_client_dials_by_hostname, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_client_closed_before_resolution_answers, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_read_swap_and_tostring, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_write_callback_may_raise, setup_loop, teardown_loop),
        cmocka_unit_test_setup_teardown(test_tls_big_write_pends_then_flushes, setup_loop, teardown_loop),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
