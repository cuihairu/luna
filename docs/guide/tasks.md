# Async tasks

`require "task"` layers a coroutine task model over `loop`: `task.run` starts a coroutine, `task.await` suspends waiting on a promise, and timers reuse `loop.setTimeout` — no new loop API. The only C change in this batch fixes an existing crash (the loop callback previously landed on the thread that created the handle, so `setTimeout` inside a coroutine would pcall onto a suspended thread; see [Boundary rules](#boundary-rules)). Script-tail auto-drain (the `maybeDrain` contract in [Event loop](/zh/guide/loop)) keeps sleeping tasks alive; the REPL does not drain, so `sleep` streams belong to script / `-e` usage.

## API

| Call | Description |
| --- | --- |
| `task.run(fn, ...)` | Immediately starts a coroutine task (fn runs until its first await or completion, then returns); returns a handle |
| `h:cancel()` | Cooperative cancel: only affects tasks suspended on an await; returns whether it was delivered |
| `task.await(p)` | Suspends the current task waiting on a promise; resolve values pass through unchanged, a rejection raises the reason |
| `task.sleep(ms)` | One-shot timer + await; on cancel it clears the timeout so the timer does not spin |
| `task.promise()` | One-shot promise: `:resolve(...)` / `:reject(err)`; a second settle raises |
| `task.promisify(fn)` | Wraps an err-first callback-style function into one returning a promise; synchronous throws become rejections |
| `task.list()` | Array of live task handles, id ascending |
| `task.stats()` | Cumulative `started`/`done`/`error`/`cancelled`, plus current `live`/`awaiting`/`running` |

A handle is a plain data table: `id`, `status` (`running`/`done`/`error`/`cancelled`), `src` (origin `file:line`), `awaiting` (what it is suspended on: `sleep` or `promise`), `results` (return-value table after `done`), `err` (failure reason; the error also goes to stderr once).

## Tasks and handles

```lua
local task = require "task"
local loop = require "loop"
local h = task.run(function(name)
  task.sleep(30)
  return "hello " .. name
end, "luna")
print(h.id, h.status, h.awaiting)   -- returns immediately: suspended on sleep
loop.run()                          -- runs until live handles clear, then returns
print(h.status, h.results[1])
```

```text
1	running	sleep
done	hello luna
```

You do not need an explicit `loop.run()` — the script tail auto-drains. But draining happens after the main chunk: to read `results` inside the main chunk, run it explicitly as above, otherwise line 10 still reads `nil`.

Multiple tasks wake by timer deadline, not by start order:

```lua
local task = require "task"
local order = {}
task.run(function() task.sleep(60) order[#order + 1] = "b" end)
task.run(function() task.sleep(20) order[#order + 1] = "a" end)
task.run(function()
  task.sleep(100)
  print(table.concat(order, ","))
end)
```

```text
a,b
```

## promise and promisify

`task.promise()` gives a one-shot settlement point, and `task.await` waits on it; `task.promisify` wraps an err-first callback-style function into one returning a promise:

```lua
local task = require "task"
local f = assert(io.open("/tmp/task-demo.txt", "w"))
f:write("luna\n")
f:close()

local function readit(path, cb)            -- err-first callback style
  local fh = io.open(path)
  if not fh then cb(path .. ": nope") return end
  local d = fh:read("a")
  fh:close()
  cb(nil, d)
end
local pread = task.promisify(readit)

task.run(function()
  print(task.await(pread("/tmp/task-demo.txt")))
  local ok, err = pcall(task.await, pread("/tmp/nope-404"))
  print(ok, err)
end)
```

```text
luna

false	/tmp/nope-404: nope
```

A rejection reason is thrown from `await` as an error (caught by `pcall`); multiple resolve values return unchanged from `await`. A callback that throws synchronously becomes a rejection too — the first settlement wins, and a throw after settlement only goes to stderr, never swallowed.

## Cancellation

`h:cancel()` only affects tasks suspended on an await: a CANCELLED error lands at the await point, and a sleeping task's timer is cleared first:

```lua
local task = require "task"
local t = task.run(function() task.sleep(10000) print("never") end)
print(t:cancel(), t.status)
```

```text
true	cancelled
```

Cancellation is cooperative: the task body can `pcall` the CANCELLED error, do cleanup, and keep running; `status` reflects the real outcome (swallowing the cancel and staying alive means `running`). A task cancelling itself returns `false` — running code cannot cancel itself.

## Error reporting

A task body error does not topple the loop: stderr gets one line (once), `h.err` is re-readable, and the exit code is unchanged:

```lua
local task = require "task"
local h = task.run(function() error("kaput") end)
print("status:", h.status, "err?", h.err ~= nil)
```

```text
task #1 failed (err.lua:2): err.lua:2: kaput
status:	error	err?	true
```

(The stderr line comes from task exit, the stdout line from `print`; after task exit there are no live handles, so drain returns immediately.)

## %tasks

In the REPL, view live tasks and cumulative counters (passively reads `package.loaded.task`, does not auto-load; before `require "task"` it shows a placeholder):

```text
In [1]: task = require "task"
In [2]: H = task.run(function() task.sleep(3000) end)
In [3]: %tasks
  #1    awaiting sleep   [string "repl[2]"]:1
1 live (1 awaiting), 0 done, 0 failed, 0 cancelled of 1 started
```

Completed tasks leave the registry immediately (the error already went to stderr; the handle stays with the caller), so the list shows only live ones; the cumulative counts are on the last line. The REPL does not drain, so this sleep never fires before process exit — to actually see a task finish, use script / `-e`.

## Boundary rules

- There is only one await channel: calling `task.await`/`task.sleep` outside a task body raises; a bare `coroutine.yield` inside a task body is recorded as a task error — no second channel exists, so received values never need guessing.
- Cancel semantics as above: only suspended tasks can be cancelled, CANCELLED lands at the await point and can be caught by the task body; a cancelled sleep is cleared first.
- A promise is one-shot: a second settle raises `task.promise: settled twice`; all waiters wake.
- A task error goes to stderr once and into `h.err`, without changing the exit code — to fail fast you must catch it yourself at the await point.
- All handles go through `loop.setTimeout`/`loop.clearTimeout`; `require "task"` pulls in `loop` (same opt-in as `require "loop"`); the synchronous standard library is unaffected.
- The only C change in this batch is the callback landing point in `luna_loop.c`: previously the callback was pcall'd on the thread that created the handle, so `setTimeout` inside a coroutine would pcall the callback onto a suspended (or dead, or already GC'd) thread stack, and resuming it there was a segfault; now it lands on the thread driving the loop (`g_L`, the same kind as attach polling). Existing usage that creates handles on the main thread is unchanged.
