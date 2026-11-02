# Error model and exit codes

Node's errors are Error objects with a `code` field; luna deliberately simplifies: **errors are strings** (Lua convention), but **when to throw, when to return, and which stream** follow a uniform contract.

## Synchronous face: two error kinds, two appearances

The binary split that runs through the standard library ([standard library overview](/zh/stdlib/index)'s cross-cutting contract):

| | Data error | Argument error |
| --- | --- | --- |
| Example | `json.decode("{oops")` | `json.decode(42)` |
| Appearance | `nil, err` (message carries line and column) | **throws** (`luaL_error`) |
| Handling | Take the second return explicitly, or `pcall` | The caller's bug, let it crash |

Format-class modules (json/csv/ini/toml/yaml/xml) follow the first row. `fs` splits in half: the convenience layer (read file, list directory, etc., Node-style APIs) **throws** per Node style (message includes the reason), while the lfs passthrough face keeps lfs's own `nil, err`; `net`'s synchronous face passes through luasocket's `nil, err` style. Which module follows which style is in each module page's contract section — the rule is: **the wrapper layer learns Node, the passthrough layer keeps upstream**.

## Asynchronous face: error-first callback, strings straight out

The `loop` families (`loop.fs`/`loop.net`/`loop.dns`/`loop.process`…) follow Node's "error-first callback" style, but the error value is a **string** rather than an Error object — `uv_strerror` passes through directly (`no such file or directory`, `connection refused`, `address already in use`):

```lua
loop.fs.stat("/nope", function(err, st)
    if err then print(err) end   -- "no such file or directory"
end)
```

The check is always `err ~= nil`, no type dispatch needed.

**Callback isolation**: an error thrown in a callback only goes to stderr, and the **loop continues** — the same way the REPL isolates a single evaluation error (Node's uncaughtException behavior of not exiting). The cost is no error return: the callback owns its errors, and throwing while forgetting `close()` makes `run()` wait forever.

**The sync-throw vs async-delivery boundary**: operations that wait (connect, read file, resolve name) deliver errors through the callback; operations that complete instantly (bind/listen, `loop.os` system info) throw directly — a bind failure has no "later" to wait for.

## Uncaught errors: process level

- A script's main chunk ending in an error → the message goes to stderr (with a traceback) and the process exits;
- The `loop` script-tail auto-drain behaves the same: **a main-chunk error does not drain**, matching Node's direct exit on an uncaught exception;
- In REPL and attach sessions, an error in a single evaluation ends only that evaluation (attach errors are isolated by `pcall` and do not topple the target process).

## Typo hints

When a runtime error is annotated `(global 'x')` and `x` is indeed undefined, the line below the error suggests the closest global name by edit distance — `hint: did you mean 'print'?` (two candidates when ambiguous). Present in both the REPL and script mode.

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Normal end (including a completed script-tail drain) |
| `1` | Script/`-e` evaluation ended in an error; a file could not be opened; the attach target went away |
| `130` | `^C` interrupt (128 + SIGINT) — the same convention in script, `-e`, and event-loop scenarios; `luna serve` on `^C` likewise |

The discriminator is the `interrupted` text in the error message (ordinary errors such as an incomplete `-e` chunk or a REPL evaluation error are unaffected). The shell side can branch by exit code like any Unix tool.

## Errors on the attach channel

`luna --attach` frames its replies with `\30`: an `OK` frame goes to stdout and an `ERR` frame (full traceback) goes to stderr — the pipe carries a clean result stream; when the target crashes the client gets `attach: target went away`. See [CLI and REPL](/zh/guide/cli-repl#attach-attach-to-a-running-luna).

