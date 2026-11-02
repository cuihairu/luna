# CLI and REPL

The REPL is luna's primary entry point; every CLI mode is a variation on it. This page walks through the interaction surface: startup, session, magic commands, and exit.

## Startup modes

| Command | Behavior | Comparable to |
| --- | --- | --- |
| `luna` | Interactive console | `node` |
| `luna script.lua a b` | Run a script and exit; `a b` become the chunk's `...`, and `arg[0]` is the script path | `lua script.lua` |
| `luna -i script.lua` | Run a script, then enter the console (the script's globals remain) | `python -i` |
| `luna -e 'code'` | Evaluate and exit; expressions echo as `Out[n]`, incomplete code errors | `node -e` |
| `luna serve [dir] [port]` | Serve static files until `^C` (current directory and port 8000 by default); exit code 130 follows the `^C` contract | `python -m http.server` |
| `luna --attach <pid>` | Attach interactively to another running luna process | gdb attach / IPython `%connect_info` |
| `luna ps` | List attachable luna processes (pid / live or stale / command line) | `ps` / `pgrep` |
| `luna new <template> <name>` | Generate starter code from a built-in template: `plugin` (directory plugin), `package` (`luna_modules/` package), or `script` (single-file CLI); `--list` shows templates and an existing target is rejected | `cargo new` |
| `luna --sandbox script.lua` (short `-S`) | Restricted session: module allowlist, native loading disabled, no subprocesses/file writes/environment; `LUNA_SANDBOX_FUEL`/`LUNA_SANDBOX_MEM` limit computation and memory | [Sandbox runtime](/zh/guide/sandbox) |

Modes combine with the common `--no-color` and `--no-plugins` switches. Plugins apply to **every** startup mode: scripts can `require` modules injected by plugins (`--sandbox` skips plugins entirely; see [Sandbox](/zh/guide/sandbox)). `luna ps` is a read-only directory view: it scans `$LUNA_SOCK_DIR` (default `/tmp`) for `luna-<pid>.sock`, probes each pid for liveness (`kill(pid, 0)`), and marks sockets left by dead processes as `stale`; an empty result exits with status 0 rather than an error.

## Input model

- **Multi-line continuation**: when a line starts with `function`/`if`/`for`/`while`/`do`/`{` and parentheses or blocks remain open, the prompt enters continuation mode and evaluates the whole block at once; `^C` during continuation discards the block.
- **`^C` interruption**: `^C` interrupts a running infinite loop without ending the session, with `interrupted` in the error message; state is reset by `kernel.clear_interrupt()`. In line editing, `^C` cancels the **current input line** (replxx echoes `^C`) and discards any continuation block, returning to the same numbered prompt; the canceled line does not enter history or consume `In[n]`. Only `^D` on an empty line ends the session. **One-shot modes** (`luna script.lua` and `luna -e`) have no next line to return to: `^C` ends the process with exit code **130** (128 + SIGINT), consistently in both modes, while the message still reaches the caller (stderr for scripts, session output for `-e`).
- **History**: input is persisted to `~/.luna_history` with timestamp separators and recalled with `↑` after restart; replxx incremental search (`^R` style) is available.
- **Tab completion**: chained expressions resolve step by step through global names, fields, and metatable `__index`; inside `require "…` it completes package names at every `luna_modules/` level, package submodules, and loaded modules; completion sources registered by plugins participate too (failing sources are skipped). Candidates are **inserted text**, not rewritten lines: only the final segment is replaced, so `string.su` becomes `string.sub(` without eating `string.`, and `require "jso` becomes `require "json"` without eating the quotes.
- **Help sugar**: `?expr` and `expr?` print the value's signature, documentation, and examples; `%whos` lists all current globals.
- **Typo hints**: when a runtime error is marked `(global 'x')` and `x` is genuinely undefined, the error is followed by the nearest global name by edit distance — `hint: did you mean 'print'?` (two candidates when ambiguous).

## Syntax highlighting

Input lines and echoed results are colored live by scintillua's Lua lexer (driven by LPeg). Coloring applies only on a TTY and degrades automatically in pipes and scripts; `NO_COLOR` / `--no-color` disable it explicitly. **Input and output move together**: turning color off makes the whole session monochrome (the line editor's cursor-positioning escapes are color-independent and still emitted). Both the lexer and color scheme are pluggable — see [Plugin development](/guide/plugins).

## Magic commands

Lines beginning with `%` bypass Lua evaluation and are dispatched by the REPL:

| Command | Effect |
| --- | --- |
| `%help` | List all magic commands, including plugin-registered ones |
| `%time expr` | Execute once and report wall time; expressions echo as `Out[n]`, statements report time only |
| `%timeit expr` | Repeat until 100 ms/1000 runs and report the best single run; accepts statements too |
| `%hist` | All input in this session; `%hist 2` shows one entry and `%hist 2-5` a range |
| `%whos` | Current global variables (name + type + value preview) |
| `%globals [filter]` | Global names and types; `filter` is a plain substring filter (for example `%globals req`); a lighter `%whos` |
| `%info` | Runtime identity: version, pid, uptime, attach socket, memory, and module counts |
| `%modules [name]` | Loaded module list (name + shape); with a name, describe that module using the same help as `?expr` |
| `%stats` | Session counts (inputs/results) plus runtime footprint (globals/modules/gc/uptime) |
| `%gc` | Run a full collection and report memory before and after |
| `%eval <expr>` | Evaluate and record as `Out[n]` (a silent `%time`) |
| `%load <file>` | Load a file into the input buffer (REPL: Enter continues, ^C discards; attach: runs directly in the target state) |
| `%reset` | Clear user globals and `Out`/`_`/`__` while preserving the runtime environment (stdlib/kernel, and so on); `In[n]` session input remains |
| `%clear` | Clear the screen (TTY only) |
| `%plugins` | Loaded plugins; failures are reported separately and shadowed duplicates appear in an overridden section |
| `%tasks` | Live task list (id/what is awaited/source) plus cumulative counts; requires `require "task"` first and only reads `package.loaded`, never auto-loads |
| `%exit` | Exit (same as `^D`) |

The introspection family (`%info`/`%modules`/`%globals`/`%stats`/`%gc`) all goes through the target kernel, so REPL and attach behave identically; this is the P1 runtime-introspection surface. `%tasks` is the observation surface for the P2 task model ([Async tasks](/zh/guide/tasks)), backed by the registry from `require "task"` and likewise read-only over loaded modules.

## Session state

- **`In[n]` / `Out[n]`**: every input (including a multi-line block) is recorded as `In[n]`, and every non-nil result as `Out[n]`, mirrored into `_` (latest) and `__` (previous);
- `%reset` clears user state but preserves the runtime snapshot (names recorded in `__LUNA_BASE_GLOBALS`: stdlibs, kernel, lpeg, and so on);
- Modules injected by plugins are visible through `require` and constrained by the `package.loaded` cache — reload semantics match Lua.

## attach: connect to a running luna

Every luna process opens a Unix-domain listener at `$LUNA_SOCK_DIR/luna-<pid>.sock` (default `/tmp`, mode 0600) at startup. `luna --attach <pid>` connects and enters the attach console: **local line editing, remote evaluation** — commands run against the target's live Lua state (the same globals, `Out`, and even a script stuck in an infinite loop), with all local REPL interaction habits available:

```
$ luna --attach 12345
attached to 12345 — %detach or ^D to leave
attach> gx = 99999
nil
attach> gx
99999
attach> print("hello")        -- output is captured: echoed to the attach session and mirrored to the target terminal
hello
nil
attach> %whos                 -- magic commands run against the target state
attach> ?gx                   -- ?expr / expr? help sugar still works
number
attach> %detach
```

- **Magic commands and help sugar**: `%whos`, `%hist`, `%reset`, `%info`, `%modules`, `%gc`, and so on execute remotely through the target's own `luna.magic` table (`%hist`/`%reset` act on the attach session's history and registers); `?expr` / `expr?` describe values in the target state. `%load` changes meaning in attach: the file runs directly in the target state (remote surgery is the point of attach), not through the input buffer.
- **Output capture**: output from the target kernel during a command is collected into the reply frame and echoed to the client while mirrored to the target's own terminal — both sides see the same stream; a single frame is capped at 64 KB.
- **Tab completion runs remotely**: candidates come from the **target's** globals and `package.loaded` over the `\1complete` meta-line protocol, so attach can complete function names that exist only in the target.
- **`%exit` in attach does not kill the target**: it detaches the client while the target keeps running; quit the target from its own terminal or with `os.exit()`.

**How the target is reached**: the target process has three polling points — while the REPL is idle, a `SIGUSR1` from the attach client makes the blocking line editor return an empty line and the main loop polls; during script execution (even an infinite loop), the C kernel's count hook polls every ~200,000 instructions (every second count-hook hit); and in an event-loop process started with `require "loop"`, the prepare hook polls each iteration. Attach therefore arrives promptly whether the target is waiting for input, spinning, or inside the event loop.

**Boundaries**:

- One command at a time with ordered replies; replies are framed with `\30` (`OK` success / `ERR` error / `EXIT` detach), `OK` frames go to stdout and `ERR` frames (with full traceback) to stderr — pipes receive a clean result stream;
- The target enables this by default; `--no-serve` disables it explicitly (no socket file), and an attach client does not open another listener;
- Evaluation is pcall-isolated — errors in attach never take down the target; if the target crashes, the client reports `attach: target went away`;
- Exit with `%detach` or `^D`; the socket file is cleaned up when the target exits.
