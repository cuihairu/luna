# stdlib · zlib

Compression. Backed by [lua-zlib](https://github.com/brimworks/lua-zlib) (binds system zlib; streaming inflate/deflate, gzip support). The C core is registered as `zlib.core`, and its streaming closures are reachable verbatim through `__index`; luna adds a one-shot convenience layer on top.

## API

| Call | Description |
| --- | --- |
| `zlib.compress(data, level?)` | One-shot deflate; `level` 0-9 (zlib convention), default 6 |
| `zlib.decompress(data)` | One-shot inflate |
| `zlib.deflate([level], [windowSize])` | lua-zlib verbatim: returns a streaming compression closure. `windowSize` defaults to `15` (zlib format); pass `15+16` for gzip, `-15` for raw deflate |
| `zlib.inflate([windowSize])` | lua-zlib verbatim: returns a streaming decompression closure. No `windowSize` means auto-detect |
| (everything else) | lua-zlib's C core functions pass through `__index`: `deflate`/`inflate`/`adler32`/`crc32`/`version` |

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
- The one-shot convenience produces a **zlib-format stream** (2-byte header plus adler32 trailer — not gzip). The window bits are fixed at closure *creation*, not at call time: `zlib.deflate(level, windowSize)` with the closure taking only `(data, flush)`. For gzip, `zlib.deflate(level, 15+16)` — pass `31`, don't append a third argument to the closure call (`-15` for raw deflate).

