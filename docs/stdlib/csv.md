# stdlib · csv

RFC 4180-style CSV encode/decode. LPeg-grammar-driven (luna bundles lpeg, zero new dependencies).

## API

| Call | Description |
| --- | --- |
| `csv.decode(str, opts?)` | Parses into **an array of rows, each a field array**; fields are always strings (CSV has no types; conversion is the caller's business, same as Node csv-parse's default); `opts.delimiter` (default `,`), `opts.headers = true` changes the return to **an array of record tables** (the first row is keys; extra columns land under numeric keys) |
| `csv.encode(rows, opts?)` | Encodes an array of rows; `opts.eol` defaults to CRLF (RFC 4180 wire format), **does not append a trailing eol**; `opts.delimiter` changes the separator; `opts.headers = true` writes a header row first (encode an array of record tables) |
| `csv.lines(str)` | A per-line iterator yielding each row's field array; bad data **raises** (an iterator's nil is the end, with nowhere to put an error); `opts` same as `decode` |

## Usage

```lua
local csv = require "csv"
local rows = csv.decode('name,age\n"Zhang, San",33\nLi,25\n')
for _, r in ipairs(rows) do print(r[1], r[2]) end
```

```text
name	age
Zhang, San	33
Li	25
```

Quote escaping is symmetric on the encode side; row separation is CRLF per RFC 4180 (changeable with `opts.eol`), made visible in the example:

```lua
local csv = require "csv"
print((csv.encode({ { "plain", 42 }, { "has, comma", 'has "quote"' } })
       :gsub("\r\n", "\\r\\n")))
```

```text
plain,42\r\n"has, comma","has ""quote"""
```

Per-line iteration, large-file friendly:

```lua
local csv = require "csv"
for r in csv.lines("a,b\n1,2\n3,4\n") do print(r[1] .. ":" .. r[2]) end
```

```text
a:b
1:2
3:4
```

Bad data returns `nil, err` with the message carrying line and column (column counted in UTF-8 characters):

```lua
local csv = require "csv"
local rows, err = csv.decode('a,"unterminated\nb,c\n')
print(rows, err)
```

```text
nil	csv: unterminated quoted field at line 3, column 1
```

## Boundary rules

- Accepted line endings: CRLF and LF; a lone CR is rejected. Fields are not trimmed — whitespace is data.
- `lines()` raises on bad data; feed untrusted input with `pcall`; `decode`/`encode` always return `nil, err`.

