# Sandbox runtime

`luna --sandbox` (short `-S`) places one session in a restricted world: module allowlist, disabled native loading, removed permissions, resource limits. Trusted mode remains today's runtime, byte-for-byte; the sandbox installs all four restriction layers **before any user code runs**, then removes its own knobs from the table.

All four startup modes work: `--sandbox script.lua args`, `--sandbox -e 'code'`, bare `--sandbox` for the console, and `--sandbox -i`. Installation first prints one banner line to stderr reporting the active layers:

```text
$ luna --sandbox -e 'print("ok")'
sandbox: allowlist + LUNA_SANDBOX_MODULES, no native loads, no subprocess/file-writes/env
ok
```

## Four layers

| Layer | Intercepts | Escape hatch |
| --- | --- | --- |
| Module allowlist | `require` goes through a guard; names outside the list are rejected | `LUNA_SANDBOX_MODULES` admits names explicitly |
| No native loading | `package.loadlib` is removed, `cpath` is cleared, and `package` is frozen | None |
| Permissions | Subprocesses, file writes, environment reads | None (reading the environment returns `nil`, not an error) |
| Resource limits | Instruction budget, memory ceiling | `LUNA_SANDBOX_FUEL` / `LUNA_SANDBOX_MEM` |

### 1. Module allowlist

The default allowlist covers the pure-Lua data face: `json` `path` `util` `events` `stream` `csv` `ini` `toml` `yaml` `xml` `zlib` `lpeg` `argparse`, plus everything already loaded (the runtime's own infrastructure: `kernel`, the line editor, the `luna.*` policy layer) and the `luna.*` namespace itself. Names outside the list (such as `fs`) must be explicitly admitted in `LUNA_SANDBOX_MODULES`:

```lua
-- sbx_demo.lua
local json = require "json"          -- in allowlist, admitted
print(json.encode({ ok = true }))
local ok, err = pcall(require, "fs") -- outside the list, rejected
print(ok, err)
print(os.getenv("HOME"))             -- environment invisible: nil
print(pcall(os.execute, "id"))       -- subprocess: loud refusal
```

```text
$ luna --sandbox sbx_demo.lua
sandbox: allowlist + LUNA_SANDBOX_MODULES, no native loads, no subprocess/file-writes/env
{"ok":true}
false	sandbox: module 'fs' is not in the allowlist
nil
false	sandbox: os.execute is denied
```

Admission is by name, comma-separated: `LUNA_SANDBOX_MODULES=fs,path`. Admission is not full power — when `fs` enters, its write surface is separately removed (layer 3).

### 2. No native loading

`package.loadlib` is set to nil, `package.cpath` is cleared, and the entire `package` table is frozen — both new keys and value assignments are rejected. Freezing uses "empty proxy + frozen copy + `__index` passthrough read", not a single `__newindex` metamethod: a metamethod only intercepts **new** keys, while a bare assignment to the already-existing `cpath` bypasses it. The metatable is locked with `__metatable`; `getmetatable(package)` gets back only the lock string.

```text
$ luna --sandbox -e 'package.cpath = "./?.so"'
[string "eval[1]"]:1: sandbox: package is read-only
$ luna --sandbox -e 'print(package.loadlib)'
nil
```

### 3. Permissions

**Action-shaped operations are loudly rejected** — subprocesses (`os.execute`, `io.popen`), filesystem changes (`os.remove`/`os.rename`/`os.tmpname`, write modes of `io.open`, `io.output(file)`), and introspection that could dismantle the guards (`debug.getupvalue`/`setupvalue`/`upvalueid`/`upvaluejoin`/`getlocal`/`setlocal`/`getregistry`/`sethook`/`setmetatable`):

```text
$ luna --sandbox -e 'local f = io.open("/tmp/pwn", "w")'
[string "eval[1]"]:1: sandbox: io.open write modes are denied
```

**Read-shaped operations silently return empty** — `os.getenv` always returns `nil`, making the environment invisible without exploding: the console's own history file and `path.cwd`'s fallback chain both assume "if unavailable, skip." This is the standard split in WASM runtimes: unavailable reads look absent; denied actions say so.

Binary-chunk loading is closed: `load` is forced to text mode (`string.dump` + binary `load` is the standard escape from the text world), and everything outside text chunks gets:

```text
attempt to load a binary chunk (mode is 't')
```

An explicitly admitted `fs` still exposes only reads: `writeFileSync`/`appendFileSync`/`mkdirSync` and `mkdir`/`rmdir`/`chdir`/`touch`/`lock`/`unlock` are individually replaced by refusal stubs during installation.

### 4. Resource limits

Two knobs are driven by environment variables, then removed from the `kernel` table after setting (the script cannot see or change them):

| Variable | Unit | Destination |
| --- | --- | --- |
| `LUNA_SANDBOX_FUEL` | Million instructions | `kernel.fuel` — the count hook decrements 1 per strike (one strike ≈ 100,000 instructions) and stops at zero |
| `LUNA_SANDBOX_MEM` | MiB | `kernel.memcap` — allocator byte accounting; allocations past the cap fail |

```text
$ LUNA_SANDBOX_FUEL=1 luna --sandbox -e 'local s=0; for i=1,100000000 do s=s+i end'
sandbox: instruction budget exhausted
$ LUNA_SANDBOX_MEM=16 luna --sandbox -e 'local t = ("x"):rep(64*1024*1024)'
not enough memory
```

If neither variable is set, the limit layer is inactive (the other three layers remain); the memory cap is a **runtime** value — the allocator behaves byte-for-byte like the default until a cap is set, and only starts accounting afterward.

## Operational surface and plugins

**The attach channel remains.** `luna ps` still observes and `luna --attach <pid>` still connects — the socket directory is snapshotted before sandbox installation, stale-socket cleanup uses the real functions captured beforehand, and `socket.unix` is also loaded before the guard. The channel itself is not a bypass: every line injected over attach is evaluated in the target state, under the same guards:

```text
$ luna --sandbox /tmp/sbx_busy.lua &        # busy-loop script
$ luna ps | grep sbx
897829   live   ./build/luna --sandbox /tmp/sbx_busy.lua
$ printf 'x = 41\nx + 1\nrequire("fs")\n' | luna --attach 897829
attach> nil
attach> 42
attach> attach:1: sandbox: module 'fs' is not in the allowlist
```

**Plugins are skipped.** Directory plugins inject through `package.preload`, which would bypass the allowlist; under sandbox mode the entire discovery-loader layer does not run, and `%plugins` reports empty.

## Boundary rules

- **One Lua state, not a second VM.** The sandbox hardens the state inside the process: it holds for "untrusted code written in Lua" — no unsafe primitive remains reachable in the language (native loading, subprocesses, file writes, upvalue/registry reads are all cut off); it does not defend the scenario where the process itself is compromised, which requires OS-level isolation.
- **All guard keys are hidden in upvalues.** The real `require` and the real `io.open` are upvalues of the installer function's closure and die when the installer frame returns; upvalue reads (`debug.getupvalue`) and frame-local reads (`debug.getlocal`) are on the rejection list. `debug.getinfo` remains — the introspection magic commands (`%whos`, `%modules`) rely on it to read metadata from **function objects**, and a function object without access to its upvalues is only a call target.
- **Reading the environment returns `nil` rather than raising.** The runtime's own call sites (REPL history, `path.cwd`) have nil fallbacks; the script sees a WORLD with "an empty environment," not a WORLD that explodes everywhere.
- **Limit semantics.** `fuel` granularity is one strike (≈100,000 instructions), so a small budget fires before the big loop; `memcap` accounting recovers on shrinking (double-entry accounting), and fixed startup overhead does not count against the budget.
- **Trusted mode is unchanged.** Without `--sandbox`, the new allocator is byte-for-byte equivalent to the default, and plugins, attach, and all modules work as before; the two worlds diverge only at the `-S` switch.
