# stdlib · zlib

Compression. Backed by [lua-zlib](https://github.com/brimworks/lua-zlib) (binds system zlib; streaming inflate/deflate, gzip support). The C core is registered as `zlib.core`, and its streaming closures are reachable verbatim through `__index`; luna adds a one-shot convenience layer on top.

## API

| Call | Description |
| --- | --- |
| `zlib.compress(data, level?)` | One-shot deflate; `level` 0-9 (zlib convention), default 6 |
| `zlib.decompress(data)` | One-shot inflate |
| `zlib.deflate([level])` | lua-zlib verbatim: returns a streaming compression closure |
| `zlib.inflate()` | lua-zlib verbatim: returns a streaming decompression closure |
| (everything else) | lua-zlib's C core functions pass through `__index`: `deflate`/`inflate`/`adler32`/`crc32`/`version` and the window-bit constants |

## Usage

```lua
local zlib = require "zlib"
local blob = ("luna"):rep(1000)
local packed = zlib.compress(blob)
print(#blob, #packed, #packed < #blob / 10)
```

```text
4000	32	true
```

Decompress restores:

```lua
local zlib = require "zlib"
print(zlib.decompress(zlib.compress("hello luna")))
```

```text
hello luna
```

The streaming face (data split into chunks, incremental output) goes through lua-zlib's verbatim closures:

```lua
local zlib = require "zlib"
local deflate = zlib.deflate()
local part1 = deflate("chunk-one ", "sync")
local part2 = deflate("chunk-two", "finish")
print(#zlib.inflate()(part1 .. part2, "finish"))
```

```text
19
```

## Boundary rules

- `compress`/`decompress` are one-shot in/out — for large files use the streaming closures in chunks, don't read the whole thing into memory.
- The one-shot convenience produces a **raw deflate stream** (no gzip header); for gzip format use the streaming closure with window bits — `zlib.deflate(level)(data, "finish", 15+16)` (raw deflate is -15, gzip is 15+16).

