# Node alignment roadmap: format standard library and low-level capability selection

This is a **planning document** (docs before implementation, the same style as [event-loop backend design](/zh/loop-backend-design)). It answers two questions:

- **A. Format standard library**: bring the five format modules `xml` / `yaml` / `toml` / `csv` / `ini` into the built-in stdlib at the same level as `json` — which backend, what API shape, and how errors are reported;
- **B. Node low-level capabilities**: item-by-item inventory of `path` / `timers` / `child_process` / `os` / `util` / `events` / `stream` — which already exist in luna (under which name), which are gaps, how to fill the gaps, and at which layer.

The selection principles follow [architecture design](/zh/architecture)'s existing discipline: **bind mature C libraries** (maturity = specification coverage + years in production + error-reporting quality, not "more C is better"); **use pure Lua for logic/policy layers** (Node itself's path/events/stream are pure JS, so luna's counterparts go in the Lua layer); **keep the single binary** (new C dependencies are always statically linked into deps/ with versions pinned). Batch decomposition and acceptance criteria are in `todo.md` (the twentieth-round proposal); this document covers only "what to choose, why, and what the API looks like."

## Standard library one-to-one comparison (Node ↔ luna)

Scope: every Node **v24** standard-library module, compared one by one; the luna side uses the source registry (`register_c_modules` + `lua/modules/*` + REPL globals), not impressions. Three statuses: **✅ corresponding** (same-name capability, API shape close to Node), **◐ partial** (a counterpart exists but the face is narrowed or shaped differently), **❌ no equivalent** (a registered gap — registration, not denial; whether to fill it is in each batch plan).

| Node module | luna counterpart | Status |
| --- | --- | --- |
| `assert` | Pure-Lua `assert` module (assert/ok/equal/notEqual/strictEqual/notStrictEqual/deepEqual/notDeepEqual/deepStrictEqual/notDeepStrictEqual/fail/ifError/throws/doesNotThrow; Node v24.21.0 shape comparison) | ✅ pure Lua (deviations in the assert verification) |
| `async_hooks` | — | ❌ no equivalent hook on the single-threaded callback face |
| `buffer` | A Lua string is an 8-bit string; binary packing goes through official `string.pack`/`unpack` | ◐ language primitives cover it, no separate Buffer type |
| `child_process` | `loop.process`: `run` (aggregate) / `spawn` (streaming, stdio handed directly to `loop.net` socks) / `exec` / `execSync` | ✅ |
| `cluster` | — | ❌ multiprocessing can be assembled with `loop.process` |
| `console` | Pure-Lua `console` module (log/info/debug/warn/error/assert/trace/dir/table/time/timeLog/timeEnd/count/countReset/group family + Console constructor) | ✅ shape compared (deviations in console verification) |
| `crypto` | `crypto` (sha1/sha256/sha384/sha512/hmac/randombytes convenience layer); cipher/kdf/pkey/x509 via `require "openssl.<sub>"` (luaossl submodules ship with the binary) | ✅ the certificate face goes deeper than Node's built-ins |
| `dgram` | `loop.udp` (bind resident receive, send callback per packet) | ✅ |
| `diagnostics_channel` | — | ❌ |
| `dns` | `loop.dns` (lookup/reverse, thread-pool resolution) + synchronous `net.dns` (luasocket) | ✅ |
| `domain` | — | ❌ (deprecated in Node itself, not pursued) |
| `events` | `events` (on/once/off/prepend/listeners/setMaxListeners/'error' contract) | ✅ batch 6 |
| `fs` | Synchronous `fs` (lfs + conveniences) + `loop.fs` (thread pool, error-first callbacks) + `fs.watch` directory observation | ✅ |
| `http` | Synchronous `http` (client + one-line serve) + `loop.http` (1.1 client over loop.net, onHead/onData streaming, redirect/timeout state machine; `http.listen` server) | ✅ 1.1 |
| `http2` | — | ❌ |
| `https` | `loop.http` via `net.connectTls` (TLS client) | ✅ |
| `inspector` | `luna --attach` (polling-point observation, not CDP) | ◐ different form |
| `module` | Module system: `require` + `luna_modules/` upward search + manifest `main` + `luna install` (LuaRocks wrapper, `luna.lock` offline reproduction) | ✅ npm's counterpart is the package-management face |
| `net` | Synchronous `net` (luasocket) + `loop.net` (connect/listen/connectTls/listenTls, multi-address fallback) | ✅ |
| `os` | Official `os` unshadowed + `loop.os` (hostname/type/arch/release/EOL/userInfo/availableParallelism/uptime/loadavg/freemem/totalmem/cpus/networkInterfaces/home/tmpdir) | ✅ batch 8 |
| `path` | `path` (posix face) + `path.win32` (device roots/UNC/`\\?\` namespace/reserved names/CVE protection), pinned to measured Node, pure-Lua dual face | ✅ posix + win32 dual face (batch 6 unfreeze, win32 same text semantics as Node on Linux) |
| `perf_hooks` | `%time` magic + `loop.os`'s uptime/loadavg | ◐ no independent high-resolution timing library face |
| `process` | Pure-Lua `process` module (pid/argv/env/cwd/chdir/exit/exitCode/uptime/hrtime/umask/memoryUsage/nextTick/stdio) + `loop.process` (subprocesses) + `loop.os` (system info) | ✅ common synchronous face (deviations in process verification) |
| `punycode` | — | ❌ (`url`'s IDNA toASCII not done, see the `url` row verification) |
| `querystring` | `querystring` (parse/stringify + escape/unescape; aliases decode/encode) | ✅ pure Lua, differential comparison against Node v24.21.0 |
| `readline` | `luna_line` (replxx bridge: edit/history/highlight/completion) — built into the REPL, not a public library | ◐ different form |
| `repl` | luna REPL itself (multi-line continuation/completion/highlight/magic commands/`In[n]`·`Out[n]`) | ✅ |
| `stream` | `stream` (Readable/Writable/Duplex/Transform/pipe backpressure; sock/chunks/http adapters) | ✅ batch 7, v1 face narrowed (no webstreams/async iterators/setEncoding/cork) |
| `string_decoder` | — | — (Lua strings are native byte strings, no encoding-stream splitting problem) |
| `timers` | `loop.setTimeout/setInterval/setImmediate/clear*` + script-tail auto-drain | ✅ batch 8 (not global: needs `require "loop"`) + REPL integration (2026-10-10, wake-thread timed face) |
| `tls` | `loop.net.connectTls/listenTls` (OpenSSL state machine, certificate validation on by default) | ✅ |
| `trace_events` | — | ❌ |
| `tty` | Kernel `kernel.tty()` + the REPL's tty shape | ◐ |
| `url` | `url` (parse/format/resolve/resolveObject, legacy face; `loop.http` still owns request-line parsing) | ◐ legacy face ported, WHATWG `URL`/`URLSearchParams`/`domainToASCII`/`pathToFileURL` not proposed (see verification below) |
| `util` | `util.inspect` (deterministic key order/depth/cycles) + `util.format` (%s %d %f %i %j %%) + `util.parseArgs` (long/short options/inline values/`--` termination/strict/positionals/`tokens`) + `util.parseEnv` (.env) + `util.stripVTControlCharacters` (ANSI stripping) + `util.styleText` (SGR wrapping) + `util.debuglog` (NODE_DEBUG gating) | ◐ no promisify/callbackify/types (no Lua promise contract) |
| `v8` | `luna.introspect` + REPL `?expr` help | ◐ different form (no heap snapshot face) |
| `vm` | Official `load` (chunkname/env parameters) | ◐ language primitives cover it |
| `wasi` | — | ❌ |
| `worker_threads` | — | ❌ (concurrency model shaped like Node: single-threaded event loop + thread-pool IO; Lua coroutines are cooperative only) |
| `zlib` | `zlib` (deflate/inflate one-shot conveniences + streaming closures) | ✅ |

**Node globals comparison**: `process`→✅ (pure-Lua `process` module, common synchronous face; subprocesses still `loop.process`); `Buffer`→❌ (`string` + `string.pack`); `console`→✅ (pure-Lua `console` module, shape compared); `URL`/`fetch`/`AbortController`→❌ (`loop.http` is callback-style, no promise contract); global `setTimeout`→◐ (`loop.setTimeout`, opt-in, see the timers row); `TextEncoder`/`TextDecoder`→the official `utf8` library.

**Reverse ledger**: things luna has that the Node standard library does not — the IPython-style REPL (`In[n]`/`Out[n]`/magic commands/live highlight and completion), the directory plugin mechanism (four extension points), `luna.lock` offline reproducible installs, `--attach` cross-process observation, and a single binary carrying the entire standard library and package manager. This table aligns only the Node standard library, not the npm ecosystem.

**Coverage philosophy**: ❌ rows are not a debt list, they are selection records — each row corresponds to a "no code just for line count" decision (see `todo.md`'s explicit "not doing" section) or an unproposed direction; ✅ rows' API shapes are pinned to measured Node v24 (the batch-6 probe verification is in the path/util/events sections below).

## A. Format standard library

### Common convention (aligned with json)

The `json` module's de facto contract (dkjson backend, `lua/modules/json/init.lua`):

1. **decode/encode failures do not throw; they return `nil, err`** (err is a string) — the caller dispatches with `local v, err = m.decode(s)`; dkjson's encode on cyclic references and decode on invalid text both take this shape;
2. **Only a wrong argument type raises** (non-string text, non-table value — a programmer error; `pcall` catches it and you fix the code);
3. **Node naming aliases are kept only for json** (`parse`/`stringify`) — the new formats are npm packages on the Node side, not built-ins, so no aliases are invented; `decode`/`encode` are the only entry points.

All five new modules follow 1/2, plus two more:

4. **Error messages carry line and column**: `<fmt>: <reason> at line N, column M` (text formats report coordinates as line/column primarily; json's second return is a byte offset, an implementation detail the new formats do not copy);
5. **Distribution is at the same level as json**: `require "xml"` etc. resolve through `luna_modules/` — the C backend registers in the `zlib.core` pattern (`register_c_modules` into `package.loaded`, glob off), and the wrapper layer lives in `lua/modules/<name>/init.lua` (Node-style convenience functions and unified error convention happen there); pure-Lua/LPeg backends stage directly in the dkjson pattern. C-library error messages are first normalized into convention 4's shape by the wrapper before returning.

| Capability | decode | encode | Streaming |
| --- | --- | --- | --- |
| json (existing) | value | string | — |
| xml | DOM table | string | `xml.sax` (SAX callback passthrough) |
| yaml | value | string | `yaml.decodeAll` (multi-document, already implemented) |
| toml | table | string | — (single-document format) |
| csv | row array | string | `csv.lines` (per-line iterator) |
| ini | section table | string | — (single-document format) |

### Comparison table (selection at a glance)

| Format | Node-side counterpart | Existing Lua ecosystem | luna choice | Why |
| --- | --- | --- | --- | --- |
| xml | fast-xml-parser / xml2js (npm, not built-in) | lua-expat (lxp, binds expat), LuaXML, SLAXML, xml2lua (pure Lua) | **expat + lua-expat binding** | XML's entity/encoding/namespace boundaries are a C library's domain; expat is the foundation of half the internet's XML stack, MIT, errors carry line/column |
| yaml | js-yaml / yaml (eemeli) | lyaml (binds libyaml), tinyyaml, lua-yaml (pure-Lua subset) | **libyaml + lyaml binding** | YAML's specification complexity is notorious (anchors/aliases, multi-document, block scalars, indentation); pure-Lua implementations are all subsets that silently drop semantics |
| toml | @iarna/toml, toml (npm) | toml.lua, lua-toml (pure Lua) | **tomlc17 binding (see the decision record below)** | TOML's spec is small and pure Lua is feasible, but the edges — multi-line strings/Unicode escapes/integer width/datetime — are carried by a C implementation that passes all of toml-test; owning it yourself is not worth it |
| csv | csv-parse (csv package), papaparse | (no de facto standard; penlight not in deps) | **LPeg grammar, pure-Lua layer** | CSV is a line-oriented regex-level format, RFC 4180 one page; pulling a C library for zero dependencies goes the wrong way (LPeg already in deps) |
| ini | ini (isaacs), dotenv | (no de facto standard) | **LPeg grammar, pure-Lua layer** | same; section/key/comment trio, a hundred-line grammar |

**Why C bindings over pure Lua (for xml/yaml/toml)**: three shared reasons — (1) **specification coverage**: the C implementations have passed official test suites (expat's test set, libyaml's production run with the PyYAML ecosystem, tomlc17 passing 214/466 of toml-test), while a pure-Lua "supports YAML" usually means "up to the subset the author used"; silently accepting bad input is more dangerous than erroring; (2) **error-reporting quality**: line/column error positions are already in a C library's parser structure, while a pure-Lua implementation must grow its own; (3) **throughput**: throughput is not the main factor in REPL scenarios (already argued when architecture.md chose dkjson), but the 3–10× gap on large YAML/TOML in script scenarios is free.

**Why csv/ini go the opposite way with LPeg**: these two formats' whole spec is field quoting/escaping and sections/keys/comments — an LPeg grammar of a dozen lines, deterministic, testable, zero new dependencies; the only benefit of C is throughput, and lpeg itself is written in C.

### XML → expat (via lua-expat)

| Candidate | Conclusion |
| --- | --- |
| **expat 2.x + lua-expat (lxp)** | **Selected**: expat is the de facto foundation among XML parsers (MIT, James Clark origin, maintained by the Expat project); lua-expat (Kepler project) is the oldest binding on the Lua side, 1.5.2 supports Lua 5.4 (5.5 adaptation expected as a small vendored patch, same as luasocket, verified at implementation time); the SAX callback model naturally gives streaming; errors carry line/column |
| libxml2 | Rejected: tree API + XPath + validation + serialization kitchen sink; luna's stdlib face needs only decode/encode — for one face you drag in the largest dependency tree and CVE tracking surface; Lua-side bindings are fragmented (no de facto standard); if XPath is ever needed, re-propose then, don't prepay |
| minixml (mxml) | Rejected: a single file is nice, but namespace/DTD/encoding coverage is weaker than expat and the Lua binding ecosystem is thin; the lightweight advantage is eaten by "still have to verify spec coverage yourself" |
| Pure Lua (LuaXML / SLAXML / xml2lua) | Rejected: entity expansion, encoding declarations, and attribute normalization each vary; none has passed a systematic spec test suite |

**API draft**:

```lua
local xml = require "xml"

-- decode: text → DOM table. An element = { tag, attrs, kids }; text nodes are plain strings in kids
local doc = xml.decode([[<person id="7"><name>Ada</name></person>]])
-- doc == { tag = "person", attrs = { id = "7" },
--          kids = { { tag = "name", attrs = {}, kids = { "Ada" } } } }

-- encode: DOM table → text (attribute values forced tostring, text nodes escaped as &amp;/&lt;/&lt;)
xml.encode(doc)                      -- compact; opts.indent = 2 for pretty output

-- streaming: SAX callback passthrough (lxp's handler model verbatim; decode uses it internally to build the DOM)
local p = xml.sax {
    StartElement = function(self, name, attrs) ... end,  -- first arg self (lxp docall convention)
    CharacterData = function(self, text) ... end,
    EndElement   = function(self, name) ... end,
}
p:parse(chunk) p:parse()             -- feed incrementally; a no-arg parse ends the stream (lxp's parse(s) does not terminate)
```

```text
-- error: xml.decode("<a><b></a>")
--   → nil, "xml: mismatched tag at line 1, column 9" (line/column from expat, normalized by the wrapper)
```

- **The DOM shape's tradeoffs**: fast-xml-parser's "tag name as key" shape is an ambiguity source for Lua's mixed array/hash tables (same-named sibling elements, attribute/child name collisions); the `{tag, attrs, kids}` trio is unambiguous, round-trip-able, traversal is just walking `kids`, and the encoder never has to guess. Attribute values are **always strings** (XML has no types);
- **namespaces**: prefixes are preserved verbatim (`<x:a>` has tag `"x:a"`), no expansion — the same convention as fast-xml-parser; the expanded version is on the "explicitly not doing" list;
- **streaming**: the SAX passthrough is a free large-document face (the same mechanism decode uses to build the DOM); `xml.sax` has no second abstraction — the handler table is just lxp's shape.

**Implementation verification (2026-09-30)**:

- **the handler's first argument is self**: lxp's docall pushes the parser userdata as the first argument — `StartElement = function(self, name, attrs)`; the API draft above omitted self (`function(name, attrs)`), and the implementation wins (the rest of the draft is pinned by tests);
- **decode keeps whitespace text nodes**: whitespace between elements (such as `<r> <a/> </r>`) is a plain string in kids — standard DOM behavior, and the round-trip of pretty XML depends on it; declarations/comments/DOCTYPE do not enter the DOM;
- **adjacent text fragments merge at element boundaries**: entity expansion (`a&amp;b`) and CDATA boundaries deliver text in pieces; decode merges them into one string at StartElement/EndElement — `&` is not double-escaped after a round-trip;
- **error columns count in UTF-8 characters** (same convention as yaml): `<r>张三<x></r>`'s mismatched tag reports column 11 (characters), not byte 15;
- **encode's tolerance face**: nil `attrs`/`kids` are treated as empty; a scalar kid (number/boolean) goes through tostring as text; attribute values are forced tostring; attribute order = pairs order (insertion order not promised, the same convention as toml encode);
- **encode cycle detection**: an element that is its own ancestor → `nil, "xml: cyclic table reference"` (error level 0, message without a file:line prefix); a shared sibling reference is not a cycle, each serializes in full;
- **expat's parse(s) does not terminate the stream**: the final flag is `s == NULL` — decode must call `p:parse(s)` and then `p:parse()` to finish; the unclosed-tag error surfaces only at finish, and when finish fails, close throws again, which decode catches with pcall (bad input does not raise).

### YAML → libyaml (via lyaml)

| Candidate | Conclusion |
| --- | --- |
| **libyaml + lyaml** | **Selected**: libyaml is the YAML spec's reference C implementation (MIT, shared by the PyYAML/libyaml ecosystem); lyaml (gvvaughan, MIT, Lua 5.1–5.4, 6.2.9) provides the C-side fast decode/emit; the 5.5 adaptation is the same as above, expected as a small vendored patch (**implementation verification 2026-09-30**: pin v6.2.9 (the latest release; the lukefile claims 5.5 support), compiles with zero patches under 5.5 — lyaml.h's 5.2–5.4 shims do not trigger, but luna's bundled Lua's luaconf.h still restores the `lua_objlen`/`lua_strlen` compatibility macros, so all four C translation units pass verbatim; only the luke build system's `VERSION` macro is defined by CMake) |
| Pure Lua (tinyyaml / lua-yaml) | Rejected: both are subset implementations — anchors/aliases, block-scalar indentation, multi-document indicators each miss corners; YAML has many ambiguous inputs, and silent misparse is more dangerous than erroring |
| Porting js-yaml | Rejected: specification complexity does not change with implementation language, same reasons as above |

**API draft**:

```lua
local yaml = require "yaml"

yaml.decode("name: luna\nreleases:\n  - 1\n  - 2\n")
--   → { name = "luna", releases = { 1, 2 } }
yaml.encode({ ok = true })           -- → "---\nok: true\n...\n"

yaml.decodeAll(doc)                  -- multi-document stream: --- separated, returns an array
yaml.decode(s, { nullval = yaml.null })  -- keeps the null sentinel (default → nil)
```

- **scalar types**: mappings → tables, sequences → array tables, strings/integers/floats/booleans → same-name Lua types; **YAML null → nil** (same as json/dkjson's default); the "key present but value null" distinction is opt-in with `opts.nullval = yaml.null` (a sentinel table) — js-yaml's null semantics, off by default, aligned with json (**implementation verification**: substitution touches only **values**; null keys keep the sentinel — silently deleting entries is worse than keeping the sentinel; null in arrays also lands as nil, shrinking the sequence's tail; a null document is a nil slot in `decodeAll`);
- **anchors/aliases**: decode resolves aliases into **shared table references**; encode by default sends each shared reference a separate copy and errors on ancestor cycles (dkjson's self-referential convention); the automatic anchor emission of `opts.anchors = true` **has landed (2026-10-10, honored by a later batch)** — a pre-pass before encode names tables referenced more than once and cycle-closure points a1..aN in discovery order, planting them into lyaml's pre-declared anchor mechanism (get_anchor emits `&aN` on first occurrence, get_alias emits `*aN` on repeats, and aliases are resolved within MAPPING/SEQUENCE_START); decode resolves aliases back into shared references, so reference identity and cycles round-trip verbatim;
- **errors**: lyaml's error strings carry line/column, normalized by the wrapper into the common convention.

### TOML → tomlc17 (decision record: the dispatch note wrote tomlc99; this selection moves to tomlc17)

| Candidate | Conclusion |
| --- | --- |
| **tomlc17** | **Selected**: see below; parser-only — decode is ready-made, **encode is luna's own Lua face** (TOML serialization rules are simple: tables/arrays/strings/numbers/booleans/datetimes, ~120 lines; tomlc17 having no encoder is the accepted cost) |
| Pure Lua (toml.lua / lua-toml) | Rejected (honest record: this is the weakest C-binding justification among the five formats): TOML 1.0's spec itself is not large, and pure Lua is feasible; the rejection reasons are the edges — multi-line basic/literal strings, Unicode escapes, 64-bit integer boundaries, combinations of local datetimes — toml-test's 466 invalid cases are a ready-made edge list already carried by a C implementation; the binding layer is actually thinner than the format layer |
| tomlc99 | Backup: same decision record |

**API draft**:

```lua
local toml = require "toml"

toml.decode [[
title = "luna"
[server]
host = "127.0.0.1"
port = 8321
tags = ["repl", "lua"]
]]
--   → { title = "luna", server = { host = "127.0.0.1", port = 8321, tags = {"repl","lua"} } }

toml.encode({ title = "luna" })      -- scalar keys first, table keys after (reordered at the encode layer, not the caller's job)
```

- **type mapping**: tables/arrays/strings/integers/floats/booleans map directly; **datetime maps to a table** `{year, month, day, hour, minute, second, secfrac?, offset?}` (a pure date lacks time parts, a pure time lacks date parts, `offset` is minutes) — Lua has no date type, and this shape feeds `os.time` directly (local time); encode recognizes the same shape on write-back (a key set entirely from the component set with a `year` or `hour` counts as a datetime; a plain data table carrying any other key avoids it);
- **encode key order**: TOML requires "scalars first, tables after"; the encode layer does the reorder (not the caller's job); within a group the Lua table order applies (pairs, insertion order not promised); mixed types inside an array output as-is;
- **errors**: tomlc17's error strings carry a line number (verified at implementation time: three string shapes are normalized into `<fmt>: <reason> at line N`, **no column clause** — tomlc17 reports only a line, an explicit fork from csv/ini's line-and-column convention, recorded in guide/modules.md).

### CSV / INI → LPeg grammar (zero new dependencies)

| Candidate | Conclusion |
| --- | --- |
| **LPeg grammar + pure-Lua wrapper** | **Selected**: the two formats' whole spec is field quoting/escaping (csv) and sections/keys/comments (ini); an LPeg grammar is deterministic and testable; deps already has lpeg (1.1.0, `require "lpeg"` registered), **zero new dependencies** |
| A small pure-C library (libcsv etc.) | Rejected: pulling a new vendor for what a hundred lines of C can do goes the wrong way; "bind mature C libraries" targets spec-heavy formats, not the format itself |
| A hand-written pure-Lua loop | A backup rather than a selection: hand-writing the csv escaping state machine with quoted fields is bound to err (delimiters/newlines/double-quotes inside quotes); LPeg writes these into the grammar and tests pin them |

**csv API draft**:

```lua
local csv = require "csv"

csv.decode('name,age\n"Ada,Lovelace",36\n')
--   → { { "name", "age" }, { "Ada,Lovelace", "36" } }
csv.encode({ {"a","b"}, {1,2} })    -- fields needing quoting are quoted automatically

csv.decode(text, { headers = true })  -- first row as headers: { {name="Ada,Lovelace", age="36"} }
csv.lines(s)                          -- iterator (streaming: large files line by line, not building the whole table)
-- opts.delimiter = ";"  /  decode accepts CRLF and LF, encode emits "\r\n" (RFC 4180; opts.eol can change to "\n")
```

- **values are always strings**: CSV has no types (Node csv-parse's same default); conversion is the caller's job (`tonumber`);
- **errors**: an unterminated quote etc. → `nil, "csv: unterminated quote at line N, column M"` (LPeg failure position → line/col conversion; the wrapper holds a shared offset→line/col helper, common to all five format modules — implemented in `lua/luna/`'s policy layer or inlined per module, decided per batch);
- **streaming**: `csv.lines`' iterator model naturally supports chunked feeding (`io.lines` / `fs.readFileSync` then `:gmatch`, the same family).

**ini API draft**:

```lua
local ini = require "ini"

ini.decode("[server]\nhost = 127.0.0.1\nport = 8321\n")
--   → { server = { host = "127.0.0.1", port = "8321" } }
ini.encode({ server = { host = "127.0.0.1" } })
```

- **values are always strings** (ini has no types; the same as Node's ini package), and `opts.cast = true` enables number/boolean auto-conversion;
- bare keys before a section land on the root table (`{ top = "1", server = {...} }`, npm ini's shape); `#` and `;` comments, `key = "value"` quoted form, duplicate keys with the latter winning;
- error convention as above (bad quote / missing `]`).

### Distribution and build (all five formats together)

- **vendor method**: all go through `deps/` submodules with pinned versions (same as luafilesystem/libuv, registered in .gitmodules); expat and libyaml upstreams ship their own CMake (`add_subdirectory(... EXCLUDE_FROM_ALL)`, see the libuv section); tomlc17's two files go directly into the static library; lua-expat/lyaml take their binding sources (`lxp`/`lyaml`'s luaopen) compiled into `luna_xml`/`luna_yaml` static libraries;
- **registration**: the binding layers `lxp`, `toml.core`, `yaml.core` go into `register_c_modules` (luna_main.c, package.loaded glob off); the wrapper layers `lua/modules/{xml,yaml,toml,csv,ini}/init.lua` are auto-staged by the existing `LUNA_STDLIB_WRAPPERS` glob — zero new mechanism in the distribution path;
- **build-dependency convention**: all three C backends are source-vendored, **no system-library dependency introduced** (unlike zlib needing system ZLIB and crypto needing OpenSSL — the format libraries are all self-contained, so offline reproducible builds are unaffected).

## B. Node low-level capability inventory

First the conclusion table (current state verified by source, registered 2026-09-29, `src/luna_loop.c` 4828 lines / `lua/modules/`):

| Node capability | luna current state | Gap | How to fill | Layer | Batch |
| --- | --- | --- | --- | --- | --- |
| fs | ✅ synchronous `fs` (lfs + conveniences) + async `loop.fs` | — | — | — | — |
| net | ✅ `loop.net.connect/listen/connectTls/listenTls` (async) + **multi-address fallback (2026-10-01 verification, see below)** | — | — | C (luna_loop.c) | 2026-10-01 ✅ |
| timers | ✅ `loop.setTimeout/setInterval/setImmediate/clear*` (opt-in) + **script-tail auto-drain (batch 8 enabled)** + **REPL integration (2026-10-10, wake-thread timed face)** | global `setTimeout` deferred (see below) | see below | C entry + Lua wrap-up | 8 ✅ |
| child_process | ✅ `loop.process.run` (aggregate) / `process.spawn` (streaming) + `exec`/`execSync` (batch 8) | — | — | C (in loop.c) | 8 ✅ |
| os | ✅ `loop.os` (hostname/type/arch/release/EOL/userInfo/availableParallelism/home/tmpdir/uptime/loadavg/freemem/totalmem/cpus/networkInterfaces) | — | — | C (in loop.c) | 8 ✅ |
| path | ✅ batch 6 landed (`path`, pure Lua) | — | — | Lua | 6 |
| util | ✅ batch 6 landed (`util`, pure Lua; `luna.introspect` is still the REPL's introspection — the division of labor below) | — | — | Lua | 6 |
| events | ✅ batch 6 landed (`events`, pure Lua) | — | — | Lua | 6 |
| stream | ✅ batch 7 landed (`stream`, pure Lua, built on events) | — | — | Lua | 7 |

**How "bind C libraries first" lands on this table**: the base of timers/child_process/os is already libuv (deps already present) — the only new C code on the B face is `loop.os`'s four getters (~60 lines, still in the existing `os_funcs` table); path/util/events/stream **have no mature C library to bind** (Node itself is all pure JS; libuv has no path join or EventEmitter), so they land in the policy layer, right in line with the architecture layering — "the C kernel as thin as possible, all behavioral logic in Lua."

### net: connect/connectTls multi-address fallback (2026-10-01 verification)

**Implementation verification (2026-10-01)**: the table's initial "Lua policy layer" estimate does not hold — the addrinfo chain and handle lifecycle are both on the C side, so the fallback lands as a direct fix in `luna_loop.c`:

- both dial paths **keep the resolution chain** (attached to sock/tsock, released in three places: first success / all fail / close), a connect failure moves to the next address, and only when all fail does the **last** address's error go to the callback (Node `net.connect`'s autoSelectFamily semantics; the single-address path behaves identically to the old code);
- the plain side: libuv does not allow a second connect on a failed handle — after `uv_close`, reinitialize the handle in the close callback and dial again;
- the TLS side: a refused SO_ERROR surfaces **asynchronously** through poll, so the poll handle must be closed with the fd and a new fd reopened in the close callback before dialing; errors after a successful connect (such as a handshake failure) are not re-dialed — the fallback covers only the connect layer, matching Node's convention;
- regression face: a v4-only listener + `localhost` dial must succeed (both plain/TLS legs covered), and the `'::'` dual-stack listen semantics, pinned by the loop group's dial regression.

### timers: event-loop current-state check and recommendation

**Current state (source-verified)**: the dispatch note's README "event loop deferred" has evolved — [architecture.md](/zh/architecture)'s section is now "event loop: first batch (explicit choice, opt-in in scripts)": `require "loop"` has long landed, libuv 1.53.0 statically linked, with eight C faces (timer/fs(+watch)/net(+TLS)/udp/dns/signal/os/process) plus the pure-Lua `loop.http` face; the prepare hook carries `^C` → `interrupted` (exit 130) and the attach poll. **The deferred thing is no longer "whether to have a loop," it is "whether to globalize / enter the REPL"** — the README overview's "event loop deferred" wording was updated to the current state with the 2026-10-04 positioning batch (timers/TCP/Unix/TLS/async fs/signals/subprocesses + script-tail auto-drain).

**Recommendation: enable "script-tail auto-drain," keep deferring "globalization and REPL integration."**

1. **Enable: script-tail auto-drain.** The Node contract is "register a timer and the process stays alive" — `luna script.lua` currently is "main chunk done, exit," and a `loop.setTimeout` without `loop.run()` is silently dropped (a footgun). Recommendation: after the main chunk of `run_script` ends normally, if `loop` has been required and still has live handles, automatically `loop.run()` to drain — exactly Node's "run the event loop to empty, then exit"; `unref` semantics naturally support "present but not holding the door," so no existing contract changes. Implementation lands at the tail of the script path in `luna_main.c` (probe `package.loaded.loop`'s handle count, or loop.c exposes a `luna_loop_maybe_drain()`); `-e` mode same convention; the REPL does not enable it. Risk surface: affects only scripts that "used loop but never called `run()`" — today the handles are dropped; the new behavior executes them, strictly closer to the intent; all existing loop tests must stay green verbatim (they call run themselves, so auto-drain is a no-op for them).
2. **Keep deferring: global `setTimeout`.** Globalization forces the REPL's main loop into the loop world: the line editor's blocking read, the `^C` 130 contract, and the attach polling points are all on the synchronous side; moving those three contracts to save one `require` is not worth the cost. The "drain (nowait) after each REPL evaluation" shape is recorded for the next round in todo, to be enabled when the conditions are ripe (the line editor can read with a timeout or a wake thread can be timed).

**Implementation verification (2026-10-01, batch 8 landed)**:

- **shape**: loop.c exposes `loop.maybeDrain()` (drains only when live handles exist, otherwise a no-op returning false), and `lua/luna.lua`'s `run_script` and `run_eval` call it after the main chunk **ends normally**; a main-chunk error does not drain (aligned with Node's uncaught-exception exit); the REPL does not call it. `-i script.lua` drains first, then enters the console;
- **`^C` during drain follows 130**: `maybeDrain` throws `interrupted` like `run()`, and `exit_code_for` gets 130 — one contract spans the script body and the drain segment;
- **a real bug fixed as a root cause**: the keep-alive prepare hook was previously a ref'd handle — as long as any user handle existed (ref'd or not) it held the loop open, so `run()` with only unref'd handles **never returned** (measured `-e` unref interval + run() hang), conflicting with "unref no longer binds the script" — the hook is now unref'd, so `run()` returns once ref'd handles clear; this makes "all unref'd exits naturally" actually true.

### path (batch 6, pure Lua; win32 face unfrozen and completed 2026-10-11)

Node semantics are pinned function by function against **measured Node v24.21.0** (every function's boundary behavior is compared into test cases); pure Lua, zero dependencies, and the reason no C is used: Node itself is pure JS and libuv has no counterpart. The win32 face was closed out by differential comparison against v24.21.0's local `node` (326 cases: normalize/join/isAbsolute/resolve/relative/dirname/basename/extname/parse×5 fields/format/toNamespacedPath/posix relative, all matching exactly).

**win32 face verification (2026-10-11, the unfreeze item)**:
- **dual-face registration**: `path` is the posix face, `path.posix === path`; `path.win32` is a separate table (`sep="\\"`, `delimiter=";"`);
- **device-root three-way split**: a drive letter (`[A-Za-z]:`), a full UNC (`\\s\\sh`), and the `\\?\`/`\\.\` namespace (only the 4-character prefix is recognized); an incomplete UNC (`//a`) folds as an ordinary root (matching v24: `normalize("//a/b")` → `"\\a\\b\\"`);
- **relative case folding**: resolve then differing only by case → `""`; mixed-case differences compare by `\` segments and the output keeps the original shape; no common separator (different drives) → returns the target's original shape; the posix face was rewritten in the same batch to Node's **character-level algorithm** (`lastCommonSep` semantics — only segments before the common separator step back with `..`, and a root-level truncation returns the target's tail directly), fixing the old segment-difference algorithm's wrong values on boundaries like `relative('/a/b','/b')`;
- **reserved names and CVE protection**: CON/PRN/AUX/NUL/COM1-9/LPT1-9 go through the fold-prevention branch (`normalize("COM1:x")` → `".\\COM1:x"`); CVE-2024-36139's colon-tail protection is retained (`normalize("foo:/bar")` → `".\\foo:\\bar"`); UTF-8 superscript variants (`COM\xb9`) are not listed — Lua's `upper()` cannot reproduce them, recorded;
- **resolve's device semantics**: once a device is claimed, a different-device segment is dropped whole; already absolute with no device, a more-left segment is dropped; a device-relative path trails cwd (the posix host's `'=X:'` environment is always missing, Node's verification fails and it still lands on cwd); no args or a single empty arg with cwd starting with a separator takes the cwd fast path (`/`→`\` returned directly);
- **text operations keep input slashes verbatim**: dirname/basename/extname/parse do not normalize (`dirname("a//b")` → `"a/"`, `parse("\\\\s\\sh").dir == "\\\\s\\sh"`, and `basename("C:/","x")` → `"/"`'s start===end quirk is kept as-is);
- **known deviation (deliberate)**: `toNamespacedPath` raises on non-strings per luna's whole-module contract (Node returns non-strings verbatim); the remaining 326 cases match exactly.

**Implementation verification (2026-10-01, batch 6)**:
- **normalize/resolve collapse consecutive slashes** — this section's first draft said "normalize keeps double leading slashes," which contradicted v24's measurement: `normalize("//a")` → `"/a"`, `resolve("//a","b")` → `"/a/b"` (old Node's double-slash preservation no longer holds); normalize and resolve fork on trailing slashes — normalize keeps (`"foo/"` → `"foo/"`), resolve strips (`resolve("/a/b/")` → `"/a/b"`);
- **dirname is a text operation**: slashes are kept verbatim (`"a//b"` → `"a/"`, `"//a//b"` → `"//a/"`); parse's dir is likewise verbatim (`parse("//x/y").dir == "//x"`);
- **extname's "all-dots prefix" rule**: `".."` and `".bashrc"` have no extension, `"a.."` → `"."` — there must be a non-dot character before the last dot;
- **format's concatenation rules** (Node shares `_format`; the `dir` and `win32` faces are isomorphic): `d = dir || root`; when `d === root` **no separator is added** (`format{dir="/", root="/", base="x"}` → `"/x"` not `"//x"`; `{dir="/a", root="/a"}` → `"/ax"`); when `dir` exists but is not `root`, it joins `d .. "/" .. file` verbatim (`dir="/a/b/"` → `"/a/b//x"`, the trailing slash is not absorbed); only when both are missing does it return file directly; `ext` without a leading dot gets one (`{name="x", ext="lua"}` → `"x.lua"`, `{ext="lua"}` → `".lua"`); an empty-string field is treated as absent (JS truthiness);
- **relative resolves first, then segment difference** — the same path (including trailing-slash differences) returns `""`;
- wrong argument types raise (message `path.<fn>: ...`); the cwd anchor goes through lfs (always present inside `luna`; when detached from the host it falls back to `$PWD`/`"/"` — both legs unreachable while lfs is present, recorded structurally).

### util (batch 6, pure Lua)

- **`util.inspect(value, opts?)`**: value formatting — `depth` (default 2, `math.huge` expands fully, negative collapses at the top), cyclic references `[Circular *1]` (registered per table), long arrays truncated as `... N more items` (Node's breakLength semantics simplified to opts.maxArrayLength/opts.maxStringLength count truncation, no line-breaking in v1), strings quoted and escaped, functions `<function: shortsrc:line>` (via debug.info; C functions fall back to tostring), threads/udata fall back to tostring, keys in the `["a b"]` form per Lua's legal-name rules (Lua keywords get brackets);
- **`util.format(fmt, ...)`**: `%s`/`%d`/`%i`/`%f`/`%x`/`%X`/`%o` (through util.inspect, Node's %o)/`%j` (JSON, dkjson backend, requireable in the same tree)/`%%`; with no format specifier all arguments join with spaces (same as Node); extra arguments are appended;
- **division of labor with `luna.introspect`**: introspect is the REPL's **introspection** (function signatures, help, doc_for, suggest — "how is this function used"); util.inspect is **value formatting** (printing any value in human-readable form — "what does this data look like"). They are not merged; the REPL's `Out[n]` table echo has switched to util.inspect (landed 2026-10-09): table values go through inspect (deterministic key order, the same depth/cycle/truncation markers as the stdlib), scalars and functions keep introspect repr (quoted strings, function signatures), and `%eval` uses the same channel and rendering.
- **not doing**: `promisify`/`callbackify` (Lua has no promise, the contract has no anchor), `types` (the isDeepStrictEqual family).

**Implementation verification (2026-10-01, batch 6)**: number spelling follows JS `String(number)` (integral floats without `.0`, `NaN`/`Infinity` as words); `parseArgs` long/short/inline/`--`/strict/positionals/tokens all compared (tokens.index is 1-based, Lua's ordering); parseEnv's multiline quoted values keep the last duplicate (`export ` prefix stripped); styleText color names + `#RGB`/`#RRGGBB` with SGR wrapping, `replaceCloseCode`'s nested reopen not implemented (simple wrapping); debuglog's callback takes no second argument (Node passes a stream); styleText has no fd1 TTY detection by default (kernel.tty only checks fd0) and is treated as non-TTY unless an explicit `stream` is passed. Error convention: styleText raises on a wrong argument type (the module contract), and an invalid name/hex reports `ERR_INVALID_ARG_VALUE` (the same error-table shape as parseArgs/assert); parseEnv raises on a non-string content. Two C tests pin the shapes (parseEnv 3 groups 33 shapes / stripVT 16 shapes / styleText 15 shapes / debuglog gating and output capture); modules group 108 → 110.

### querystring / url (2026-10-11, pure Lua, differential against Node v24.21.0)

Node's new `url` face (WHATWG `URL`) is pinned to a C++ binding; the legacy face (`parse`/`format`/`resolve`/`resolveObject`) is a pure JS state machine in `lib/url.js` + `lib/internal/url.js` — luna follows the path/events discipline and **ports the legacy face whole**, alongside `querystring` (`lib/querystring.js`, a hard dependency of `url.parse(_, true)` and `url.format`'s query table):

```lua
local qs = require "querystring"
qs.parse("a=1&b=2")                     -- {a="1", b="2"}; '+' becomes a space, duplicate keys aggregate into arrays
qs.parse("a=1&b=2&c=3", nil, nil, {maxKeys=2})  -- default 1000; <=0 = unlimited (Node's same quirk)
qs.stringify({a="b c", b={1,2}})        -- "a=b%20c&b=1&b=2"; numbers/booleans bare-stringed, not encoded
qs.escape("a b")                        -- "a%20b"; !'()*-._~ and alphanumerics not escaped
qs.unescape("a+b%20c%2z")               -- "a+b c%2z" loose decode, a broken % is kept verbatim

local url = require "url"
url.parse("http://u:p@h.com:8080/p?q#h") -- field table: protocol/slashes/auth/host/hostname/
                                         --   port/hash/search/query/pathname/path/href
url.parse("http://a.com/?a=1&a=2", true) -- query goes through querystring.parse
url.parse("//user@host/x?q", false, true) -- slashesDenoteHost
url.format({protocol="http:", hostname="a.com", port=8080, query={a="1"}})
url.resolve("http://example.com/a/b/c", "../f")   -- "http://example.com/a/f"
url.resolveObject("http://a.com/a/b", "c/d")      -- returns a field table, not a string
```

**Implementation verification (168 differential probes, reconciled against Node v24.21.0 case by case)**:
- **parse all fields aligned**: whitespace trimmed, `\` → `/` normalized (slash protocols only), auth taken before the first `@` and unescaped, `simplePathPattern` fast path, host 255 limit truncated to empty, IPv6 bracket stripped, `javascript:` hostless/unsafe, `file:` empty host, autoEscape (`" ' < > ` ``` ` ` space etc. → %XX), a `:` followed by a non-digit does not enter port (falls into pathname, same as Node);
- **href rebuilt by format**: auth encoding goes through `noEscapeAuth` (a few more passes than qs.escape; `u:p` in href is a bare colon, `@` is `%40`); pathname's `#`/`?` escape to `%23`/`%3F`;
- **error convention**: `parse(non-string)` → `url.parse: url must be a string, got N`; a host with a forbidden char or an unclosed IPv6 bracket → `Invalid URL: <original, position 0>` (Node `ERR_INVALID_URL`'s message text);
- **recorded deviations** (probe-verified case by case): `stringify` key order is deterministic byte order (Node is insertion order); an empty Lua table is treated as an empty array — the field is omitted entirely, while Node's `{}` is an object outputting `a=` (language-inherent, cannot distinguish empty array from empty object); `escape(nil)` gives `nil` (Node `String(null)`); `parse`'s auth decode is loose (Node goes through `decodeURIComponent`, a broken %XX throws `URIError`, luna keeps it verbatim); DEP0169/DEP0170 deprecation warnings not emitted; NBSP/ZWNBSP do not count as whitespace;
- **explicitly not proposed**: WHATWG `URL`/`URLSearchParams` (a C++ binding face, no promise/Rust structure to compare), `domainToASCII`/`domainToUnicode` (IDNA, non-ASCII hostnames kept verbatim), `pathToFileURL`/`fileURLToPath`, `urlToHttpOptions`, the `Url` constructor (`parse`'s result is an equivalent object);
- 6 C tests (querystring 3 + url 3) pin the shapes; modules group 98 → 104.

### process (2026-10-11, pure Lua, shape-compared against Node v24.21.0)

Node's `process` is a C++ binding face, but the common synchronous subset (pid/argv/env/cwd/chdir/exit/exitCode/uptime/hrtime/umask/memoryUsage/nextTick/stdio) has no hard C dependency — luna follows the querystring discipline and **aggregates existing faces in pure Lua** (global `arg`/`os` + `lfs` + `kernel`'s pid/millis/umask/tty/write), zero new C:

```lua
local p = require "process"
p.pid; p.ppid; p.platform; p.arch       -- linux/x64 (host detection best-effort, nil if undetectable)
p.argv                                  -- {[0]=interpreter, [1]=script/"[eval]", [2..]=arguments}
p.execPath; p.argv0; p.execArgv         -- argv0 = raw argv[0] = interpreter (not the script); execArgv always empty
p.env.HOME                              -- getenv proxy + this process's overlay layer (no enumeration)
p.cwd(); p.chdir("/tmp")                -- lfs face, failure raises with a process.chdir: prefix
p.exitCode = 3; p.exit()                -- default nil (Node undefined); no-arg exits with exitCode
p.hrtime(); p.hrtime.bigint()           -- [seconds, nanoseconds] table + difference face; bigint returns an integer
p.umask()                               -- no-arg pure query (kernel.umask's default sets 0077, wrapped)
p.nextTick(fn)                          -- goes through setImmediate when loop is present, otherwise immediately
p.stdout.write("x")                     -- kernel.write funnel, returns true; non-string raises
p.memoryUsage().heapUsed                -- Lua heap bytes
```

**Implementation verification (29-shape comparison, reconciled against Node v24.21.0 item by item)**:
- **argv layout conversion**: Lua's `arg[0]` is the script, Node's `argv[0]` is the interpreter — `process.argv = {[0]=interpreter, [1]=script/`[eval]`, [2..]=arguments}`; script vs binary distinguished by extension + ELF/Lua-bytecode magic; an embedded host (no full `arg` table) falls to the eval context, outputting `[eval]`;
- **exitCode three states**: `exit()` no-arg uses `exitCode`; `exitCode` defaults to nil (actual exit 0, same as Node's undefined), and `exit(7)` takes explicit priority;
- **recorded deviations** (probe-verified item by item): hrtime/uptime resolve to milliseconds (kernel.millis; Node is nanoseconds), `hrtime.bigint` returns an integer, not a bigint; memoryUsage reports only `heapUsed` (collectgarbage count; Node's five keys rss/external/arrayBuffers/heapTotal/heapUsed); env is not enumerable (Lua cannot walk environ), and the overlay is invisible to `os.execute` subprocesses; `process.type` is an extension (Node undefined); `versions` is `{lua=..., luna=...}`, not Node's key set; ppid goes through /proc/self/stat;
- **explicitly not proposed**: abort/kill/signal handling (owned by loop.process), title/config/dlopen/binding, cpuUsage/resourceUsage (owned by loop.os), emitWarning, directory-handle faces beyond chdir;
- 10 C tests pin the shapes (one process-surface group); modules group 104 → 105; the harness also registers the kernel C module (same as luna_main; the rocks guard test's block hides kernel, preserving its "kernel absent" premise).

### console (2026-10-11, pure Lua, shape-compared against Node v24.21.0)

Node's console is a logging layer over a C++ binding face, but the whole surface is formatting + writing + counting — pure Lua can aggregate it: the log family formats through `util.format`, cells render through `util.inspect`, output goes through the process module's stdout/stderr (the `kernel.write` funnel / direct write flushed immediately), timing uses `kernel.millis`, and `table` is a home-built ASCII grid renderer. The `Console(stdout, [stderr])` constructor accepts any stream table with `write(s)`, and the singleton hangs on `process.stdout/stderr` (same shape as Node's `console.Console(stream)`):

```lua
local c = require "console"
c.log("a %s %d", "x", 42)        -- util.format convention
c.warn("w %d", 1)                -- same shape, stderr side
c.assert(false, "m")             -- "Assertion failed: m" (falsy warns, does not throw)
c.assert(0, 42, "x")             -- non-string first arg: "Assertion failed 42 x"
c.time("t"); c.timeEnd("t")      -- "t: 0ms"; missing label defaults to "default"
c.count("n"); c.countReset("n")  -- "n: 1"; no label warns
c.group("g"); c.log("in")        -- one level, two-space indent
c.trace("here")                  -- "Trace: here" + stack
c.dir({a=1, b={c=2}}, {depth=0}) -- inspect passthrough
c.table({{x=1,y=2},{y=3,z=4}}, {"y","z"})  -- ASCII grid, missing cells blank
```

**Implementation verification (70+ shape comparisons, reconciled against Node v24.21.0 case by case)**:
- **grid convention pinned**: single-space margins, all left-aligned, `┌┬┐├┼┤└┴┘` box drawing, the `(index)` header, and column width taken as the maximum code-point count; row shape is binary — `#t > 0` is an array row (index starting at 0), otherwise an object row (key names become columns); a function row is an empty-key object row (the Values column appears only when a scalar row exists);
- **properties semantics**: the columns are the properties list (given order, repeatable, independent of row content); the selection applies only to table rows, and numeric properties go by array index (the probe convention `[...],[b]` column present but cell empty);
- **assert's three conventions**: no argument `Assertion failed`, string first argument with a colon `Assertion failed: <format>`, non-string first argument with a space `Assertion failed <format>`; the falsy set matches Node (`nil`/`false`/`0`/`''`/`NaN`, whereas Lua natively only `nil`/`false`);
- **recorded deviations**: `time`/`timeLog`/`timeEnd` resolve to milliseconds (integer milliseconds output `5ms`, same shape as Node; sub-millisecond outputs `0`); cells and `dir` use `util.inspect` (Lua source shape, deterministic key order — Node's JS inspect is single-quoted with the `[ 1, 2 ]` shape), column width counts UTF-8 code points (CJK/emoji cells will be narrow); Lua has no Set/Map, so no `(iteration index)` column; object-row key order is byte order (Node is insertion order); the trace stack is `debug.traceback`'s shape; warnings lack the `(node:NNNN)` process prefix and the trace-warnings hint; `clear` cannot detect stdout TTY and is always a no-op (same behavior as Node without a TTY);
- **explicitly not proposed**: the Inspector/color face (`profile`/`profileEnd`/`timeStamp` always no-op, same shape as Node without an Inspector), and `console.Console`'s color/inspectOptions face;
- 8 C tests pin the shapes (singleton funnel capture + self-supplied stream isolation, millisecond quantities matched by pattern to avoid timing jitter); modules group 105 → 106.

### assert (2026-10-13, pure Lua, shape-compared against Node v24.21.0)

Node's `assert` is a pure JS assertion library; the whole surface is comparisons + error construction — pure Lua can aggregate it: scalar comparisons go through `loose_eq` (JS `==` coercion semantics) / `strict_eq` (Object.is semantics), deep comparison recursively compares key sets, and the error object is a table with `code`/`actual`/`expected`/`operator`/`message` fields (Node's `AssertionError` shape):

```lua
local a = require "assert"
a.assert(value, message)          -- falsy throws "The expression evaluated to a falsy value:\n\n  assert(<inspect(v)>)\n"
a.ok(value)                       -- alias of assert
a.equal(actual, expected, msg)    -- == coercion; failure "<inspect(a)> == <inspect(b)>"
a.notEqual(actual, expected, msg) -- != coercion
a.strictEqual(actual, expected, msg)    -- Object.is (NaN==NaN, -0≠0, no coercion)
a.notStrictEqual(actual, expected, msg)
a.deepEqual(actual, expected, msg)      -- loose deep (coercion, -0==0, NaN==NaN)
a.notDeepEqual(actual, expected, msg)
a.deepStrictEqual(actual, expected, msg) -- strict deep (-0≠0, no coercion)
a.notDeepStrictEqual(actual, expected, msg)
a.fail(message)                   -- "Failed" or a custom message
a.ifError(err)                    -- "ifError got unwanted exception: <err>"
a.throws(fn, error?, message?)    -- expects a throw; error matches: string (message substring)/table (message|code|name fields)/function (constructor)
a.doesNotThrow(fn, error?, message?) -- expects no throw
```

**Implementation verification (5 groups of shape comparisons, reconciled against Node v24.21.0 item by item)**:
- **the truthy set matches Node**: falsy = `nil`/`false`/`0`/`''`/`NaN` (Lua natively only `nil`/`false` false — a language boundary);
- **loose equality**: `loose_eq` implements JS `==` coercion (boolean→number, string→number), so `1 == '1'` is true; `strict_eq` implements Object.is (NaN equal, `-0 ≠ 0` distinguished via `string.format("%.17g")`, no coercion);
- **deep comparison**: `deepEqual` is loose (scalars coerce, tables recursively compare key sets, a missing key is unequal); `deepStrictEqual` is strict (scalars use Object.is, tables recursively compare key sets); `-0` and `0` are equal in `deepEqual` and unequal in `deepStrictEqual`;
- **error object**: a table with `code`/`actual`/`expected`/`operator`/`message` fields, and `__tostring` returns message; `operator` takes `==`/`!=`/`strictEqual`/`!==`/`deepEqual`/`notDeepEqual`/`deepStrictEqual`/`notDeepStrictEqual`/`fail`/`ifError`/`throws`/`doesNotThrow`;
- **throws' error matching**: a string second argument is the assertion message (same as Node); a table is a validation object compared by `message`/`code`/`name` fields; a function is a constructor compared by `type`; regexps are not supported (Lua has no built-in regex, recorded);
- **recorded deviations**: the error object is a table, not a Lua error string (Node's `AssertionError` instance has no counterpart in Lua, while the field convention is identical); `ifError` directly concatenates a string error (same as Node); `throws`' error matching does not support regexps;
- **explicitly not proposed**: the async face (`rejects`/`doesNotReject`) — luna's promise contract is the `task` module; async assertions will be separate on the task face;
- 1 C test pins the shapes (5 groups of `assert_string_equal` covering the truthy set/scalar comparisons/deep comparisons/fail+ifError+throws+doesNotThrow/throws error matching); modules group 106 → 107.

### events (batch 6, pure Lua)

```lua
local EventEmitter = require "events".EventEmitter
local em = EventEmitter.new()
em:on("tick", function(n) ... end)      -- on/addListener/once/prependListener/prependOnceListener
em:once("done", fin)
em:emit("tick", 1)                      -- → true/false (any listener)
em:off("done", fin)                     -- off/removeListener; removeAllListeners(ev?)
em:listeners("tick")                    -- array
em:listenerCount("tick")
em:setMaxListeners(20)                  -- default 10 (Events.defaultMaxListeners), a stderr warning once over the limit
-- 'newListener'/'removeListener' built-in events same as Node; emit('error') with no listener → error() throws (same as Node)
```

~150 lines of pure Lua; the no-C reason is the same as path (no C library to bind; Node is pure JS); it is the foundation of stream, one batch before it.

**Implementation verification (2026-10-01, batch 6, measured Node v24.21.0)**:
- `events.new()` constructs, and `require("events").EventEmitter` is the module-table alias (`.defaultMaxListeners = 10` changeable, affecting all subsequent add checks);
- **newListener fires before the add** (its callback's `listenerCount` does not include the new one), **removeListener fires after the removal**;
- **self-removal and re-attach during emit** act on the live table and this turn still calls them (Node's snapshot-dispatch semantics); a listener added during emit is not called this turn;
- **`error` with no listener raises** (same as Node); the payload is passed verbatim if a table, otherwise wrapped one layer (`('boom')` in the message);
- **over-limit warning** goes to stderr once per (emitter, event name), reset on removal/re-set — it only warns, never throws;
- `removeAllListeners(ev)` fires `removeListener` one by one (LIFO), and a no-arg clear fires none.

### stream (batch 7, pure Lua, built on events)

Node's `stream` is pure JS; luna implements a **v1 narrow face** with the same discipline as path/events — no C library to bind, Node is pure JS. The v1 face is deliberately narrowed: no webstreams (`ReadableStream` family), no async iterators (`for ... in s:lines()`'s Lua iterator form is actually handy, recorded as optional), no `setEncoding`/`cork` family of small faces.

**Implementation verification (2026-10-01, batch 7, Node v24 semantics compared)**:
- construction is factory functions: `stream.readable/writable/duplex/transform(opts)` (`stream.Readable` etc. capital shapes are aliases of the same function); `end` is spelled `s:end_(chunk, cb)` due to the keyword conflict, and the **sock-equivalent** `s["end"]` shape also works;
- **start-of-flow timing**: the stream turns flowing only after the first `data` listener (via the overridden `on`/`once`) actually lands — events' `newListener` fires **before** the listener lands, so opening the stream in that hook would throw the first few chunks to the air; this differs from Node's implementation path (resumeScheduled one tick later) but is semantically the same;
- **pull-source loop**: `_read` is called when the internal buffer is empty and flowing; a synchronous no-output stops and waits for the next `push` trigger; `push` inside `_read` only books and does not deliver (guards against unbounded recursion), with delivery wrapped up by the outer loop; `push(nil)` is EOF, and a `push(chunk)` after EOF fires an `error` event;
- **two-way backpressure**: on the consuming side, `write` returning false pauses the source and the target's `drain` resumes it; on the producing side, `push` returning false makes the source back off, and `_read` is re-fed as the buffer empties — `pushReadable` + a self-held queue is the adaptation shape of loop.http onData ("watch remaining" = this return value plus this refill point);
- **autoDestroy's two-face criterion**: a pure stream's single face (`end`/`finish`) arriving triggers `destroy`→`close`; a duplex/transform requires `end` **and** `finish` both arriving before `close` (same as Node) — after a sock EOF you still need `end_()` to close the write face; a transform's `finish` precedes the read side's `end` (flush completes → finish, buffer empties → end);
- **deliberate fork from a bare Node pipe**: a source `error` unpipes and then `destroy(same error)` to the destination (Node's bare pipe does not touch the destination — that is `pipeline()`'s contract; luna folds that contract into `pipe`, one call covers it); a destination `error` only unpipes, the source is not torn down; `opts.end == false` turns off the source `end` → destination `end_()` linkage;
- **accounting simplification**: strings count in bytes, other values count as 1 (the merged always-on objectMode face); `highWaterMark` defaults to 16384 and is a threshold, not a byte-exact count; `destroy` settles pending write callbacks (**one in-flight + several queued**) with `"stream destroyed"`, and `destroy(err)` without an error listener raises per events semantics;
- three adapters: `duplexFromSock(sock)` (resident read callback → push, write forwarded, `_destroy` closes the sock, a read error goes through the error→destroy linkage), `readableFromChunks(chunks)` (a string or chunk table, one block pushed on demand), `pushReadable(opts)` (pure push face, `push` public); modules group 8 use cases + the loop group's 1 real sock echo integration (write forward/EOF→end/autoDestroy→close) pinned.

### os completion (batch 8, C inside loop.c)

`loop.os` additions, names aligned with Node `os` (no top-level `os` module — Lua's global `os` is the official standard library, and shadowing it violates the "official semantics unchanged" discipline):

| Addition | Base | Node counterpart |
| --- | --- | --- |
| `loop.os.arch()` | `uv_os_uname()->machine`, mapping `x86_64→"x64"` and `aarch64→"arm64"` (Node's names, not the uname text) | `os.arch()` |
| `loop.os.release()` | `uv_os_uname()->release` | `os.release()` |
| `loop.os.EOL` | the constant `"\n"` (POSIX platform face) | `os.EOL` |
| `loop.os.userInfo()` | `uv_os_get_passwd`: `{username, uid, gid, shell, homedir}` | `os.userInfo()` |
| `loop.os.availableParallelism()` | `uv_available_parallelism` | Node ≥18 same name |

**Implementation verification (2026-10-01, batch 8)**: `arch`'s mapping accepts `x86_64`/`amd64`→`"x64"`, `aarch64`/`arm64`→`"arm64"`, the rest of uname's machine verbatim; `EOL` is a field constant `"\n"` (setfield after `luaL_newlib`, not a function); `userInfo`'s keys `{username, uid, gid, shell, homedir}` — a missing shell means **key absent** (nil), and uid/gid are numbers; all five return synchronously and `luaL_error` on failure, same as the existing os face. The REPL's `os` global is unaffected (the official standard library's turf, not shadowed).

### child_process sugar (batch 8)

- `process.exec(cmd, opts?, cb)`: sugar for `run("sh", {"-c", cmd}, opts, cb)` (Node `exec`'s shell semantics; stdout/stderr aggregation as before); `execFile` is just `run` itself, named in the docs;
- `process.execSync(cmd) -> stdout`: the `popen` synchronous face (`LUA_USE_POSIX` already on) — reads all of stdout, takes the exit code from `pclose`'s wait status, and a nonzero (or death by signal) raises — Node `execSync`'s shape (the same semantics as `io.popen`'s `close()` triple);
- **not doing** `spawnSync`'s three-stream synchronous face (scenarios that a popen single stream + `/dev/null` redirect covers go through execSync; a true three-stream is `process.spawn`'s turf).

**Implementation verification (2026-10-01, batch 8)**: both pieces land in the **C face `process_funcs`** (inside loop.c), not the table's initial "Lua layer" — the sugar must appear on the same face as the `loop` module: the CLI, the embedded test harness (`luaL_requiref` opening loop directly), and `lua/luna.lua` all get the same `loop.process` table, and a Lua-only layer would miss the first two; `exec` builds `sh, {"-c", cmd}, opts, cb` on the stack and then **calls `l_process_run` verbatim** (aggregation/delivery/cwd semantics zero fork), and `execSync` goes through `popen`/`pclose` + `WIFEXITED`/`WEXITSTATUS`, with the 20000-byte-scale output across a 4 KiB read loop, and both the `exit 3` and `kill -9 $$` raise legs have test cases.
