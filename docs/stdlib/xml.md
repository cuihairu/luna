# stdlib · xml

XML's DOM trio (`decode`/`encode`) plus a SAX passthrough. Backed by expat (via the `lxp` binding of lua-expat).

## API

| Call | Description |
| --- | --- |
| `xml.decode(str)` | Parses to a DOM; an element = `{ tag, attrs, kids }`, and text nodes are plain strings inside `kids` |
| `xml.encode(node, opts?)` | Encodes a DOM back to XML text; `opts.indent = N` gives newline-indented pretty output |
| `xml.sax(handlers)` | Returns an lxp parser (`:parse(str)` feeds segments, a no-arg `:parse()` ends it); callback names are lxp's, and the first argument is always the parser: `StartElement(p, name, attrs)` / `EndElement(p, name)` / `CharacterData(p, str)` |

Attribute values are always strings; namespace prefixes are preserved verbatim (`<x:a>` has tag `"x:a"`).

## Usage

```lua
local xml = require "xml"
local doc = xml.decode('<root id="1"><name>luna</name>rocks<nested/></root>')
print(doc.tag, doc.attrs.id, doc.kids[1].tag, doc.kids[1].kids[1], doc.kids[2])
```

```text
root	1	name	luna	rocks
```

Encoding is symmetric (text and attributes auto-escaped):

```lua
local xml = require "xml"
print(xml.encode({ tag = "msg", attrs = { from = "luna" },
                   kids = { 'fish & chips <today>' } }))
```

```text
<msg from="luna">fish &amp; chips &lt;today&gt;</msg>
```

SAX: when you do not want a DOM, go event-stream (callback names and the first argument are lxp's; feed a no-arg `parse()` to end the stream):

```lua
local xml = require "xml"
local p = xml.sax({
  StartElement = function(_, name) print("open", name) end,
  CharacterData = function(_, s)
    if #s > 0 then print("text", s) end
  end,
  EndElement = function(_, name) print("close", name) end,
})
p:parse("<a><b>hi</b></a>")
p:parse()
```

```text
open	a
open	b
text	hi
close	b
close	a
```

Bad data returns `nil, err` (line and column from expat):

```lua
local xml = require "xml"
local doc, err = xml.decode("<a><b></a>")
print(doc, err)
```

```text
nil	xml: mismatched tag at line 1, column 9
```

## Boundary rules

- The SAX face is the lxp parser object passed through verbatim (`:parse(str)` feeds segments, a no-arg `:parse()` ends the stream, `:close()` frees) — when you need low-level control (such as CDATA/comment callbacks), use its full callback set directly.
- The DOM does not preserve comments or processing instructions; entities in text (`&amp;` etc.) decode to characters, and encode escapes as needed.

