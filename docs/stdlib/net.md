# stdlib · net

TCP/UDP sockets. Backed by [luasocket](https://github.com/lunarmodules/luasocket): a **synchronous blocking** model, with the full `socket` namespace still reachable via `require "socket"`; this module lifts the common factories up one layer. The async face of the event loop (`connect`/`listen`/`connectTls`/`listenTls`, Unix sockets) is in `loop.net` under [loop](/zh/guide/loop); the async server and TLS are there too. This page is the synchronous blocking face — one-shot clients, plus `net.serve`, a one-line blocking TCP service.

## API

| Call | Description |
| --- | --- |
| `net.tcp()` | A new TCP master socket (use with `:connect(host, port)`) |
| `net.connect(host, port)` | Direct connect, returns a client socket; on failure `nil, err` (such as `connection refused`) |
| `net.bind(address, port)` | Binds a master socket (`:listen()` + `:accept()`) |
| `net.serve(host, port, handler)` | One-line blocking TCP service: bind → accept loop, `handler(client)` called once per connection; a handler error goes to stderr and the service stands; returning `false` stops it; the actual listen address is announced on stderr (`port` 0 grabs an ephemeral port); accept carries a 250 ms deadline so the loop periodically turns — a pending `^C` stops the service with code 130, the deadline hangs only on the master, and the client a handler gets is unaffected by the timeout |
| `net.udp()` | A new UDP socket |
| `net.select(recvt, sendt[, timeout])` | Readiness polling: first arg waits for **readable**, second for **writable** (luasocket convention); returns two ready arrays |
| `net.dns` | The DNS face (`resolve`/`toip` …) |

Socket objects follow luasocket semantics:

| Method | Description |
| --- | --- |
| `s:send(str)` | Sends; returns the byte count (such as `12.0`), on failure `nil, err` |
| `s:receive("*l" \| "*a" \| n)` | Receives a line / until the connection closes / n bytes; on failure `nil, err` (`timeout`/`closed`) |
| `s:settimeout(sec)` | Send/receive timeout in seconds; unset means blocking |
| `s:getsockname()` | Local `ip, port` |
| `s:close()` | Closes |

**Error shape** (luasocket convention): failure is always `nil, err`, and the error is a string — an unreachable `net.connect` address returns `nil	connection refused`, and a timed-out `receive` returns `nil, "timeout"`. Judge with `err ~= nil`.

The UDP face in one pass (`setsockname` binds, `sendto`/`receivefrom` send/receive; connectionless, `receivefrom` also returns the peer address):

```lua
local net = require "net"
local u = net.udp()
u:setsockname("127.0.0.1", 0)          -- 0 = the OS picks a free port
local _, port = u:getsockname()
print(u:sendto("ping", "127.0.0.1", port))
print(u:receivefrom())
u:close()
```

```text
4.0
ping	127.0.0.1	42618
```

## Usage

One loopback round-trip: connect to a local [http](/zh/stdlib/http) service and grab a response header, the most common script posture —

```lua
local net = require "net"
local s = net.connect("127.0.0.1", 8000)
s:send("GET / HTTP/1.0\r\nHost: localhost\r\n\r\n")
local status = s:receive("*l")
print(status:find("200 OK", 1, true) ~= nil)
s:close()
```

```text
true
```

### One-line TCP service

Terminal 1 starts an echo service (`^C` stops it):

```lua
local net = require "net"
net.serve("*", 9000, function(c)
  c:send("echo: " .. (c:receive("*l") or "") .. "\n")
end)
```

```text
net.serve: listening on 0.0.0.0:9000
```

Terminal 2 connects and talks:

```lua
local net = require "net"
local c = net.connect("127.0.0.1", 9000)
c:send("hello\n")
print(c:receive("*l"))
c:close()
```

```text
echo: hello
```

The handler gets a raw luasocket client (all methods above available), and whether to close it is the handler's call; a handler error only goes to stderr and the service keeps serving; returning `false` wraps up cleanly. For async concurrency or a TLS server, use `net.listen`/`net.listenTls` in [loop](/zh/guide/loop); for HTTP directly, [http.serve](/zh/stdlib/http).

DNS resolution:

```lua
local net = require "net"
print((net.dns.toip("localhost")))
```

```text
127.0.0.1
```

(`toip` actually returns two values — the IP string and a resolution detail table; the parentheses take the first.)

## Boundary rules

- **Blocking calls**: `receive` with no data just hangs — inside the event loop use `loop.net`; this page is for synchronous scripts.
- The default timeout follows luasocket's global (blocking); a careful script sets `:settimeout(5)` explicitly.

