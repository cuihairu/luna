# The kernel global

`kernel` is a pre-registered **global** — no `require` needed, and `require "kernel"` returns the same table (the counterpart to `process` in Node: a single entry point for process state, output funnel, and interrupt control). It is the only policy base in the runtime written in C; the REPL, completion, and plugins all sit on top of it.

## API overview

| Call | Semantics |
| --- | --- |
| `kernel.exec(src, chunkname?, ...args)` | Evaluates a chunk of code; `args` become the chunk's `...` |
| `kernel.check(chunk)` | Parses only, does not evaluate: returns `"ok" \| "incomplete" \| "error", msg` |
| `kernel.request_interrupt()` | Sets the pending `^C` flag from Lua (symmetric with `clear_interrupt`) |
| `kernel.write(...)` | Writes each argument (must be a string) into the output funnel |
| `kernel.sink(fn?)` | Redirects output to a Lua function; `nil` restores stdout |
| `kernel.clear_interrupt()` | Resets the pending `^C` flag |
| `kernel.pid()` | The current process pid (the attach socket path derives from it) |
| `kernel.wake(pid)` | Sends `SIGUSR1` to the target process, waking its polling point |
| `kernel.alive(pid)` | Process liveness probe: `kill(pid, 0)`; EPERM counts as alive, ESRCH as `false`; always `false` on Windows |
| `kernel.chmod(path, "600")` | Sets permissions from an octal string (a supplement for lfs, which lacks chmod) |
| `kernel.umask(mask?)` | Sets umask and returns the old value; defaults to `0077` |
| `kernel.millis()` | Monotonic clock in milliseconds (integer, `CLOCK_MONOTONIC`) |
| `kernel.started` | The monotonic-millisecond anchor at `luaopen`; `kernel.millis() - kernel.started` is process uptime |
| `kernel.tty()` | Whether stdin is a terminal |
| `kernel.colors()` | Whether color is available (TTY + `TERM` not `dumb` + no NO_COLOR) |
| `kernel.version()` | `"luna 0.1.0"` |
| `kernel.fuel(strikes?)` | Sets the instruction budget (count-hook strike count); no argument reads the current value, and an unset value returns `nil` |
| `kernel.memcap(bytes?)` | Sets the memory byte ceiling; no argument reads the current value, and an unset value returns `nil` |

## Execution and validation

- **`kernel.exec(src, chunkname?, ...args)`**: compiles and runs `src`. `chunkname` feeds the traceback (the convention is `"@" .. path` for real files; the REPL uses `"repl"`/`"repl[N]"`, and `-e` uses `"eval"`); the remaining arguments become the chunk's `...` — `run_script` relies on this to hand command-line arguments to a script. Returns `ok, err`-style: both compile and runtime errors come back as the second return value, never thrown.
- **`kernel.check(chunk)`**: runs only the compiler, separating "still needs the next line" (`"incomplete"` — ending with an operator / open bracket / bare `return` where the parser explicitly demands continuation) from "a real error" (`"error", msg`). The REPL's multi-line continuation decision is built on top of it; if you want to build your own interactive surface at the input layer, this is the entry point.

## Output funnel

All kernel output converges through one `emit()`:

- **`kernel.write(...)`**: strings (a non-string throws an argument error) are written to the funnel verbatim — no separator, no newline;
- **`kernel.sink(fn)`**: once registered, each output segment is redirected to `fn(str)` (order preserved; this is the output capture for tests and plugins); `kernel.sink(nil)` restores stdout;
- The built-in **`print` has been taken over** to the same funnel (the standard print format contract is unchanged: tab-separated, trailing newline) — so `kernel.sink` also captures `print`, which is what makes attach's output capture and the REPL's session output work.

## Interrupts and processes

- **`kernel.clear_interrupt()`**: called at the return to the prompt — a `^C` that arrives while idle is discarded (bash semantics), not left for the next chunk;
- **`kernel.pid()` / `kernel.wake(pid)`**: attach's primitive pair — the socket filename takes the pid, and the client sends `SIGUSR1` to make the target's blocked line edit return an empty line, letting the main loop poll;
- **`kernel.alive(pid)`**: `luna ps`'s liveness check (`ps` scans for socket files then probes each pid; a dead process leaves only a file, which is marked `stale`).

## Permissions

- **`kernel.chmod(path, "600")`**: luafilesystem has no chmod, while the attach socket must be owner-only from the first moment — this is the supplement; the mode must be an octal string;
- **`kernel.umask(mask?)`**: `serve.start` tightens umask before binding and restores it afterward, relying on this "return old value" face.

## Clock and environment

- **`kernel.millis()`**: monotonic clock, unaffected by system time sync; `%timeit` and `loop.now()` (libuv's own clock) each have their uses — don't mix them; process uptime = `kernel.millis() - kernel.started` (the uptime `%info`/`%stats` report is this formula);
- **`kernel.tty()`** inspects stdin, and **`kernel.colors()`** inspects stdout plus environment variables (`NO_COLOR`/`LUNA_NO_COLOR` appearing turns it off, the [no-color.org](https://no-color.org) contract; `LUNA_COLOR=1/0` forces on/off) — the REPL's color switches all defer to it;
- **`kernel.version()`**: the version string, sourced from the same place as `luna --help`'s title.

## Limits

- **`kernel.fuel(strikes?)`**: the instruction budget, in count-hook strike units (one strike ≈ 100,000 instructions); the hook first checks the `^C` flag, then deducts the budget, and hitting zero raises `error("sandbox: instruction budget exhausted")`. Setting 0 is equivalent to unset; calling with no argument reads the current value.
- **`kernel.memcap(bytes?)`**: the allocator byte ceiling; allocations past the cap fail (Lua reports `not enough memory`); the cap is a runtime value — unset (or 0) makes the allocator byte-for-byte equivalent to the default, and accounting starts only once set (shrinking recovers, double-entry accounting).

These two are the underlying knobs of the [Sandbox](/guide/sandbox) resource-limit layer: `LUNA_SANDBOX_FUEL`/`LUNA_SANDBOX_MEM` translate into these calls, and the knobs are removed from the `kernel` table after being set — in trusted mode they are ordinary adjustable APIs whose availability is unaffected.

## Internal surface

`kernel.count_hook()` is an ordinary callable form of the count-hook logic itself: one `^C` check, one attach poll, and one fuel strike when a budget is set. Only coverage builds (luacov owns the debug-hook slot exclusively and forwards line events here) call it explicitly; normal runs never encounter it and do not need to.
