# stdlib · util

Node `util`'s `format`/`inspect`/`parseArgs`/`parseEnv`/`stripVTControlCharacters`/`styleText`/`debuglog` face (`promisify`/`types` have no Lua contract anchor and are explicitly out of scope). Semantics are pinned to measured Node v24.21.0; the selection record is in [Node alignment](/zh/node-parity).

## API

| Call | Description |
| --- | --- |
| `util.format(fmt, ...)` | printf-style: `%s %d %i %f %j %o %O %c` and `%%`; the extensions `%x`/`%X` for hexadecimal |
| `util.inspect(value, opts?)` | Renders a value as a **Lua source shape**, key-sorted for determinism; `opts.depth` (default 2), `opts.maxArrayLength` (default 100), `opts.maxStringLength` (default 10000) |
| `util.parseArgs(config)` | CLI argument parsing: long/short options, inline values, `--` termination, strict, positionals, `tokens` |
| `util.parseEnv(content)` | Parses `.env` text into a key/value table (comments / `export ` prefix / quoted values can span lines / `#` truncation / last duplicate wins) |
| `util.stripVTControlCharacters(str)` | Strips ANSI/VT control sequences (OSC and CSI; a non-terminator ESC is kept verbatim) |
| `util.styleText(fmt, text, opts?)` | Color name / `#RGB` / `#RRGGBB` / array → wrapped in SGR codes; returns text unchanged on non-TTY by default |
| `util.debuglog(section)` | Returns a function; gated by `NODE_DEBUG`, writing `SECTION PID: msg` to stderr when it matches |

`format` details (aligned with measured Node v24): `%d`/`%f` do not truncate, `%i` truncates toward zero; with no format specifier, arguments are appended separated by a space; unknown conversions are kept verbatim; `%j` goes through [json](/zh/stdlib/json) encoding.

The per-item verification and deviation ledger for `parseArgs`/`parseEnv`/`styleText`/`debuglog` is in the util section of [Node alignment](/zh/node-parity).

## Usage

```lua
local util = require "util"
print(util.format("%s has %d points (%x in hex)", "luna", 255, 255))
print(util.format("%i %f %j", 3.99, 3.99, { ok = true }))
```

```text
luna has 255 points (ff in hex)
3 3.99 {"ok":true}
```

```lua
local util = require "util"
print(util.format("no placeholders", "with", 2, "extras"))
```

```text
no placeholders with 2 extras
```

`inspect` outputs Lua shape (`{}` rather than JS `[]`), and key sorting guarantees the same rendering for the same table every time:

```lua
local util = require "util"
print(util.inspect({ name = "luna", version = 8, nested = { 1, 2, 3 } }))
```

```text
{ name = "luna", nested = { 1, 2, 3 }, version = 8 }
```

`parseArgs`:

```lua
local util = require "util"
local r = util.parseArgs({
  options = { name = { type = "string", short = "n" }, verbose = { type = "boolean" } },
  args = { "-n", "luna", "--verbose" },
})
print(r.values.name, r.values.verbose)
```

```text
luna	true
```

`parseEnv` (quoted values can span lines; trailing text after a closing quote on the same line is discarded; an unquoted value truncates at the first `#`):

```lua
local util = require "util"
local env = util.parseEnv('# note\nexport HOST=localhost\nPORT=8321 # inline\n')
print(env.HOST, env.PORT)
```

```text
localhost	8321
```

`styleText` (returns text unchanged on non-TTY by default; with `validateStream = false` or `stream.isTTY = true` it emits codes; an array opens codes in order and closes them in reverse):

```lua
local util = require "util"
print(util.styleText("red", "x", { validateStream = false }))
print(util.styleText({ "bold", "red" }, "x", { validateStream = false }))
```

```text
[31mx[39m
[1m[31mx[39m[22m
```

(Each `[31m` etc. in the output is preceded by an ESC byte, omitted here for display.)

`debuglog` (a no-op when `NODE_DEBUG` is unset; entries are case-insensitive, `*` is everything, a trailing `*` is a prefix wildcard):

```lua
local util = require "util"
local debuglog = util.debuglog("mysection")
debuglog("hello %d", 42)   -- with NODE_DEBUG=mysection: MYSECTION 12345: hello 42
```

`stripVTControlCharacters`:

```lua
local util = require "util"
print(util.stripVTControlCharacters("\27[31mred\27[0m"))
```

```text
red
```

## Boundary rules

- Number spelling follows JS `String(number)`: an integral float has no `.0` (`3.0` prints as `3`), and `NaN`/`Infinity` print as their words.
- `inspect`'s table key sorting is deliberate: `pairs` order is nondeterministic and cannot align with Node's insertion order — determinism wins, and it is recorded as an adaptation, not a defect.
- `parseArgs`'s `tokens.index` uses Lua 1-based ordering (Node is 0-based); `config.args` defaults to the global `arg` table.
- `styleText` treats output as non-TTY by default (luna's stdout has no TTY detection), emitting color codes only with an explicit `validateStream = false` or `stream = { isTTY = true }`; nested reopen (`replaceCloseCode`) is not implemented and is recorded as simple wrapping.
- `debuglog`'s `NODE_DEBUG` entries are **not whitespace-trimmed** (a trailing space does not match, same as Node); message formatting uses the same `util.format` convention.
- `parseEnv` does not strip quotes from keys and strips `\r` at the file level (CRLF and lone CR alike); the only raise is a non-string `content`.

