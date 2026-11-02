# stdlib · json

JSON encode/decode. Backed by [dkjson](https://dkolf.dk/dkjson) (pure Lua, UTF-8 aware, zero dependencies). The Node ecosystem's `parse`/`stringify` aliases are provided too, so both naming styles work.

## API

| Call | Description |
| --- | --- |
| `json.encode(value, opts?)` | Encodes to a JSON string; `opts` passes through to dkjson (`indent`/`keyorder`, see the next section) |
| `json.decode(str, pos?, nullval?)` | Decodes from `pos` (default 1); bad data returns `nil, err` — `err` is the character position of the failure (1-based, dkjson's convention) |
| `json.parse(str)` | Node-style alias of `decode` |
| `json.stringify(value)` | Node-style alias of `encode` |

## encode options

`opts` passes through to dkjson verbatim; the two most used:

- **`indent = true`**: multi-line pretty output (two-space indent);
- **`keyorder = {...}`**: object keys are output in the order of the given table. Lua's `pairs` key order is not stable across processes; to make encoded output **byte-for-byte reproducible** (test assertions, files to diff/hash), this is the answer:

```lua
local json = require "json"
print(json.encode({b=1,a=1}, {indent=true, keyorder={"a","b"}}))
```

```text
{
  "a":1,
  "b":1
}
```

Without `keyorder`, key order follows `pairs` — it can differ per run, so don't assert output order; keys listed in the table follow the table order, the rest still follow `pairs`.

## Usage

Arrays restore in order; objects become hash tables with key order following `pairs` — **can differ per run**; for key-order-sensitive output, assemble keys one by one or use arrays:

```lua
local json = require "json"
print(json.encode({ "luna", true, 42 }))
local text = json.encode({ name = "luna", tags = { "lua", "repl" }, ok = true })
print(text:find("\"name\":\"luna\"", 1, true) ~= nil,
      text:find("\"tags\":[\"lua\",\"repl\"]", 1, true) ~= nil)
```

```text
["luna",true,42]
true	true
```

```lua
local json = require "json"
local t = json.decode('{"n": 3, "pi": 3.14, "nested": [1, {"deep": true}]}')
print(t.n, t.pi, t.nested[2].deep)
```

```text
3	3.14	true
```

Bad data returns `nil, err` and does not throw — `err` is not a message string but the character position of the failure (`{"n": ` is 6 characters, breaking at position 7):

```lua
local json = require "json"
local v, err = json.decode('{"n": ')
print(v, err)
```

```text
nil	7
```

`stringify`/`parse` are synonymous aliases:

```lua
local json = require "json"
print(json.stringify({ x = 1 }) == json.encode({ x = 1 }))
print(json.parse('[1, 2, 3]')[2])
```

```text
true
2
```

## Boundary rules

- JSON `null` decodes to Lua `nil` — "value is null" and "field absent" are indistinguishable, so judge emptiness by business logic; on the encode side a `nil` field is simply absent, since a plain table cannot express JSON null.
- Numbers: integers stay integers; beyond precision, Lua numeric semantics apply.
- Encoding an invalid value (a function, a cyclic table) **throws** (catch with `pcall`; the message looks like `type 'function' is not supported by JSON.`), unlike `decode`'s `nil, err` convention.
