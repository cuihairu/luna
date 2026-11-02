# Standard library overview

Everything ships in one binary and is `require`-ready — nothing to install. The standard library is organized into **four classes** — Core, Data-Format, Dev tooling, Ecosystem: before a new module enters a class it must answer "which class does it belong to," and functionality that belongs to no class does not enter the standard library (guarding against "stuff everything in").

## Core — runtime and general face

| Module | In one sentence | Backend | Page |
| --- | --- | --- | --- |
| json | JSON encode/decode (with Node-style `parse`/`stringify` aliases) | dkjson | [json](/stdlib/json) |
| fs | Filesystem: Node-style sync conveniences + the full lfs face | luafilesystem | [fs](/stdlib/fs) |
| path | Path handling, Node `path`-compatible API | pure Lua | [path](/stdlib/path) |
| util | `format`/`inspect`/`parseArgs`/`parseEnv`/`styleText`/`debuglog`, Node v24 semantics | pure Lua | [stdlib/util](/stdlib/util) |
| events | EventEmitter: on/once/emit/error contract | pure Lua | [events](/stdlib/events) |
| stream | Readable/Writable/Transform/pipe/backpressure | pure Lua | [stream](/stdlib/stream) |
| net | TCP/UDP synchronous sockets and one-line `serve()` | luasocket | [net](/stdlib/net) |
| http | One-line `serve()` + luasocket client face | luasocket + pure Lua | [http](/stdlib/http) |
| crypto | sha1/256/384/512, HMAC, random bytes (needs OpenSSL) | luaossl | [crypto](/stdlib/crypto) |
| console | Node console face: log/table/time/count/group family | pure Lua | (see [Node alignment](/node-parity)) |
| process | Node process synchronous subset: argv/env/cwd/exit/stdio | pure Lua | (see [Node alignment](/node-parity)) |
| querystring | URL query-string encode/decode (Node semantics) | pure Lua | (see [Node alignment](/node-parity)) |
| url | URL decompose/recompose/parse (legacy face) | pure Lua | (see [Node alignment](/node-parity)) |
| assert | Node assert face: assert/equal/strictEqual/deepEqual/deepStrictEqual/throws family | pure Lua | (see [Node alignment](/node-parity)) |

## Data-Format — data format encode/decode

| Module | In one sentence | Backend | Page |
| --- | --- | --- | --- |
| csv | RFC 4180 CSV encode/decode and per-line iteration | LPeg | [csv](/stdlib/csv) |
| ini | Classic .ini encode/decode | LPeg | [ini](/stdlib/ini) |
| toml | TOML v1.1 encode/decode, datetime component table | tomlc17 (C) | [toml](/stdlib/toml) |
| yaml | YAML 1.1 encode/decode, multi-document | lyaml | [yaml](/stdlib/yaml) |
| xml | DOM encode/decode + SAX passthrough | expat (lxp) | [xml](/stdlib/xml) |
| zlib | compress/decompress one-shot + streaming deflate/inflate | lua-zlib | [zlib](/stdlib/zlib) |

## Dev tooling — development tool face

| Module | In one sentence | Backend | Page |
| --- | --- | --- | --- |
| logging | Leveled logging, console/file/rolling-file/socket appenders (upstream passed through verbatim) | lualogging | [logging](/stdlib/logging) |

The REPL's own faces (`repl`/`introspect`/`magic`/`complete`/`highlight`, embedded policy modules) also belong to this class, used through the REPL and [CLI and REPL](/guide/cli-repl), not occupying require names.

## Ecosystem — ecosystem access face

No separate stdlib page; entries live in the guide: [LuaRocks wrapper](/guide/rocks) (`luna install` etc., package management), [directory plugins](/guide/plugins) (extension-point access), [module system](/guide/modules) (`luna_modules/` resolution). This class's job is "bring outside code in," not "rebuild outside things" — LuaRocks is wrapped without building a registry of its own, which is exactly that convention.

The async face (TCP/TLS/async fs/subprocess/timers, `require "loop"` explicit entry) is listed separately under [Event loop](/guide/loop), and the coroutine task model on top of it (`require "task"`, run/await/sleep/cancel) under [Async tasks](/guide/tasks) — both are the opt-in second world, not folded into the four classes; see the architecture page's rationale.

Two cross-cutting contracts:

- **Data errors do not throw; argument errors do**: format-class modules (json/csv/ini/toml/xml/yaml) `decode`/`encode` return `nil, err` for bad data (message carries line and column; toml is the exception, line only); a wrong argument type (such as `decode(42)`) raises directly. Feeding untrusted data in a loop/script suggests `pcall` or explicitly taking the second return value.
- **Strings count in bytes**: no module does encoding conversion on Lua strings; UTF-8 is just "pass through and try to count correctly in error locations."

## Bottom-layer modules shipped with the binary

Beyond the table above, the binary also registers a layer of **upstream-verbatim** C modules and bundled Lua libraries — the cores of each wrapper layer (zlib streaming, TOML decoding, expat SAX, the full luaossl family) stand on them; when you need to bypass a wrapper layer, `require` them directly:

| Module | In one sentence | Upstream |
| --- | --- | --- |
| `lpeg` | LPeg pattern matching; the base of highlighting and csv/ini lexing | [LPeg](http://www.inf.puc-rio.br/~roberto/lpeg/) |
| `socket.core` | luasocket's raw TCP/UDP face | [luasocket](https://github.com/diegonehab/luasocket) |
| `socket.unix` | unix domain sockets; the attach channel's transport | same |
| `mime.core` | MIME encode/decode (base64, quoted-printable) | same |
| `zlib.core` | lua-zlib's raw streaming face; the `zlib` wrapper's one-shot API sits on it | [lua-zlib](https://github.com/brimworks/lua-zlib) |
| `toml.core` | tomlc17 decoding core (no encode; encode is the wrapper's Lua face) | [tomlc17](https://github.com/cktan/tomlc17) |
| `yaml.core` | lyaml's C face (parser + emitter; encode/decode both sit on it) | [lyaml](https://github.com/gvvaughan/lyaml) |
| `lxp` | expat's SAX face; `xml`'s DOM builds on it | [lua-expat](https://github.com/tomasguisasola/luaexpat) |
| `_openssl.*` | the full luaossl family (25 submodules, under the `crypto` wrapper) | [luaossl](https://github.com/wahern/luaossl) |
| `lfs` | luafilesystem's full face (the passthrough half of `fs`) | [luafilesystem](https://github.com/keplerproject/luafilesystem) |
| `argparse` | CLI argument parsing (the entry point uses it; scripts can `require` it too) | [argparse](https://github.com/luarocks/argparse) |
| `linedit` | The C line editor; normally via the REPL, directly available for a custom interactive surface | built into luna |

Above those are the `luna.*`-prefixed **embedded policy modules** (`luna.magic`/`luna.complete`/`luna.introspect`/`luna.highlight`/`luna.plugins`/`luna.serve`/`luna.modules`/`luna.rocks` etc.) — the REPL's Lua half; see the three-layer model section of [Architecture design](/architecture). Upstream-verbatim means: error style and API shape follow upstream (mainly `nil, err`), and luna does not re-wrap.

