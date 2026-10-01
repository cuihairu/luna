# stdlib · stream

Node 流的 v1 收窄面,纯 Lua,地基是 [events](/stdlib/events)。语义按 Node v24 钉版,勘定记录见[Node 方向选型](/node-parity)。与 socket 的对接(`duplexFromSock`)在 [loop](/guide/loop) 有完整的回显服务示例。

## API

| 调用 | 说明 |
| --- | --- |
| `stream.readable(opts)` | 读侧;`opts._read(self, size)` 拉式源,`self:push(chunk)` 灌数据(`nil` 即 EOF) |
| `stream.writable(opts)` | 写侧;`opts._write(self, chunk, cb)` 是汇,`cb(err)` 报完成 |
| `stream.duplex(opts)` | 两半共存 |
| `stream.transform(opts)` | 中间变换;`opts._transform(self, chunk, cb)`,可多次 `push`,可选 `_flush` |
| `stream.pushReadable(fn)` | 推式源的糖 |
| `stream.readableFromChunks(iter)` | 把分片集合转可读流 |
| `stream.duplexFromSock(sock)` | `loop.net` socket → Duplex 适配 |
| `r:pipe(w)` | 管道;写侧缓冲到 highWaterMark 即暂停,`drain` 事件续推(背压) |

事件:`data`/`end`(读侧)、`finish`(写侧排空)、`drain`(缓冲回退)、`error` → `destroy` → `close`。`end` 是 Lua 关键字,拼作 `s["end"]` 或 `s:end_()`。

## 用法

变换流:输入行转大写,管道驱动:

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

背压:写侧返回 `false` 时暂停,`drain` 再续:

```lua
local stream = require "stream"
local seen, cbs = {}, {}
local slow = stream.writable({
  highWaterMark = 3,
  _write = function(self, chunk, cb)  -- 慢汇:收下但不立即回账(单飞契约)
    seen[#seen + 1] = chunk
    cbs[#cbs + 1] = cb
  end,
})
local pushed = {}
for i = 1, 5 do pushed[#pushed + 1] = tostring(slow:write(i)) end
print(table.concat(pushed, ","))
while #cbs > 0 do table.remove(cbs)() end  -- 逐笔回账,单飞的 _write 跟着续跑
slow["end"](slow)
print(table.concat(seen, ","))
```

```text
true,true,true,false,false
1,2,3,4,5
```

(`write` 的返回值是缓冲账:在途的那笔不占账,排队到 highWaterMark 即 `false`;汇是**单飞**的——同时只有一笔 `_write` 在跑,回账(`cb()`)才续下一笔。同步 `cb` 的快汇则永远 `true`,不积压。)

拉式源 + 管道收尾:

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

(流动在 `'data'` 监听挂上的那一刻**同步开跑**——挂后发车。`'end'` 等后续监听要先挂,否则事件发完了才注册,就永远等不到。)

## 契约与边界

- 记账:字符串按字节、其余值按 1 计(objectMode 恒开的简化);`highWaterMark` 缺省 16384,只做阈值不做字节精确;
- 错误联动与 Node 裸 pipe 的差异是**勘定过的**:源 `error` 会 destroy 目标并 unpipe,目标 `error` 只 unpipe——详见 [Node 方向选型](/node-parity)的 stream 节;
- `end`/`finish` 后自动 destroy(autoDestroy 口径),`destroy(err)` 先 `error` 后 `close`,幂等。
