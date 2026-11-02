# FAQ

## Why is it called luna?

The moon (luna) reflects sunlight, neither emitting nor generating heat — it just integrates the mature tools of the Lua ecosystem (interactive, modules, standard library, plugins, event loop) without reinventing wheels.

## Relationship to stock Lua / LuaJIT / luau

- **The runtime is Lua 5.5** (embedded, carried by the binary, no system Lua dependency)
- Not LuaJIT, no JIT compilation; not luau (no type system, no compile-time optimization)
- Syntax and standard library follow the Lua 5.5 reference manual; extra built-ins such as LPeg

## Relationship to Node.js / Bun / Deno

- **Not a JS runtime**; does not run JavaScript/TypeScript
- Module resolution, standard-library API, and the event loop are **deliberately aligned to Node v24 semantics** (see [Node alignment](/node-parity))
- `http.serve`/`stream`/`events`/`path`/`util` etc. are pinned to measured Node, but underneath they are pure Lua + mature C libraries (luasocket, libuv, etc.), not V8

## Why not use LuaRocks?

- Pursuit of **single-binary distribution**: all dependencies are compiled into the binary or shipped with `luna_modules/`, zero external runtime dependencies
- `luna_modules/` upward search matches `node_modules`, zero mental overhead
- Package management is a set of subcommands (`luna install|search|list|update`) with LuaRocks **embedded in the binary** (no system luarocks needed): installed into the project's `.luna/rocks/` and locked into `luna.lock`, optional at dev time; the runtime and end users need not run a line — `git clone` or unzip is enough

## How to install? Where do the artifacts come from?

One-line install (installs the latest daily build into `PATH`, verifies with `luna --version` after install, upgrades on rerun):

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/cuihairu/luna/main/install.sh | sh
```

```powershell
# Windows (PowerShell)
irm https://raw.githubusercontent.com/cuihairu/luna/main/install.ps1 | iex
```

The artifacts are a **rolling nightly Release**: fixed tag [`nightly`](https://github.com/cuihairu/luna/releases/tag/nightly), re-published on each green run, assets `luna-nightly-<os>-<arch>.zip` (binary plus the `luna_modules/` module sidecar) + `.sha256` sidecar. Public repository assets are pulled anonymously, **no credentials needed**; `--token`/`GITHUB_TOKEN` and `LUNA_INSTALL_MIRROR` / `LUNA_MIRROR` remain for private forks and direct-link fallbacks. Actions artifacts are only run-page local copies; the delivery channel is the Release. See [Getting started](/guide/getting-started).

## Why does `require("crypto")` say OpenSSL was not linked?

The crypto module binds luaossl, and at build time it **must** link the system OpenSSL dev headers (`libssl-dev` / `openssl-devel`). Without them the build still passes, but the runtime gives an instructive error. Fix:

```bash
# Debian/Ubuntu
apt-get install libssl-dev
# Fedora/RHEL
dnf install openssl-devel
# macOS
brew install openssl
```

Then re-run `cmake` and build. Note `loop.net.connectTls`/`listenTls` also depend on OpenSSL — without it the functions still exist, and calling one gives an instructive error; the other modules (zlib, fs, net, http, csv, ini, toml, yaml, xml, path, util, events, stream) are unaffected.

## Difference between `luna serve` and `http.serve()`

- `luna serve [dir] [port]`: CLI subcommand, zero-script static file service, blocking until `^C`, exit 130
- `http.serve(dir|handler [, port])`: Lua API, the same entry point with **static directory** and **programmable handler** dual modes, returns a service object (`srv:close()`, `srv.port`, `srv.url`), good for tests and embedding

Both sit on the same implementation; the CLI is just the static mode wrapped into one command.

## What if `^C` does not stop the service/script?

- **`luna serve` / a service started by `luna -e`**: an internal 100 ms heartbeat timer wakes the event loop from epoll, so `^C` lands promptly on the prepare hook, exit 130
- **A bare script in a busy loop**: the C kernel's count hook polls the interrupt flag every 100,000 instructions, so `^C` can break in
- **If it still does not stop**: check whether a blocking C extension is bypassing the count hook; or switch to the `loop` event loop (the prepare hook polls each turn)

## Why is the `end` event in `stream` written as `s:end_(...)` or `s["end"](...)`?

`end` is a Lua keyword and cannot be called directly as a method name. `stream` has a uniform convention: keyword-colliding methods are spelled with an underscore (`end_`), and `s["end"](...)` works the same way — `loop`'s sock hit the same constraint, where Node's `end` was renamed `shutdown` (see the half-close section of [Event loop](/guide/loop)). This is a Lua-language limitation, not a bug.

## `csv.lines` iterator raises on bad input; how to use it safely?

`csv.lines` is a per-record iterator, and bad input (such as an unterminated quote) can only raise. **Do not put it inside `for ... in pcall(...)`** — `pcall`'s return is not a valid iterator triple; wrap the whole loop in `pcall`, and rows already collected remain usable when bad data arrives:

```lua
local csv = require("csv")
local data = 'a,b\n1,2\n3,"unclosed\nnext,row\n'
local rows = {}
local ok, err = pcall(function()
    for row in csv.lines(data) do rows[#rows + 1] = row end
end)
if not ok then print("stop:", err) end
print("rows:", #rows)
```

```text
stop: csv: unterminated quoted field at line 5, column 1
rows: 2
```

The format modules' `decode`/`encode` are the `nil, err` convention; bad data does not throw there.

## Is the nil returned by yaml `decode` a real nil or a sentinel?

By default a **real nil** (YAML `null` → Lua `nil`). To keep the sentinel for round-tripping:

```lua
local yaml = require("yaml")
local t = yaml.decode("x: null", { nullval = yaml.null })
-- t.x == yaml.null (the sentinel table), then encode restores null
```

`yaml.null` is the one internal sentinel table, equal to no user data.

## Is it normal for toml `decode` errors to have only a line number, no column?

Normal. The underlying tomlc17 reports only a line, format `<reason> at line N`. This is the C backend's semantics, pinned by test cases.

## Why are `xml.sax` callback names `StartElement`/`EndElement`/`CharacterData`?

Direct passthrough of lua-expat (lxp)'s handler model; both the callback names and the first argument (the parser object) come from lxp. Usage:

```lua
local xml = require("xml")
local p = xml.sax{
    StartElement = function(parser, name, attrs) ... end,
    EndElement   = function(parser, name) ... end,
    CharacterData= function(parser, data) ... end,
}
p:parse(chunk1)
p:parse(chunk2)
p:parse()        -- wrap-up, must call a no-arg parse once
```

## `util.inspect` and `util.format`'s `%j` behavior

- `util.inspect`: Lua-syntax-shape output (strings quoted and escaped, cyclic references marked `[Circular *N]`, `depth` truncation); **key sorting guarantees determinism** — sequence segment first, then numeric ascending and strings byte-order, consistent across runs
- `util.format("%j", v)`: JSON serialization. dkjson is stricter than browser `JSON.stringify` (the latter omits function fields); for values it rejects — functions, NaN, cyclic references — it **falls back to `util.inspect` rendering rather than throwing** — `format("%j", {f = f})` gives `{ f = <function ...> }`, and a cycle gives `{ self = [Circular *1] }`. This is a verified fork, see [Node alignment](/node-parity)

## How to embed luna into my own C/C++ project?

luna is designed as a **single-binary CLI**, not a library. Embedding scenarios:

1. Call the `luna` binary directly as a subprocess (fast start, few dependencies)
2. Use `luna --attach <pid>` to take over a running instance
3. For deep embedding, use Lua 5.5 + the C libraries you need (luasocket/lfs/openssl etc.) directly, referencing `src/luna_main.c`'s initialization flow

## Windows support status

- **Native Windows is available**: the verification face's (daily.yml) Windows leg went green 2026-10-08 (`experimental: false`, a red pulls the night's alarm); the delivery face's (daily-build.yml) Windows leg is delivering `luna-nightly-windows-x86_64.zip` (+`.sha256` sidecar, last verified 2026-10-13) — the earlier compile-time blockers (kernel `mode_t`, luasocket platform source selection, unix domain socket target, loop network-layer POSIX headers, test `poll.h`) have all been digested
- The delivery face's Windows leg keeps `continue-on-error`: a single leg's build failure does not freeze all-platform delivery (the verification face already raises the alarm), a delivery-robustness convention, not a trial run
- `install.ps1` and `install.sh` are the same anonymous direct pull, one-line Windows install
- The `path` module provides `path.posix` and `path.win32` dual faces (both Node v24 semantics; win32 is platform-independent text semantics, usable on Linux too)
- Native Windows and Linux/macOS have the same feature set; WSL2 is also supported; `^C` exit 130, attach, and `luna serve`'s ^C contract have been walked through per-item on POSIX

## How to contribute / report issues

- GitHub Issues: bug reports, feature suggestions, doc corrections
- PRs welcome: open an Issue first to align direction, then open a PR; tests all green, no AI wording in commits
- The docs site source is in `docs/`, VitePress local preview `cd docs && pnpm run docs:dev`

