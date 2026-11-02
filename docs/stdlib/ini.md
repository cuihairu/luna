# stdlib · ini

Classic .ini encode/decode. LPeg-grammar-driven (luna bundles lpeg, zero new dependencies).

## API

| Call | Description |
| --- | --- |
| `ini.decode(str, opts?)` | Parses into a table: `[name]` sections, duplicate sections merged, keys before a section land on the root table; with `opts.cast` true, numbers and lowercase `true`/`false` are cast |
| `ini.encode(tbl, opts?)` | Encodes back to ini text; `opts.eol` is the line terminator (default LF) |

Dialect: full-line comments (`#` or `;`, optionally leading whitespace), **no inline comments** — a `#` inside a value is data; `"..."` quoted values support `\"` and `\\` escapes, other backslashes verbatim; a duplicate key takes the last; a key does not start with `[` and contains no `=`/newline, otherwise it falls back to parsing first as a section line, then as a key line.

## Usage

```lua
local ini = require "ini"
local conf = ini.decode([[
# global default
debug = true
[server]
host = 127.0.0.1
port = 8080
[log]
level = info
]])
print(conf.debug, conf.server.host, conf.server.port, conf.log.level)
```

```text
true	127.0.0.1	8080	info
```

Without `opts.cast`, everything is a string (above, `debug` is the string `"true"`); with cast it converts:

```lua
local ini = require "ini"
local conf = ini.decode("debug=true\nport=8080\n", { cast = true })
print(conf.debug, type(conf.debug), conf.port, type(conf.port))
```

```text
true	boolean	8080	number
```

Encoding is symmetric (both section order and intra-section key order follow `pairs`, possibly differing per run; for deterministic text keep one key per layer or assemble keys one by one):

```lua
local ini = require "ini"
io.write(ini.encode({ server = { host = "127.0.0.1" } }))
```

```text
[server]
host = 127.0.0.1
```

Bad data returns `nil, err` (with line and column):

```lua
local ini = require "ini"
local t, err = ini.decode("[sec\nkey=val\n")
print(t, err)
```

```text
nil	ini: section header missing ']' at line 1, column 5
```

## Boundary rules

- `cast` converts only numbers and `true`/`false` (lowercase); other literals stay strings.
- Encoding does not include comments — comments are a parse-time loss, and encode makes no round-trip guarantee.

