# stdlib · events

Node `EventEmitter` 语义。`events.new()` 构造,方法挂在实例上。语义按 Node v24 实证钉版,选型记录见[Node 方向选型](/node-parity)。事件循环([loop](/guide/loop))与流([stream](/stdlib/stream))都架在它上面。

## API

| 调用 | 说明 |
| --- | --- |
| `events.new()` | 新 emitter |
| `em:on(name, fn)` | 挂监听,返回 emitter(可链) |
| `em:once(name, fn)` | 触发一次后自动摘除 |
| `em:off(name, fn)` | 摘监听(同 `removeListener`) |
| `em:prependListener(name, fn)` | 头插监听 |
| `em:emit(name, ...)` | 分发;返回**是否有监听** |
| `em:listeners(name)` / `em:listenerCount(name)` | 查询 |
| `em:setMaxListeners(n)` / `events.defaultMaxListeners` | 超限警告阈值(默认 10) |
| `em:removeAllListeners(name?)` | 清监听 |

## 用法

```lua
local events = require "events"
local em = events.new()
em:on("greet", function(name, punct) print("hi", name .. punct) end)
print(em:emit("greet", "luna", "!"))
print(em:emit("nobody-listens"))
```

```text
hi	luna!
true
false
```

`once` 与摘除:

```lua
local events = require "events"
local em = events.new()
local n = 0
em:once("tick", function() n = n + 1 end)
em:emit("tick"); em:emit("tick")
print(n, em:listenerCount("tick"))
```

```text
1	0
```

`error` 事件无监听时 raise(Node 同款),表负载原样、标量包一层:

```lua
local events = require "events"
local em = events.new()
local ok, err = pcall(function() em:emit("error", "boom") end)
print(ok, err)
```

```text
false	Unhandled error. ('boom')
```

## 契约与边界

- `newListener` 在添加前发出(监听数不含新者),`removeListener` 在移除后发出;`removeAllListeners(ev)` 逐个(LIFO)发 `removeListener`,无参全清不发。
- 超限警告走 stderr,每个 (emitter, 事件名) 一次,移除/重设后重置——只警告,不抛。
- `emit` 期间的自移除/重挂都作用于活表,本轮照常调用(快照分发的 Node 语义)。
