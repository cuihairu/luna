# stdlib · yaml

YAML 1.1 encode/decode, multi-document streams. Backed by [lyaml](https://github.com/gvvaughan/lyaml) 6.2.9 (a staged copy in deps, with the require path adapted).

## API

| Call | Description |
| --- | --- |
| `yaml.decode(str, opts?)` | Parses the first document; `opts.nullval = yaml.null` can turn null into a sentinel (defaults to `nil`) |
| `yaml.decodeAll(str, opts?)` | Parses all `---`-separated documents, returns an array; opts same as `decode` |
| `yaml.encode(value, opts?)` | Encodes to YAML text; `opts.anchors = true` enables automatic anchors (shared references emit `&aN`/`*aN`, and cycles can be encoded) |
| `yaml.null` | The encode-side null sentinel: on `encode` it becomes `~`; on the decode side `~`/empty/`null` all become `nil`, so the sentinel does not flow back |
| `yaml._VERSION` | The lyaml version string |

## Usage

```lua
local yaml = require "yaml"
local t = yaml.decode([[
name: luna
features:
  - repl
  - serve
nested:
  deep: true
]])
print(t.name, t.features[2], t.nested.deep)
```

```text
luna	serve	true
```

Multi-document stream:

```lua
local yaml = require "yaml"
local docs = yaml.decodeAll("---\nok: true\n---\nok: false\n")
print(#docs, docs[1].ok, docs[2].ok)
```

```text
2	true	false
```

Encoding (lyaml style, `---`/`...` document markers wrapped; multiple keys at one level follow `pairs`, possibly differing per run):

```lua
local yaml = require "yaml"
io.write(yaml.encode({ list = { 1, 2 } }))
io.write(yaml.encode({ map = { a = 1 } }))
```

```text
---
list:
- 1
- 2
...
---
map:
  a: 1
...
```

null is asymmetric on the two sides: the encode side expresses it with the `yaml.null` sentinel (becoming `~`); the decode side by default turns `~`, an empty value, and `null` all into `nil`, indistinguishable from "field absent" — to distinguish, pass `opts.nullval` to bring the sentinel back:

```lua
local yaml = require "yaml"
local t = yaml.decode("value: ~\n")
print(t.value, t.value == nil)
local u = yaml.decode("value: ~\n", { nullval = yaml.null })
print(u.value == yaml.null)
print(yaml.encode({ v = yaml.null }))
```

```text
nil	true
true
---
v: ~
...
```

Automatic anchors (off by default): by default a table referenced twice is encoded as two copies, and a self-referencing cycle directly errors with `yaml: cyclic table reference`; `opts.anchors = true` emits a `&aN` anchor at a multiply-referenced table's first occurrence and a `*aN` alias at each repeat, and the decode side resolves aliases back to shared references — reference identity and cycle structure round-trip verbatim:

```lua
local yaml = require "yaml"
local shared = { 1, 2 }
io.write(yaml.encode({ shared, shared }, { anchors = true }))
local t = yaml.decode(yaml.encode({ shared, shared }, { anchors = true }))
print(t[1] == t[2])
local cyc = {}
cyc.self = cyc
io.write(yaml.encode(cyc, { anchors = true }))
local back = yaml.decode(yaml.encode(cyc, { anchors = true }))
print(back.self == back)
```

```text
---
- &a1
  - 1
  - 2
- *a1
...
true
--- &a1
self: *a1
...
true
```

Anchor names are numbered in discovery order (a1, a2, …), and with multiple shared tables they follow `pairs` like same-level key order (possibly differing per run); without a shared table the output is byte-for-byte identical to the default mode. lyaml's original pre-declared form `opts.anchors = { name = value }` keeps its meaning (the two forms are distinguished by type).

## Boundary rules

- YAML 1.1 convention (the subset lyaml implements): boolean literals such as `yes/no/on/off` are handled per 1.1 rules; projects needing strict 1.2 should note the difference.
- `decode` takes only the first document; for a multi-document stream use `decodeAll`.
- null asymmetry (above): the encode side's null can only be the `yaml.null` sentinel; the decode side defaults to `nil`, and only `opts.nullval = yaml.null` brings the sentinel back.

