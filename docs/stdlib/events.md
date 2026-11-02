# stdlib · events

Node `EventEmitter` semantics. Construct with `events.new()`; methods hang off the instance. Semantics are pinned to measured Node v24; the selection record is in [Node alignment](/zh/node-parity). The event loop ([loop](/zh/guide/loop)) and streams ([stream](/zh/stdlib/stream)) both sit on top of it.

## API

| Call | Description |
| --- | --- |
| `events.new()` | A new emitter (`events.EventEmitter` is an alias for the same class) |
| `em:on(name, fn)` | Attaches a listener, returns the emitter (chainable) |
| `em:once(name, fn)` | Auto-removes after one firing |
| `em:off(name, fn)` | Removes a listener (same as `removeListener`) |
| `em:prependListener(name, fn)` | Inserts a listener at the head |
| `em:emit(name, ...)` | Dispatches; returns **whether any listener existed** |
| `em:listeners(name)` / `em:listenerCount(name)` | Queries |
| `em:setMaxListeners(n)` / `events.defaultMaxListeners` | The over-limit warning threshold (default 10) |
| `em:removeAllListeners(name?)` | Clears listeners |
| Aliases | `addListener`=`on`, `removeListener`=`off`, `prependOnceListener` (head-insert one-shot) |

## Usage

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

`once` and removal:

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

An `error` event with no listeners raises (same as Node); table payloads pass through verbatim, scalars are wrapped in one layer:

```lua
local events = require "events"
local em = events.new()
local ok, err = pcall(function() em:emit("error", "boom") end)
print(ok, err)
```

```text
false	Unhandled error. ('boom')
```

## Boundary rules

- `newListener` fires before the add (the listener count does not include the new one); `removeListener` fires after the removal; `removeAllListeners(ev)` fires `removeListener` one by one (LIFO), and a no-arg clear fires none.
- The over-limit warning goes to stderr, once per (emitter, event name), reset on removal/re-set — it only warns, never throws.
- Self-removal and re-attach during `emit` act on the live table and this turn still calls them (Node's snapshot-dispatch semantics).

