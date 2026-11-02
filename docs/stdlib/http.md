# stdlib · http

HTTP's "one-line service" face: `http.serve()` compresses starting a service into a single call — static directory or programmable handler, both modes from one entry point. The client face is luasocket's `socket.http` re-exported verbatim (`request()` and so on); the async client of the event loop (redirects, timeouts, streaming, TLS) is in `loop.http`, see [loop](/guide/loop).

## serve: two modes, one call

| Call | Mode |
| --- | --- |
| `http.serve()` | Static **current directory**, port 8000 |
| `http.serve("./docs")` | Static directory, port 8000 |
| `http.serve("./docs", 9000)` | Static directory, explicit port |
| `http.serve(handler)` | Programmable handler, port 8000 |
| `http.serve(handler, 9000)` | Programmable handler, explicit port |
| `http.serve{dir = "...", handler = fn, port = 9000, host = "127.0.0.1", quiet = true}` | Full option form |

Defaults: same as `python -m http.server` — directory `"."`, port **8000**, host **0.0.0.0** (the banner shows localhost); when `dir` and `handler` are both given, **handler wins**.

Returns a service object:

| Field/method | Description |
| --- | --- |
| `srv.port` | The real port (read back after passing 0 for a random one) |
| `srv.url` | `"http://localhost:8000/"` shape |
| `srv.dir` / `srv.host` | Static mode's root (absolutized) / the listen host |
| `srv:address()` | `{address = "0.0.0.0", family = "inet", port = 8000}` |
| `srv:close(cb?)` | Clears keep-alive and closes the listener; the event loop then wraps up normally |

The startup banner goes to **stderr** (so pipes stay clean); `quiet` turns it off.

**Error shape** (thrown, two messages): a missing directory → `http.serve: no such directory: /nope-dir-404`; a port in use → `http.serve: cannot listen on 0.0.0.0:8000 (listen failed: address already in use)` — the parenthetical is loop.net's original reason.

CLI fast path: `luna serve [dir] [port]` (the `http.serveCli(argv)` entry point, returns an exit code) starts a static service with zero scripting — the directory and port arguments are order-independent (`luna serve 9000` = current directory, port 9000). Argument surface: `-h`/`--help` prints usage (exit 0); an unknown switch or a doubled port → usage + exit **2**; a missing directory → exit **1**; the port must be an integer 1–65535. `^C` stops the service, exit **130**.

## Static mode

The route is a decision chain; each step's exit:

| Order | Condition | Response |
| --- | --- | --- |
| 1 | Method not GET/HEAD | `405` + `Allow: GET, HEAD` |
| 2 | Path contains a NUL byte | `400` |
| 3 | Any `..` segment (any encoding wrapping) | `403`, body carries the original path |
| 4 | Attributes unavailable | `404`, body carries the original path |
| 5a | Directory, URL without trailing slash | `301` + `Location: <path>/` |
| 5b | Directory, has `index.html` | Serves the index file |
| 5c | Directory, no index | HTML directory listing |
| 6 | File | Content-Type by extension, serves the file |

One measured loop (served from `./docs`, real output):

```text
HTTP/1.1 200 OK                        ← GET /a.txt
content-type: text/plain; charset=utf-8
Content-Length: 10

hello luna

HTTP/1.1 301 Moved Permanently         ← GET /sub (bare directory)
location: /sub/
Content-Length: 0

HTTP/1.1 200 OK                        ← HEAD /a.txt (empty body, type still sent)
Content-Length: 0

404 Not Found: /nope.txt               ← GET /nope.txt
405 Method Not Allowed: POST           ← POST /a.txt (allow: GET, HEAD)
403 Forbidden: /../etc/passwd          ← GET /../etc/passwd
```

The directory listing body (`GET /sub/`): directories first (with a `/` suffix), case-insensitive byte order, a `../` up-link on non-root paths, and the `luna serve` footer:

```html
<!doctype html><html><head><meta charset="utf-8">
<title>Index of /sub/</title></head><body>
<h1>Index of /sub/</h1><hr><ul>
<li><a href="../">../</a></li>
</ul><hr>luna serve</body></html>
```

Content-Type by extension (html/css/js/json/md/txt/lua/yaml/svg/png/jpg/gif/webp/ico/woff/woff2/ttf/pdf/wasm/zip/gz/mp3/mp4/webm/csv/xml/map, etc., text types with `charset=utf-8`), unknown type `application/octet-stream`.

## Programmable mode

The handler receives `(req, res)`:

| Side | Field/method | Description |
| --- | --- | --- |
| req | `method` / `url` | Method / raw target (**including** the query string) |
| req | `path` / `query` | Path without the query string / decoded query table |
| req | `headers` | Request header table, **all keys lowercase** |
| req | `body` | Request body string, possibly empty |
| res | `res.status = n` | Status code (default 200), set before sending |
| res | `res:setHeader(k, v)` | Custom header; stored lowercase, later writes override earlier ones |
| res | `res:send(body?)` | Text response, default `text/plain; charset=utf-8`; `send()` sends an empty body |
| res | `res:json(value, status?)` | JSON response (`application/json; charset=utf-8`); **the second arg sets the status directly**, one line less than assigning `res.status` first |

`query` decode convention: `%XX` decoded, `+` in the query string counts as a space, a bare flag with no `=` decodes to an empty string, and duplicate keys **last wins** (`?n=1&n=2` gives `{n = "2"}`).

`setHeader("Content-Type", ...)` overrides the default type of `send`/`json` — header keys appear lowercase in the response:

```lua
local http = require "http"
http.serve(function(req, res)
  if req.path == "/api" then
    res:json({ hello = "luna", youAsked = req.query.q })
  else
    res.status = 404
    res:json({ err = req.path .. " not found" })
  end
end, 9000)
```

```bash
$ curl 'http://localhost:9000/api?q=hi'
{"youAsked":"hi","hello":"luna"}
$ curl http://localhost:9000/other
{"err":"/other not found"}
```

Key order follows `pairs`, per runtime — don't assert JSON object key order.

A handler error with nothing sent yet hands the caller a **500** (empty body) — a service API need not write its own fallback:

```text
HTTP/1.1 500 Internal Server Error
Content-Length: 0
```

## Service and exit contract

- The service is a **live handle**: the listener plus an internal heartbeat together hold the event loop open, and script-tail auto-drain (the `maybeDrain` contract in [loop](/guide/loop)) does not end the process while the service is alive — a server is meant to keep running;
- `^C` lands on `loop.run()`'s `interrupted` error: both `luna serve` and a `luna -e` that starts a service respond immediately, exit **130**;
- After `srv:close()` the heartbeat is cleared, the loop drains, and the process wraps up normally — use this for tests and background wrap-up, not `^C`.

```lua
-- test posture: start on a random port, close to wrap up
local http = require "http"
local srv = http.serve{ handler = function(req, res) res:send("ok") end,
                        port = 0, quiet = true }
print(srv.port > 0, srv.url:find(":" .. srv.port .. "/", 1, true) ~= nil)
srv:close()
```

```text
true	true
```

## Client face (luasocket)

`require "http"` also re-exports all of luasocket's `socket.http` (`request` and its option fields). Two postures:

```lua
local h = require "socket.http"     -- or require "http", same table
-- simple form: returns (body, code, headers, status_line), all four values
local body, code, headers, line = h.request("http://127.0.0.1:8000/a.txt")
print(body, code, line)             -- hello luna  200  HTTP/1.1 200 OK
-- general form: an ltn12 sink of your own (POST, streaming, custom headers all go here)
```

On failure (connection refused, etc.) the simple form returns `nil, err`; a non-2xx is **not** a failure — `code` comes back with 404/500 and the body is still delivered; branch yourself.

**HTTPS is not in this face**: requesting an https address throws `module 'ssl.https' not found` (luasocket's https branch needs LuaSec, which luna does not bundle) — for a TLS client use `loop.http`'s `connectTls` face.

