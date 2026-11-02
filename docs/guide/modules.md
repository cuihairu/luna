# Module system

luna installs a **second searcher** immediately after `package.preload` to implement Node.js-style project resolution. The existing searchers keep their semantics, and `package.loaded` caching still applies.

## Resolution rules

`require("name")` goes through:

1. `package.preload` (where built-in policy modules and plugin injections are found);
2. **the luna searcher**:
   - relative names (`./x`, `../x`): resolved relative to the **current chunk's directory** — `require("./util")` finds `util.lua` or `util/init.lua` beside it;
   - bare names (`hello.sub`): **climb upward** from the current directory, checking `luna_modules/hello/` and `luna_modules/hello.lua` at each level up to the filesystem root — the same strategy as `node_modules`;
   - **package subpaths** (`dep.sub` / `dep/sub`): first try the full name, then resolve the remainder as a file inside the longest matching package prefix — `luna_modules/dep/sub.lua`, then `luna_modules/dep/sub/init.lua`. Subpaths do **not** go through the package's `main` (matching Node's `node_modules/dep/sub`), and each level tries the full name before the subpath fallback;
3. Lua's official path searcher and C searcher (`package.path` / `package.cpath`).

## Package manifests

A package directory looks like:

```
luna_modules/hello/
├── package.json     {"name":"hello","version":"0.1.0","main":"init.lua"}
├── init.lua         ← conventional entry when main is absent
└── lib/util.lua
```

- `main` may point to a file (`"lib/main"` → `lib/main.lua`) or a package directory (`"lib"` → `lib/init.lua`);
- without `main`, `init.lua` is the convention;
- the manifest is parsed with dkjson; the result enters `package.loaded` as usual, so a second `require` hits the cache.

The manifest skeleton is generated too: `luna new package mylib` creates `package.json` and `init.lua` (see [Examples and scaffolding](/zh/guide/examples)); move it under `luna_modules/` and it resolves.

## Built-in modules

All ship in the binary: C backends are registered in the kernel, while thin pure-Lua layers live in `luna_modules/`.

| Module | Backend | Contents |
| --- | --- | --- |
| `json` | dkjson | `encode`/`decode` and `stringify`/`parse` aliases |
| `fs` | luafilesystem | The full lfs surface plus `exists`/`isFile`/`isDirectory`/`readFileSync`/`writeFileSync`/`appendFileSync`/`readdirSync`/`mkdirSync` conveniences |
| `net` | luasocket | `tcp`/`udp`/`connect`/`bind`/`serve` (one-line blocking TCP server)/`select`/`dns` |
| `http` | luasocket | `request`, `get`, and the full `http.client` surface |
| `csv` | LPeg | RFC 4180-style `decode`/`encode` (delimiters/newlines/doubled quotes inside quotes, `headers` key row, `delimiter`) and a record-by-record `lines()` iterator |
| `ini` | LPeg | Classic `decode`/`encode` (sections/pre-section bare keys/two comment styles/quoted-value escapes/duplicate keys/`cast` numbers and booleans) |
| `toml` | tomlc17 | TOML v1.1 `decode`/`encode`; `decode` uses the C backend and `encode` is implemented by the wrapper (scalar keys first, `[[array-of-tables]]`, inline arrays/datetimes/empty tables) |
| `yaml` | libyaml + lyaml | YAML 1.1 `decode`/`decodeAll`/`encode`; null becomes nil by default (`opts.nullval = yaml.null` preserves the sentinel), anchors/aliases become shared table references, and `---` multi-documents use `decodeAll` |
| `xml` | expat + lua-expat | The DOM trio `decode`/`encode` (elements are `{tag, attrs, kids}`, text nodes are strings in kids, attributes are always strings, namespace prefixes are preserved) plus streaming `xml.sax` pass-through (the lxp handler model) |
| `path` | pure Lua | Both Node `path` faces: the full `posix` API plus `win32` (`normalize`/`join`/`resolve`/`isAbsolute`/`relative`/text operations/`parse`/`format`, with device-root/UNC/reserved-name guards) and `sep`/`delimiter` (pinned against measured Node v24 behavior) |
| `util` | pure Lua | `util.inspect` (depth/circular `[Circular *N]`/truncation/key quoting in Lua syntax) and `util.format` (`%s %d %i %f %o %O %c %j %%` plus `%x %X`; concatenation without a format string, extra arguments appended, Node v24 behavior) |
| `querystring` | pure Lua | The full Node surface: `parse`/`stringify` (aliases `decode`/`encode`) plus `escape`/`unescape` (`'+'` becomes a space, duplicate keys aggregate into arrays, `maxKeys` defaults to 1000 and `<=0` means unlimited, `'!'()*-._~` and alphanumerics are not escaped) |
| `url` | pure Lua | The full Node legacy surface: `parse` (12-field table, optional querystring conversion, slashes flag), `format` (table/string reconstruction, query tables use stringify), `resolve`/`resolveObject` (RFC 3986 dot-segment collapsing); WHATWG `URL` is not planned (see the node-parity audit) |
| `process` | pure Lua | Common synchronous Node `process` globals: `pid`/`ppid`/`argv` (interpreter first)/`execPath`/`env` (proxy plus overlay)/`cwd`/`chdir`/`exit`/`exitCode`/`uptime`/`hrtime` (+`bigint`)/`umask` (query-only without arguments)/`memoryUsage`/`nextTick` (loop-aware)/`stdout`/`stderr`/`stdin`; deviations are audited in node-parity |
| `console` | pure Lua | Node console surface: `log`/`info`/`debug`/`warn`/`error` (util.format semantics, stdout/stderr split)/`assert` (warns but does not throw on falsy)/`trace`/`dir` (inspect pass-through)/`table` (ASCII grid; properties is the attribute list)/`time`/`timeLog`/`timeEnd`/`count`/`countReset` (millisecond resolution)/`group`/`groupCollapsed`/`groupEnd` (two-space indent)/`clear`/`timeStamp`/`profile`/`profileEnd` (no-op) plus the `Console(stdout, [stderr])` constructor; deviations are audited in node-parity |
| `events` | pure Lua | `events.new()` EventEmitter: `on`/`once`/`off`/`prepend*`/`listeners`/`listenerCount`/`setMaxListeners`/`emit` (default 10, over-limit warning, `'error'` raises with no listener, built-in newListener/removeListener events) |
| `stream` | pure Lua | Node streams v1 on top of events: `readable`/`writable`/`duplex`/`transform` (`_read`→`push`, `_write`/`_transform`+`_flush`, `write`/`end_`), `pipe` backpressure (stop on write false, resume on `drain`, `unpipe`), `highWaterMark` accounting (16 KiB default); adapters `duplexFromSock` (loop.net sock), `readableFromChunks` (in-memory chunks), `pushReadable` (onData push) |
| `assert` | pure Lua | Node assert surface: `assert`/`ok` (JS falsy: nil/false/0/`''`/NaN), `equal`/`notEqual` (loose), `strictEqual`/`notStrictEqual`, `deepEqual`/`notDeepEqual`, `deepStrictEqual`/`notDeepStrictEqual` (structural), `fail`, `ifError`, `throws`/`doesNotThrow` |
| `zlib` | lua-zlib | Streaming `deflate`/`inflate` plus one-shot `compress`/`decompress` conveniences |
| `crypto` | luaossl | `sha256` family, `hmac`, `randombytes` (requires build-time OpenSSL) |
| `loop` | libuv (C) + pure-Lua http | Asynchronous surface: timer/fs/net/process/udp/dns/os/signal/http; see [loop](/zh/guide/loop) |
| `task` | pure Lua | Structured asynchronous tasks: `run`/`await`/`sleep`/`promise`/`promisify`; see [tasks](/zh/guide/tasks) |
| `logging` | lualogging (vendored) | Six-level thresholds plus log4j-style appenders; see [logging](/zh/stdlib/logging) |

**Error contract for format modules** (csv/ini/toml/yaml/xml, like json; see [Node alignment](/zh/node-parity)): `decode`/`encode` **return `nil, err` for bad data instead of throwing**, and the message carries line and column (`<fmt>: <reason> at line N, column M`, columns counted in UTF-8 characters). Only wrong argument types raise. toml is the exception: tomlc17 reports only a line, so messages are `<reason> at line N` with no column clause (numeric arguments are first converted with `string.format`'s `%s`, then treated as bad data and returned as `nil, err`). yaml's coordinates also differ: they identify the start of the **last successfully parsed event** (lyaml discards libyaml's own problem_mark), often not the failing line — that is the implementation semantics, pinned by tests. `csv.lines` is an iterator (nil means done), and bad input can only raise; use `pcall` when feeding untrusted data.

**path/querystring/url/process/console/util/events/stream/assert are Node-semantics modules** (not format modules, so the `nil, err` contract above does not apply): there is no "bad data," only wrong argument types, which always raise. Behavior is pinned function by function against **measured Node v24 behavior**; intentional divergences (`util.format`'s `%x`/`%X`, inspect's Lua syntax and deterministic key order, `pipe` error propagation, deterministic querystring key order, millisecond-resolution process hrtime, console's millisecond timers and Lua-shaped units, and so on) are audited in [Node alignment](/zh/node-parity). Because `end` is a keyword, stream's method is spelled `s:end_(...)` (`s["end"]` also works, following the existing sock precedent).

```lua
local fs = require("fs")
local text = fs.readFileSync("notes.txt")
local packed = require("zlib").compress(text)
```

## Relationship to official semantics

- The searcher is inserted after preload, so **plugin injection always takes precedence over project modules** — plugins can act as shims;
- Relative names are anchored to the chunk file path (the `@` source from `debug.getinfo`); in the interactive console they anchor to the current working directory;
- Circular requires do not get official Lua's recursion protection (the loaded mark happens after the loader returns), so modules must not require themselves — same rule as official Lua;
- Lua 5.5's `require` returns two values on a **fresh load** (the module plus the searcher's loader data) and one value on a `package.loaded` hit; `return require("x")` passes both through, while `return (require("x"))` truncates to the module itself.
