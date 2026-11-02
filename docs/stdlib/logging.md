# stdlib · logging

The [lualogging](https://github.com/lunarmodules/lualogging) (lunarmodules, 1.8.2, MIT) `logging` family ships with the binary and is `require`-ready: six-level thresholds plus log4j-style appenders. The core is pure Lua, passed through verbatim and unwrapped; `logging.socket`/`logging.email` sit on the bundled luasocket, while `sql`'s database drivers and the `rsyslog`/`nginx` hosts are optional and only require'd when used. luna's upstream CI on Lua 5.5 is not yet covered (the matrix stops at 5.4); each file was verified locally against the upstream regression: env/console/file/rolling/socket/SQL all pass, generic 10/11 — the single red, `format_error_stacktrace`, hardcodes the host stack depth (it wants 3, gets 4 under luna's runner and 2 on stock) — a pure test-shape sensitivity, not a semantic difference; zero patches and zero changes; mail needs an external SMTP and was not run locally.

## API

| Call | Description |
| --- | --- |
| `require "logging"` | Core: the six constants `DEBUG/INFO/WARN/ERROR/FATAL/OFF`, `logging.new(append, level)` for a custom appender, `defaultLevel()`/`defaultLogger()` manage the global defaults |
| `require "logging.console"` | Console appender; `{ logLevel=, destination="stdout"\|"stderr", logPattern=, timestampPattern= }` |
| `require "logging.file"` | Append-write file; `{ filename=, logLevel=, datePattern=, logPattern=, timestampPattern= }` |
| `require "logging.rolling_file"` | Rolls on `maxFileSize`, keeps `maxBackupIndex` archives; other params same as file |
| `require "logging.socket"` | Each message goes over luasocket to a TCP peer at `{ hostname=, port= }` |
| logger methods | `:debug/:info/:warn/:error/:fatal(msg, ...)`, arguments expanded via `string.format`; `:setLevel()`, `:getPrint()` |

## Levels

Records below `logLevel` are silently dropped; default DEBUG; message arguments go through `string.format`:

```lua
local logging = require "logging"
local log = require "logging.console"({ logLevel = logging.INFO })
log:debug("this one is filtered out")
log:info("up and running")
log:error("boom: %s", "disk full")
```

```text
Sat Oct  3 11:54:35 2026 INFO up and running
Sat Oct  3 11:54:35 2026 ERROR boom: disk full
```

## Format

`logPattern` defaults to `%date %level %message\n`, and `%date`/`%level`/`%message`/`%source` (the call point's filename:line and function name) can be freely combined; `timestampPattern` goes through `os.date`:

```lua
local logging = require "logging"
local log = require "logging.console"({
  logLevel = logging.DEBUG,
  logPattern = "%date [%level] %message\n",
})
log:warn("cache cold")
```

```text
Sat Oct  3 11:54:35 2026 [WARN] cache cold
```

## File

`logging.file` appends, line-buffered; below `logLevel` it likewise does not touch disk — `app.log` has only that one WARN:

```lua
local logging = require "logging"
local log = require "logging.file"({ filename = "app.log", logLevel = logging.WARN })
log:info("not written (below WARN)")
log:warn("written to app.log")
```

```text
Sat Oct  3 11:54:35 2026 WARN written to app.log
```

## Rolling file

`maxFileSize` is in bytes, checked before each write: on exceed it rolls — `server.log` is renamed to `server.log.1`, older archives push up, at most `maxBackupIndex` kept, and the main file reopens:

```lua
local logging = require "logging"
local log = require "logging.rolling_file"({
  filename = "server.log", maxFileSize = 200, maxBackupIndex = 1,
  logLevel = logging.INFO,
})
for i = 1, 8 do log:info("request %d handled", i) end
```

After 8 entries (each ~49 bytes): the first 5 are squeezed into `server.log.1` by the 200-byte cap, and the main file restarts at entry 6 —

```text
server.log
Sat Oct  3 11:54:35 2026 INFO request 6 handled
Sat Oct  3 11:54:35 2026 INFO request 7 handled
Sat Oct  3 11:54:35 2026 INFO request 8 handled
```

A formatting failure does not drop the message: when `string.format` errors, that entry still goes to the appender as `Error formatting log message: … | stack trace`. The `email`/`sql`/`rsyslog`/`nginx` appenders also sit under the `logging.` name, requiring respectively a reachable SMTP, database drivers, and the copas/ngx hosts — upstream options; the four luna guarantees are console/file/rolling_file/socket.

