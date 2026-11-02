# stdlib · stream

A v1 narrow face of Node streams, pure Lua, built on [events](/zh/stdlib/events). Semantics pinned to Node v24; the verification record is in [Node alignment](/zh/node-parity). The socket adapter (`duplexFromSock`) has a full echo-server example in [loop](/zh/guide/loop).

## API

| Call | Description |
| --- | --- |
| `stream.readable(opts)` | Read side; `opts._read(self, size)` is the pull source, and `self:push(chunk)` feeds data (`nil` is EOF) |
| `stream.writable(opts)` | Write side; `opts._write(self, chunk, cb)` is the sink, `cb(err)` reports completion |
| `stream.duplex(opts)` | Both halves coexist |
| `stream.transform(opts)` | A middle transformation; `opts._transform(self, chunk, cb)` can `push` multiple times, with an optional `_flush` |
| `stream.pushReadable(opts?)` | A push source: the producer calls `:push(chunk)` directly, and a `false` return means back off (the default `_read` has no pull face) |
| `stream.readableFromChunks(chunks)` | Turns a collection of chunks (strings or a chunk table) into a readable stream |
| `stream.duplexFromSock(sock)` | `loop.net` socket → Duplex adapter |
| `r:pipe(w)` | Pipes; the write side pauses when buffered to highWaterMark and resumes on the `drain` event (backpressure); `opts.end = false` does not auto-close the write side |
| `r:unpipe(w)` | Removes the pipe |

Events: `data`/`end` (read side), `finish` (write side drained), `drain` (buffer receded), `error` → `destroy` → `close`. `end` is a Lua keyword, spelled `s["end"]` or `s:end_()`.

## Usage

Transform stream: input lines to uppercase, pipe-driven:

```lua
local stream = require "stream"
local up = stream.transform({
  _transform = function(self, chunk, cb)
    self:push(chunk:upper())
    cb()
  end,
})
local out = {}
up:on("data", function(c) out[#out + 1] = c end)
up:on("end", function() print(table.concat(out)) end)
up:write("hello ")
up:write("streams")
up["end"](up)
```

```text
HELLO STREAMS
```

Backpressure: the write side pauses when `write` returns `false`, and resumes on `drain`:

```lua
local stream = require "stream"
local seen, cbs = {}, {}
local slow = stream.writable({
  highWaterMark = 3,
  _write = function(self, chunk, cb)  -- slow sink: accept but do not settle yet (single-flight contract)
    seen[#seen + 1] = chunk
    cbs[#cbs + 1] = cb
  end,
})
local pushed = {}
for i = 1, 5 do pushed[#pushed + 1] = tostring(slow:write(i)) end
print(table.concat(pushed, ","))
while #cbs > 0 do table.remove(cbs)() end  -- settle one by one; the single-flight _write follows
slow["end"](slow)
print(table.concat(seen, ","))
```

```text
true,true,true,false,false
1,2,3,4,5
```

(`write`'s return is the buffer account: the in-flight chunk does not count, and queuing up to highWaterMark gives `false`; the sink is **single-flight** — only one `_write` runs at a time, and settling (`cb()`) advances to the next. A synchronous fast sink is always `true` and never backs up.)

Pull source + pipe wrap-up:

```lua
local stream = require "stream"
local n = 0
local src = stream.readable({
  _read = function(self)
    n = n + 1
    if n <= 3 then self:push("item" .. n .. ";") else self:push(nil) end
  end,
})
local acc = {}
src:on("end", function() print(table.concat(acc)) end)
src:on("data", function(c) acc[#acc + 1] = c end)
```

```text
item1;item2;item3;
```

(Flowing starts **synchronously** the moment a `'data'` listener is attached — attach to start. Listeners like `'end'` must be attached first, otherwise they are registered after the event already fired and will never fire.)

## Boundary rules

- Accounting: strings count in bytes, other values count as 1 (an always-on objectMode simplification); `highWaterMark` defaults to 16384 and is a threshold, not a byte-exact count;
- The error linkage difference from a bare Node pipe is **verified**: a source `error` destroys the destination and unpipes, while a destination `error` only unpipes — see the stream section of [Node alignment](/zh/node-parity);
- `end`/`finish` auto-destroy afterwards (autoDestroy semantics), and `destroy(err)` fires `error` then `close`, idempotently.

