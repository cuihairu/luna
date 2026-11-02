# stdlib · toml

TOML encode/decode. `decode` goes through the C face ([tomlc17](https://github.com/cktan/tomlc17), a strict implementation of TOML v1.1, pinned in deps); `encode` is luna's own Lua face (tomlc17 carries no encoder).

## API

| Call | Description |
| --- | --- |
| `toml.decode(str)` | Parses; values map flatly to Lua, **datetime decodes to a component table** |
| `toml.encode(tbl)` | Encodes; scalar keys first, sub-tables after (a TOML syntax rule), a pure table array renders as `[[header]]`, everything else inline |

The datetime component table: a date `{year, month, day}`, a time `{hour, minute, second, secfrac?}`, a datetime has both, and an offset datetime additionally carries `offset` (minutes from UTC, `Z` is 0). `secfrac` appears only when the sub-second component is a nonzero fraction (`.0` does not produce one).

## Usage

```lua
local toml = require "toml"
local t = toml.decode([[
title = "luna"
tags = ["lua", "repl"]

[owner]
name = "cui"
since = 2024-01-15
]])
print(t.title, t.tags[2], t.owner.name, t.owner.since.year, t.owner.since.month)
```

```text
luna	repl	cui	2024	1
```

Encoding: scalars first, sub-tables after (a TOML syntax rule); a pure table array goes through `[[header]]`:

```lua
local toml = require "toml"
io.write(toml.encode({
  name = "demo",
  servers = { { port = 1 }, { port = 2 } },
}))
```

```text
name = "demo"

[[servers]]
port = 1

[[servers]]
port = 2
```

(At one level, multiple keys follow `pairs`, possibly differing per run; table blocks are separated by a blank line. The example keeps one key per layer, so its output is deterministic.)

Bad data returns `nil, err` (tomlc17 reports only a line, no column):

```lua
local toml = require "toml"
local t, err = toml.decode("key = ")
print(t, err)
```

```text
nil	toml: missing value at line 1
```

## Boundary rules

- A table that "looks like a datetime" is encoded as a datetime — triggered when all keys come from the component set and there is a `year` (or `hour`); a plain data table naturally does not satisfy this, so it cannot be mistaken.
- Error messages look like `toml: <reason> at line N`, with no column number (a backend limitation, unlike csv/ini's line-and-column).

