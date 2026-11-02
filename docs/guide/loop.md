# Event loop

luna's event loop is a direct exposure of libuv with Node naming and Lua semantics. It is an **explicit choice**: the synchronous standard library is unaffected, and only code that does `require "loop"` enters the asynchronous world — this is the first step of the architecture note's "optional async in script mode, progressively exposed in the REPL."

```lua
local loop = require("loop")

local ticks = 0
loop.setInterval(function() ticks = ticks + 1 end, 1000)
loop.setTimeout(function()
    print("a minute passed, ticks = " .. ticks)
end, 60000, "unused-arg")     -- trailing arguments pass through unchanged

loop.run()                    -- drives the loop until no handles remain
```

## API

| Call | Semantics |
| --- | --- |
| `loop.setTimeout(fn, ms?, ...)` | Calls once after `ms` milliseconds; returns a handle |
| `loop.setInterval(fn, ms?, ...)` | Repeats every `ms` milliseconds; returns a handle |
| `loop.setImmediate(fn, ...)` | Runs in the check phase of the next loop turn |
| `loop.clearTimeout(h)` / `loop.clearInterval(h)` / `loop.clearImmediate(h)` | Clears a handle; on an already-fired one-shot it is a harmless no-op |
| `loop.run(mode?)` | `"default"` (default, runs to empty), `"once"` (one turn, blocks waiting for readiness), `"nowait"` (one turn, no blocking) |
| `loop.maybeDrain()` | Drains only when live handles exist (equivalent to `run()`), otherwise a no-op — the public face of script-tail auto-drain |
| `loop.turn()` | One non-blocking turn (`UV_RUN_NOWAIT`): dispatches callbacks that are ready now and never waits; `^C` inside the turn follows `run` and throws `interrupted` |
| `loop.waitMs()` | How long the loop wants to block (`uv_backend_timeout`): `0` when work is ready, `n` until the next timer, `nil` when there is nothing to wait for (an empty loop, or handles with no timers) |
| `loop.stop()` | Makes the current `run` return as soon as possible; handles stay in their scheduled state |
| `loop.now()` | The loop millisecond clock (`uv_now`) |
| `handle:unref()` / `handle:ref()` | Removes/restores a handle's keep-alive (see the next section) |

For coroutine-style await/cancel and one-shot promises, the sugar layer is [Async tasks](/zh/guide/tasks) (`require "task"`), built on this table's `setTimeout`/`clearTimeout` without adding a loop API.

Behavior rules:

- **Callback isolation**: an error thrown in a callback only goes to stderr and the loop continues — the same way the REPL isolates one evaluation error;
- **keep-alive**: `run("default")` returns naturally once the last handle closes; an uncleared interval makes it never return, just like Node;
- **`^C` interrupt**: while the loop blocks in `uv_run`, the count hook cannot see the signal — the prepare hook takes over: a raised `^C` makes `run` throw `interrupted`, and the script exits with code 130 as usual. The precondition is that **the loop is turning**: the prepare hook runs only at the start of each iteration, while a fully silent loop (say, only an `fs.watch` waiting) sits blocked in epoll, where libuv retries EINTR internally without ending the iteration, so the flag is invisible — a long-lived silent service must give the loop a heartbeat: `http.serve`'s 100 ms heartbeat, `net.serve`'s 250 ms accept deadline, and the automation example's heartbeat all follow this rule;
- **attach remains reachable**: the same prepare hook also polls the attach socket — a process running the event loop can still be inspected, evaluated, and changed through `luna --attach`; every constraint of `serve.step` is unchanged (reached only at polling points).

## handle:unref/ref: what keeps the loop alive

Every handle keeps the loop alive by default: as long as a handle exists, `run("default")` does not return. `unref` takes one handle off that account — it still runs (a timer still fires and its callback is still delivered), it just no longer keeps the loop alive on its own; `ref` puts it back on the account:

```lua
local loop = require("loop")

local iv
iv = loop.setInterval(function() log_flush() end, 30000)
iv:unref()                    -- periodic logging does not block natural script exit

loop.run()                    -- returns once other handles clear; the interval dies with the process
```

| Call | Semantics |
| --- | --- |
| `handle:unref()` | The handle keeps running but no longer holds the loop open; idempotent |
| `handle:ref()` | Restores keep-alive; idempotent |

This applies to every handle: timer/interval/immediate handles, the `fs.watch` watcher, `loop.signal` watchers, `loop.net` socks and servers, `loop.udp` socks, and `loop.process` procs (including the three stdio socks of `process.spawn`).

Behavior rules:

- **unref is not pause**: the handle's running, callback delivery, and error paths are all unchanged — the only difference is that the keep-alive count no longer counts it; while the loop turns because of other handles, an unref'd timer fires as usual;
- **this is Node's `unref` semantics**: timers, servers, and watchers can all be "present but not holding the door" — background periodic tasks, listeners that do not care when a connection arrives, observers that glance once and leave;
- **cleanup responsibility remains yours**: unref affects only when the loop exits; it does not clean up for you — before a script ends, still `close`/`clear*` handles you no longer need (calling them on an already-closed handle is a safe no-op);
- **run's return does not distinguish who kept it alive**: once everything is unref'd, `run("default")` returns, indistinguishable from "no handles" — to know whether work remains, track state in your callbacks; don't infer it from the loop.

## Script-tail auto-drain

After the main chunk of `luna script.lua` and `luna -e` **ends normally**, if the loop still has keep-alive handles, it automatically drains the loop before exiting — Node's "run the event loop to empty, then exit" contract:

```lua
-- timer.lua: no loop.run(), and the timer still fires
local loop = require("loop")
loop.setTimeout(function()
    print("fired")
end, 500)
```

- **It only affects scripts that used loop but never called `run()`**: today the handles are silently dropped; under the new contract they are executed to completion — strictly closer to the intent. Scripts that already call `run()` are unaffected (by the time the main chunk ends, the count is empty, and drain spins without doing anything);
- **unref semantics apply unchanged**: a script whose handles are all unref'd exits immediately, and callbacks do not fire;
- **a main-chunk error skips drain**: when a script ends in an error, the process exits directly, matching Node's uncaught exception behavior;
- **`^C` during drain follows code 130**: `maybeDrain` throws `interrupted` like `run`, and the exit-code convention is unchanged;
- **the REPL drains on a timer, with session-level opt-in**: after an interactive session does `require "loop"`, an expired timer interrupts the line editor's blocking read while the prompt idles — one empty-line tick runs one loop turn (`turn`) and returns to the prompt. Detection reads `package.loaded`, so without loading the loop, not a single extra byte is executed; multi-line blocks are never interrupted by timers (an empty line is block content); typing drafts are never interrupted (the tick lands only at an empty prompt). The three synchronous contracts of line-editing's blocking read, `^C`/130, and attach polling points remain unchanged.

## loop.fs: asynchronous file IO

`loop.fs` connects libuv thread-pool file operations to the same callback convention — Node's `fs` style, with read/write/stat plus directory management and Node's "error-first callback" shape:

```lua
local fs = require("loop").fs

fs.readFile("/etc/hostname", function(err, data)
    print(err and err or data:gsub("\n", ""))
end)

fs.writeFile("/tmp/note.txt", "hello\n", function(err)
    assert(err == nil, err)
    fs.stat("/tmp/note.txt", function(err, st)
        print(st.size)          -- 6
    end)
end)

loop.run()
```

| Call | Success callback | Failure callback |
| --- | --- | --- |
| `fs.readFile(path, cb)` | `cb(nil, data)` (the whole file as one string) | `cb(err)` |
| `fs.writeFile(path, data, cb)` | `cb(nil)` (overwrite; creates if missing) | `cb(err)` |
| `fs.appendFile(path, data, cb)` | `cb(nil)` (append; creates if missing) | `cb(err)` |
| `fs.stat(path, cb)` | `cb(nil, st)`, with `st` holding `size` / `mtime` / `mode` / `type` | `cb(err)` |
| `fs.readdir(path, cb)` | `cb(nil, names)` — an array of filenames, without `.`/`..`, in no guaranteed order | `cb(err)` |
| `fs.mkdir(path, cb)` | `cb(nil)` (mode 0777, umask applies as usual; **not recursive** — the parent must exist) | `cb(err)` |
| `fs.rmdir(path, cb)` | `cb(nil)` (removes only an empty directory) | `cb(err)` |
| `fs.unlink(path, cb)` | `cb(nil)` | `cb(err)` |
| `fs.rename(old, new, cb)` | `cb(nil)` (atomic rename/move; overwrites `new` when present) | `cb(err)` |
| `fs.copyFile(src, dst, cb)` | `cb(nil)` (silently overwrites dst when present) | `cb(err)` |
| `fs.access(path, cb)` | `cb(nil)` — the path exists and is stat-able | `cb(err)` |
| `fs.realpath(path, cb)` | `cb(nil, resolved)` — the absolute path after resolving `..` and symlinks | `cb(err)` |
| `fs.truncate(path, len?, cb)` | `cb(nil)`; an omitted `len` truncates to 0 (Node-style overload: `cb` can follow `path` directly) | `cb(err)` |
| `fs.lstat(path, cb)` | `cb(nil, st)`, same fields as stat; **does not follow symlinks** | `cb(err)` |

- **IO runs on the thread pool; callbacks run on the loop thread**: large reads and writes do not block timers; callbacks still execute in isolation, and an error does not affect the loop;
- **errors are strings**: `uv_strerror` passes through verbatim (such as `no such file or directory`), a deliberate simplification versus Node's Error objects — in Lua, `err ~= nil` is enough, and stat on a missing path also calls `cb(err)`;
- **nested operations are safe**: a callback may start the next operation — the keep-alive count is allowed to briefly reach zero between two operations, and the callback chain continues within `run()` all the way to completion;
- **readdir is `uv_fs_scandir` underneath**: one thread-pool call lists all entries, names are copied into Lua before delivery, and libuv reclaims the list itself;
- **`st.type` is a type string**: `"file"` / `"dir"` / `"link"` / `"other"` — for classification read it first rather than decoding `mode` bits; `stat` follows symlinks by definition, and `"link"` comes only from `lstat`;
- **copyFile overwrites; access only tests existence**: `copyFile` has no "do not overwrite" switch (an existing dst is replaced, Node's default); `access` answers only "is it there" — readability and writability are left to the `err` of `read`/`write`, not pre-checked;
- **truncate can also grow**: when `len` exceeds the original length the file is extended with zero bytes (POSIX `ftruncate` semantics, same as Node); libuv has only an fd-based truncate, so the implementation goes `open` → `ftruncate` → `close`, a failure along the way still calls `cb(err)`, and the fd is always closed.

### fs.watch: watching directory changes

`fs.watch` is `loop.fs`'s observation face: it watches a directory, and file creation, modification, rename, and deletion are delivered as events — on Linux a direct inotify connection:

```lua
local fs = require("loop").fs

local w                       -- split declaration: read w inside the callback (see the net scope reminder)
w = fs.watch("/tmp/log", function(err, filename, event)
    if err then print(err) return end
    print(event, filename)    -- rename	note.txt
end)

loop.setTimeout(function() w:close() end, 5000)
loop.run()
```

| Call | Semantics |
| --- | --- |
| `fs.watch(path, onEvent)` | Watches directory `path` (watching a single file is not guaranteed platform-consistent; a directory is the common semantics); a missing path **throws synchronously**; returns a watcher |
| `watcher:close()` | Idempotent close; after closing it delivers nothing, and an open watcher keeps the loop alive |

Behavior rules:

- **the event callback is resident**: the reference to `onEvent(err, filename, event)` is held by the implementation, like `udp.bind`'s receive callback; `event` is `"rename"` (create/rename/delete) or `"change"` (content/attributes);
- **events are coalesced**: adjacent kernel-side changes are synthesized into one delivery, and `filename` is best-effort (nil for some events) — treat it as a hint, not a fact; for the truth, `fs.stat`;
- **not recursive**: it watches only the directory itself; changes inside subdirectories are not reported — for recursion, `fs.watch` each level;
- **watch does not guarantee delivery**: the last changes before a process crash, or changes made before the watch was established, are unknowable — it is complementary to tail/sync scanning, not a replacement;
- **the watched directory's deletion arrives as an error**: when the watched directory itself is removed, `onEvent(err, ...)` gets an error and the watcher becomes invalid.

## loop.signal: asynchronous signals

`loop.signal` is the loop's sixth block: it connects Unix signals into the callback world — graceful exit for daemon-style scripts (receive SIGTERM, close servers and flush first, then exit) is its home turf:

```lua
local loop = require("loop")

local closing = false
loop.signal(loop.sig.TERM, function(num)
    if closing then return end
    closing = true
    print("graceful shutdown on signal " .. num)
    -- close server, flush…then the last handle closes and run() returns naturally
end)

loop.run()
```

| Call | Semantics |
| --- | --- |
| `loop.signal(signum, cb)` | Registers a resident signal callback `cb(signum)`; multiple watchers for the same signal **all** fire (libuv fans out); returns a watcher |
| `loop.sig.HUP / INT / QUIT / … / TERM / USR1 / USR2 / …` | The signal-number constant table (Linux standard: `TERM`=15, `USR2`=12, …) |
| `watcher:close()` | Idempotent close; delivers nothing after closing |

Behavior rules:

- **SIGINT and SIGUSR1 registration is refused**: `^C` is the loop's own interrupt key (it throws `interrupted` on timeout, exit code 130), and SIGUSR1 is attach's doorbell — these two have owners, so `signal` throws synchronously; don't touch them;
- **the last watcher's close restores default handling**: after the last watcher for a signal is `close()`d, libuv withdraws its own handling and restores the kernel default — the next signal either terminates the process or not. To make a process "immune" to a signal, keep at least one watcher in place; to keep it from holding the loop open, `watcher:unref()` (see [handle:unref/ref](#handlerefref-what-keeps-the-loop-alive)): handling stays installed, delivery still works, it just does not stop `run` from draining;
- **SIGKILL/SIGSTOP numbers are in the table, but the kernel never delivers them**: these two cannot be caught, per Unix rules;
- **callback isolation**: an error thrown in a callback only goes to stderr and the loop continues; a signal arriving between loop turns is not lost — the handler is installed, the event is queued by libuv, and delivered on the next `run` turn;
- **signals are not a queue**: repeated same-signal delivery is not queued by the kernel (standard signals merge) — the callback may fire fewer times than the signal was sent; count them yourself if you need an exact count.

## loop.dns: asynchronous name resolution

`loop.dns` is the loop's seventh block: it separates the thread-pool resolver that `net.connect` uses internally into its own face — names to addresses and addresses to names, under the same "error-first callback" contract:

```lua
local dns = require("loop").dns

dns.lookup("example.com", function(err, addr)
    assert(err == nil, err)
    print(addr)               -- 93.184.216.34 (or the first v6 address)
end)

dns.reverse("127.0.0.1", function(err, name)
    assert(err == nil, err)
    print(name)               -- localhost
end)

require("loop").run()
```

| Call | Semantics |
| --- | --- |
| `dns.lookup(host, cb)` | `cb(nil, addr)` — the **first** address in the result list (v4 or v6); a resolution failure gives `cb(err)` |
| `dns.reverse(addr, cb)` | `cb(nil, hostname)`; `addr` must be a **numeric address** (v4/v6), otherwise it throws synchronously; a missing name gives `cb(err)` |

Behavior rules:

- **resolution runs on the thread pool; callbacks run on the loop thread**: a real DNS query (when `/etc/hosts` is not hit) can take hundreds of milliseconds, during which timers keep turning;
- **lookup returns only the first address**: no full family and no round-robin — to select an address yourself, use `net.connect`, which already resolves by the same rule at the connection layer;
- **errors are strings**: `uv_strerror` passes through (`name or service not known`, etc.), the same as `loop.fs`/`loop.net`;
- **reverse accepts only numeric addresses**: passing a hostname throws synchronously — it is not the inverse sugar of lookup, it is `getnameinfo`'s direct vehicle.

## loop.os: system information (synchronous face)

`loop.os` is the loop's eighth block and its only **synchronous** face: it returns immediately and needs no `loop.run()` — it sits under the loop name because it also comes from libuv (Node calls this face `os`), not because it enters the loop:

```lua
local os = require("loop").os

print(os.hostname(), os.type())         -- coding	Linux
print(os.arch(), os.release():sub(1, 6))  -- x64	6.12.3
print(#os.cpus() .. " cpus, " .. os.availableParallelism() .. " usable")
local u = os.userInfo()
print(u.username, u.uid, u.homedir)     -- cui	1000	/home/cui
print(os.EOL == "\n")                   -- true (POSIX platform face)
for name, addrs in pairs(os.networkInterfaces()) do
    for _, a in ipairs(addrs) do
        print(name, a.family, a.address, a.internal)
    end
end
```

| Call | Returns |
| --- | --- |
| `os.home()` | The user home directory (`$HOME` or a passwd entry) |
| `os.tmpdir()` | The temp directory (`$TMPDIR` or `/tmp`) |
| `os.hostname()` | The hostname |
| `os.type()` | The kernel name (`"Linux"`) |
| `os.arch()` | CPU architecture, in Node's names: `"x64"` (x86_64/amd64), `"arm64"` (aarch64); otherwise the raw uname machine |
| `os.release()` | The kernel version string (`"6.12.3-luna1"` and so on) |
| `os.EOL` | The constant `"\n"` (POSIX platform face) |
| `os.userInfo()` | `{username=, uid=, gid=, shell=, homedir=}` — a passwd entry with Node's key names; `shell` is absent when there is no entry |
| `os.availableParallelism()` | The CPUs actually available to this process (affinity-aware, same name as Node ≥ 18) |
| `os.uptime()` | System uptime in seconds (number) |
| `os.loadavg()` | Three load values `{1min, 5min, 15min}` |
| `os.freemem()` / `os.totalmem()` | Free / total physical memory (bytes) |
| `os.cpus()` | One entry per logical CPU: `{model, speed, times={user, nice, sys, idle, irq}}`, times in milliseconds |
| `os.networkInterfaces()` | Keyed by interface name: an array of addresses per name, each `{address=, family="IPv4"/"IPv6", mac=, internal=}` — Node's shape |

Behavior rules:

- **synchronous, no callbacks, no loop**: these read system state and return immediately; errors (unavailable, etc.) are thrown rather than delivered by callback — the async contract belongs only to operations that would wait;
- **cpus' times are cumulative**: they accumulate since boot; to compute utilization, take the difference between two samples;
- **networkInterfaces' `internal`** marks loopback interfaces; `mac` is always the `xx:xx:xx:xx:xx:xx` shape (all-zero for virtual interfaces without a mac).

## loop.net: asynchronous sockets

`loop.net` is the loop's third block: stream sockets — a client/server pair × two transports (TCP / unix domain), under the same "error-first callback" convention.

The client side: connect gets a `sock`, then write, read, half-close, close:

```lua
local net = require("loop").net

net.connect("localhost", 8080, function(err, sock)
    assert(err == nil, err)
    sock:write("ping\n", function(e)
        assert(e == nil, e)
        sock:read(function(e2, chunk)
            if chunk then print(chunk) end
            sock:close()          -- an open socket keeps the loop alive; you must close it
        end)
    end)
end)

loop.run()
```

The server side: bind/listen errors **throw synchronously** (both steps return synchronously in libuv), and each incoming connection is handed to a **resident** connection callback — the same "keep the callback" semantics as `setInterval`:

```lua
local net = require("loop").net

local srv                      -- declare first, then assign: see the scope reminder below
srv = net.listen("127.0.0.1", 0, function(err, sock)  -- 0 = ephemeral port
    if err then print(err) return end
    sock:read(function(e, chunk)
        if chunk then sock:write(chunk) else sock:close() end
    end)                       -- an echo server
end)
print("listening on", srv:port())

loop.run()
```

| Call | Semantics |
| --- | --- |
| `net.connect(host, port, cb)` | Asynchronous DNS resolution + TCP connect; `cb(err, sock)` |
| `net.connectPipe(path, cb)` | unix domain stream socket; `cb(err, sock)` |
| `net.listen(host, port, onConn)` | Listens on TCP; `host` must be a **numeric address** (`"0.0.0.0"` = all interfaces), `port 0` = ephemeral; returns a server |
| `net.listenPipe(path, onConn)` | Listens on unix domain; the path must not exist (it is unlinked first); closing **does not** remove the path |
| `server:port()` | The actually bound port (to read back an ephemeral port) |
| `server:address()` | The listen address `{address = "127.0.0.1", port = N, family = "inet"}`; a pipe server is `{address = path, family = "unix"}` |
| `server:close(cb?)` | Idempotent close; the callback lands after the handle truly closes |
| `sock:write(data, cb(err)?)` | Writes the payload; the callback fires on delivery (the implementation holds the payload until then) |
| `sock:read(cb)` | Streamed read: each block `cb(nil, chunk)`; peer EOF is `cb(nil, nil)`; an error is `cb(err)`; calling again swaps the callback |
| `sock:shutdown(cb(err)?)` | Half-close (FIN): the peer sees EOF, while this side can keep reading; not named `end` because it is a Lua keyword (`sock:end()` will not parse) |
| `sock:peer()` / `sock:sockname()` | Peer / local address, shaped like `server:address()`; must be called while the socket is alive |
| `sock:close()` | Idempotent close; an open socket or server keeps `run()` going — like Node, you must close it when done |

Behavior rules:

- **connection failure goes through the callback**: a DNS failure or refused connect comes out of `cb(err)`, the failed socket cleans itself up, and the loop drains to a natural return as usual;
- **EOF is `(nil, nil)`**: the peer closing makes the next `read` callback receive `err=nil, chunk=nil` — judge the end by `chunk == nil`, matching Node's stream end;
- **half-close**: `shutdown()` closes only the write side, leaving the read side open — request/response protocols use it to say "I'm done sending";
- **the connection callback is resident**: `onConn(err, sock)` is delivered once per connection and the reference is held by the implementation; an accept error (such as fd exhaustion) also comes out of its `err` and does not topple the loop;
- **a callback error does not topple the loop, but the connection belongs to the callback**: `error()` inside the callback only goes to stderr, and time keeps moving (timers continue); the `sock` already handed to the callback is still the callback's responsibility — throwing without `close()` makes `run()` wait forever (the same ownership as an uncaught exception's socket in Node);
- **errors are strings**: same as `loop.fs`, `uv_strerror` passes through (`connection refused`, `address already in use`, etc.);
- **the address face reads synchronously**: `peer()`/`sockname()`/`address()` read the kernel directly, not inside a callback; TCP's `family` is `inet`/`inet6`, a pipe is `unix` with no `port`; a pipe peer that has not explicitly bound its own path has `peer().address` as an empty string (the OS's anonymous semantics); calling them on a closed sock throws;
- **the Lua scope reminder**: in `local srv = net.listen(..., function() ... srv ... end)`, the `srv` the callback sees is **global** nil — a local variable does not enter scope until its declaration statement ends, and the callback is written inside that statement; split the handle into `local srv` + `srv = net.listen(...)` on two lines.

## loop.net: TLS (connectTls / listenTls)

The net face's TLS pair: client `connectTls` and server `listenTls`. Connection, certificate validation, and handshake all run through the event loop, and the `sock` delivered after the handshake uses the same `write` / `read` / `close` contract as an ordinary TCP socket — payload encryption happens in the implementation, and the error-first callback convention is unchanged:

```lua
local net = require("loop").net

net.connectTls("example.com", 443, function(err, sock)
    assert(err == nil, err)          -- certificate/handshake failure comes out here
    sock:write("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n", function(e)
        assert(e == nil, e)
        sock:read(function(e2, chunk)
            if chunk then io.write(chunk) end
            sock:close()
        end)
    end)
end)

loop.run()
```

The server side is isomorphic to `net.listen`: opts carry `cert` / `key` file paths, and each handshake-completed connection is handed to a resident `onConn` callback:

```lua
local net = require("loop").net

local srv
srv = net.listenTls("127.0.0.1", 0, {   -- 0 = ephemeral port
    cert = "server-cert.pem",
    key = "server-key.pem",
}, function(err, sock)
    assert(err == nil, err)
    sock:read(function(e, chunk)
        if chunk then sock:write(chunk) else sock:close() end
    end)                                -- a TLS echo server
end)
print("listening on", srv:port())

loop.run()
```

| Call | Semantics |
| --- | --- |
| `net.connectTls(host, port, opts?, cb)` | Async resolution + TCP connect + TLS handshake; `cb(err, sock)`; opts may be omitted |
| `net.listenTls(host, port, opts, onConn)` | Listens on TCP + TLS; opts' `cert`/`key` are required; returns a server (`port` / `address` / `close` / `unref` / `ref` same as the net face) |
| `sock:write(data, cb(err)?)` | Encrypted write, callback on delivery; a payload sent before the handshake finishes is held and flushed automatically once the handshake completes |
| `sock:read(cb)` | Decrypted plaintext delivered in blocks; peer close_notify is `cb(nil, nil)`, and the connection wraps up right after |
| `sock:peer()` / `sock:sockname()` | Same shape as the net face (bare fd reads the kernel); must be called while the socket is alive |
| `sock:close(cb?)` / `sock:unref()` / `sock:ref()` | Same semantics as the net face |

Behavior rules:

- **certificate validation is on by default**: the client checks against the system CA store plus the hostname (SNI is sent with the host); a failed check (self-signed, wrong hostname, expired) fails the handshake and the error comes out of `cb(err)`;
- **opts customize validation**: `insecure = true` skips validation (for self-signed dev servers); `ca = "path"` substitutes a specific CA file for the system store; a server-side `cert`/`key` load failure **throws synchronously** (the same as a listen bind failure);
- **a failure wraps up immediately**: a client-side resolution, connect, or handshake failure calls `cb(err, nil)` once and the half-built socket cleans itself up; **a server-side handshake failure is silently dropped**, and `onConn` only receives peers that completed the handshake;
- **EOF ends it**: after the peer sends close_notify, `read` gets `(nil, nil)` and the whole connection closes (unlike TCP's half-close, the TLS face has no `shutdown`);
- **closing a listener does not extend to existing connections**: `server:close` only stops listening; already-established connections each continue;
- **build dependency**: available when luna is built with OpenSSL; without it both entry points report how to enable it, and the rest of the loop face is unaffected.

## loop.http: asynchronous HTTP client and server

`loop.http` is a pure-Lua HTTP/1.1 client (`require "loop.http"`, same name as Node's `require("http")`), built on `net.connect` / `connectTls`: one request, one aggregate callback — `cb(err, res)`, where `res = {status, headers (lowercase keys), body}`; large responses can be turned into streaming observation via `onData`/`onHead`:

```lua
local http = require("loop.http")

http.get("https://example.com/", {ca = "ca.pem"}, function(err, res)
    assert(err == nil, err)
    print(res.status, res.headers["content-type"])
    print(#res.body, "bytes")
end)

loop.run()
```

| Call | Semantics |
| --- | --- |
| `http.request(url, opts?, cb)` / `http.request(opts_table, cb)` | The general entry point; opts_table may carry `url`/`method`/`headers`/`body`/`insecure`/`ca` |
| `http.get(url, opts?, cb)` | GET shorthand |
| `opts.maxRedirects` | The follow limit, default 5; `0` disables following (the raw 3xx is delivered as usual) |
| `opts.timeoutMs` | The whole request's time budget (shared across connect, each redirect hop, and body); a timeout gives `cb("... timed out ...")` |
| `opts.onHead(res)` | Called as soon as the response head lands (only the final response — redirect hops never pass through it); an error thrown inside delivers `cb` with that error |
| `opts.onData(chunk)` | **The streaming switch**: body is delivered in batches through it and `res.body` becomes an empty string; content-length and unframed bodies are truly incremental (no whole-body accumulation in memory), while chunked is still buffered internally and delivered whole once the terminating chunk lands; an error thrown inside delivers `cb` with that error |
| `res` | `{status = 200, reason = "OK", headers = {lowercase keys, multi-values joined by comma}, body = string}` |

The server side: `http.listen(host, port, handler)` returns a net server (`port()`/`address()`/`close()` all present), and the handler runs once **after the whole request lands**:

```lua
local http = require("loop.http")

local srv = http.listen("127.0.0.1", 8080, function(req, res)
    if req.method == "POST" then
        res.send(201, "got " .. #req.body .. " bytes",
                 {["X-Srv"] = "luna"})
    else
        res.send("hello " .. req.path)          -- default 200
    end
end)
print("listening on", srv:port())

loop.run()
```

| Server call | Semantics |
| --- | --- |
| `http.listen(host, port, handler, opts?)` | Starts the service, returns a net server; `port 0` = ephemeral; a non-function handler throws synchronously; `opts.max_body` (bytes, default 16 MiB) tunes the request-body ceiling |
| `req` | `{method, path (with query string), headers (lowercase keys, multi-values joined by comma), body (string)}` |
| `res.send(status?, body?, headers?)` | Three overloads: `send(body)`=200 / `send(status, body)` / `send(status, body, headers)`; closes the connection after writing the whole response; **idempotent** — a later call does nothing |

Behavior rules:

- **body framing has three paths**: read to `content-length` fullness, decoded as `chunked` (including the terminating chunk), or when neither is present, read to the peer's close — every request sends `Connection: close`, so the response is bounded by the connection ending;
- **streaming is a purely additive observation**: with `onData` set, each body byte is delivered through it exactly once and `res.body` is an empty string; framing and completion detection are unchanged; without it, everything aggregates as before; an error thrown in `onHead`/`onData` becomes a `cb(err)` that ends the request;
- **status codes with no body** (1xx/204/304) are delivered with an empty body;
- **redirects**: 301/302/303/307/308 with a `Location` are followed; absolute URLs, `//host/path`, and relative paths all resolve (`../` is reduced); 301/302/303 fold non-GET/HEAD methods into GET and drop the body, while 307/308 keep them as-is; cross-host following is unrestricted; an exhausted budget reports `too many redirects`;
- **the timeout is for the whole request**: it starts before connection and redirects do not reset it; on timeout the socket is closed first and then the error is delivered, so a late response or an already-queued connection callback can no longer reach the callback;
- **errors are strings**, coming out of `cb(err)` (refused connection, TLS failure, truncated body, bad framing, over-limit, timeout); a URL-parse failure and a missing callback **throw synchronously**;
- **https certificate validation is on by default**, and `opts.insecure` / `opts.ca` pass through to `connectTls`;
- **the server is one connection per request**: the same `Connection: close` semantics as its own client — the response closes right after being written, with no keep-alive;
- **the server calls the handler only after the whole request lands**: the body is read to `content-length` fullness (chunked request bodies are out of scope); a request line that does not match HTTP/1.x or headers over 64 KiB gets a 400 and a close;
- **the server caps the request body**: a declared `content-length` or a streaming body over `opts.max_body` (default 16 MiB) gets a 413 and a close — content-length is the peer's self-report, so without a cap you let the peer pull memory along;
- **a handler error**: if the response has not been sent at that point, the implementation sends a 500; an already-`send` connection is unaffected;
- **a handler that does not `send` leaves the connection hanging**: there is no automatic send — forgetting to reply means hanging until the peer times out;
- **boundaries**: this face does not do multiplexing or infinite streams (SSE, for example — the aggregate model would never see completion; for large file downloads use `onData` streaming; for bidirectional long-lived connections use the net face directly); IPv6 literal hosts are not yet supported; the passed opts table is never modified (redirect folding happens on an internal copy); and it is independent of the stdlib synchronous `http` (luasocket backend) — use the blocking one-shot for that, and `loop.http` for in-loop concurrency.

## loop.udp: asynchronous datagrams

`loop.udp` is the loop's fourth block: the datagram face — connectionless, no ordering guarantee, no delivery guarantee — with one bound end receiving resident packets and one socket end sending per-packet, under the same "error-first callback" convention:

```lua
local udp = require("loop").udp

local r                       -- split declaration: read r inside the callback (see the net scope reminder)
r = udp.bind("0.0.0.0", 0, function(err, data, rinfo)   -- 0 = ephemeral port
    if err then print(err) return end
    print(data, rinfo.addr, rinfo.port)
    r:close()
end)
print("listening on", r:port())

local s = udp.socket()        -- unbound: first send auto-binds an ephemeral port
s:send("ping", "127.0.0.1", r:port(), function(err)
    assert(err == nil, err)
    s:close()
end)

loop.run()
```

| Call | Semantics |
| --- | --- |
| `udp.bind(host, port, onMsg)` | Binds a numeric address (`"0.0.0.0"` = all interfaces), `port 0` = ephemeral (read back via `sock:port()`); each datagram delivers once to `onMsg(nil, data, rinfo)`, with `rinfo = {addr=, port=}` |
| `udp.socket()` | An unbound sending end; libuv auto-binds an ephemeral port on the first `send` |
| `sock:send(data, host, port, cb(err)?)` | Sends one datagram (`host` must be a numeric address); the callback is delivered once per packet and the implementation holds the payload until then |
| `sock:port()` | The actually bound port (to read back an ephemeral port) |
| `sock:sockname()` | Its own address `{address, port, family}` — the bound end reports the bound address; the sender reports the ephemeral port libuv chose after the first `send` (the wildcard address `0.0.0.0`) |
| `sock:close()` | Idempotent close; an open sock keeps the loop alive — close it when done |

Behavior rules:

- **a bind conflict throws synchronously**: when the port is taken, `bind` raises directly — the same as `listen`;
- **the message callback is resident**: the reference is held by the implementation (the receive-side `onConn`); **a receive error also comes out of its `err`**, and the socket is not torn down by it;
- **UDP does not guarantee delivery**: on loopback a packet arrives immediately, but across a network, loss, reordering, and duplication are normal — retries and dedup are protocol-layer concerns and `loop.udp` does not do them for you;
- **send errors go through the callback**: an unreachable destination or a bad address comes out of `cb(err)`, and the failed packet does not bring down the loop.

## loop.process: asynchronous subprocesses

`loop.process` is the loop's fifth block: it opens a shell, starts a child process, collects its stdout/stderr into memory, and delivers once **when it exits and both pipes have drained**; `exec` is its shell sugar (`run("sh", {"-c", cmd})` passed through, Node `exec` semantics); `execSync` is the synchronous face, blocking until the command finishes; `spawn` hands the three stdio streams directly to ordinary `loop.net` socks for streaming send/receive:

```lua
local loop = require("loop")
local process = loop.process

process.run("sh", {"-c", "echo hi; exit 0"}, function(err, res)
    assert(err == nil, err)
    print(res.status, res.stdout)   -- 0	hi
end)

process.exec("echo hi | tr a-z A-Z", function(err, res)  -- shell-semantics sugar
    print(res.stdout)               -- HI
end)

loop.run()

print(process.execSync("date +%F"))  -- synchronous blocking, returns stdout; a nonzero exit throws
```

`run` **returns immediately** with a proc handle (no need to wait for the child): `p:pid()` is readable at any time; `p:kill(sig?)` defaults to SIGTERM — a killed child still runs its exit callback, `res.signal` is the signal number, and `status` is meaningless then (the libuv convention).

| Call | Semantics |
| --- | --- |
| `process.run(cmd, args?, opts?, cb)` | Runs `cmd` without a shell; `args` is a string table for the argv tail; `opts` accepts only `{cwd = path}`; stdin is ignored; returns a proc handle immediately |
| `process.exec(cmd, opts?, cb)` | `run("sh", {"-c", cmd}, opts, cb)` passed through verbatim: the command line goes to the shell for interpretation; the aggregation/delivery contract is identical to `run`; `opts` likewise accepts only `{cwd = ...}` |
| `process.execSync(cmd) -> stdout` | Synchronous execution (via `popen`), blocking until the command ends and returning all stdout; a nonzero exit code (or death by signal) throws — Node `execSync`'s shape |
| `proc:pid()` | The child pid |
| `proc:kill(sig?)` | Sends a signal (default 15/SIGTERM); the exit callback still lands; there is no idempotency — killing an already-exited process throws |
| `res.status` | The exit code (meaningless when `signal` is non-nil) |
| `res.signal` | The terminating signal number; nil for a normal exit |
| `res.stdout` / `res.stderr` | The two captured outputs (strings, possibly empty) |

Behavior rules:

- **a spawn failure throws synchronously**: an exec failure such as a missing command is returned synchronously by libuv in `uv_spawn`, so `run` raises directly — the same as a `listen` bind error; after `pcall` catches it the loop drains as usual, leaving no dangling handle;
- **the delivery condition is "exited + both pipes EOF"**: when the child dies, the kernel necessarily closes its fds, so the aggregate callback arrives deterministically and output is not truncated by unflushed buffers;
- **`run` has no shell**: `cmd` does not go through `/bin/sh`, and pipes, globs, and `~` are never expanded — for shell semantics use `exec` (or `run("sh", {"-c", "..."})` yourself), matching the Node `spawn`/`exec` split;
- **`execSync` is the synchronous face**: it never enters the event loop and blocks until the child ends — a long command stalls the calling thread, and like Node `execSync` it is the "know what you are doing" interface; stderr beyond stdout is not captured, and the exit code comes back via the close status, throwing on nonzero;
- **stdin is ignored**: `run`/`exec`/`execSync` do not attach to the child's stdin — to write, use `process.spawn` (next section), where all three stdio streams are ordinary socks;
- **errors are strings**: same as `loop.fs`/`loop.net`, `uv_strerror` passes through.

## process.spawn: streaming subprocess

`spawn` is the streaming opposite of aggregation: the three stdio streams are just three ordinary `loop.net` socks — the full `write`/`read`/`shutdown`/`close` semantics apply unchanged, the read callback likewise streams multiple blocks as `cb(nil, chunk)` and `cb(nil, nil)` is EOF:

```lua
local process = require("loop").process

local p                       -- split declaration: read p inside the callback (see the net scope reminder)
p = process.spawn("cat", {}, function(err, res)
    print("exit", res.status)                 -- delivered on exit, does not wait for streams
end)
p:stdout():read(function(e, chunk)
    if chunk then io.write(chunk) else p:stdout():close() end
end)
p:stdin():write("ping", function(e2)
    p:stdin():shutdown(function() p:stdin():close() end)  -- half-close says "I'm done"
end)
p:stderr():close()            -- an unread stream must also be closed

loop.run()
```

| Call | Semantics |
| --- | --- |
| `process.spawn(cmd, args?, opts?, cb)` | Same arguments as `run`; returns a proc handle immediately; the exit callback `cb(nil, res)` carries only `status`/`signal`, no captured output |
| `proc:stdin()` / `proc:stdout()` / `proc:stderr()` | The sock for each of the three stdio streams; **returns nil after the exit callback has been delivered** |

Behavior rules:

- **onExit aligns with Node's `'exit'`**: the child's exit is delivered immediately, **without waiting** for stdout/stderr to drain — an unread pipe never reaches EOF, and waiting on it would deadlock; after exit, the streams remain readable;
- **the streams are ordinary net socks**: apart from EOF there is no implicit wrap-up — close when done reading, and close an unread stream too (an open sock keeps the loop alive, the same "close when done" rule as `loop.net`);
- **handles are only available until delivery**: `p:stdout()` is already nil inside the exit callback — to use the sock in a callback, capture it into a local right after `spawn`: `local out = p:stdout()`;
- **the write side says done with `shutdown`**: a program like `cat` that exits on EOF needs a half-close (not `close`, which would throw away the read side too) to say "I'm done sending";
- **stdin/stdout/stderr are independent**: the three streams do not affect each other, and stderr's EOF does not affect stdout — reading them separately is exactly the point of `spawn` over `run`.
