# 异步任务

`require "task"` 在 loop 之上铺一层 coroutine 任务模型:`task.run` 起协程,`task.await` 挂起等 promise,timer 复用 `loop.setTimeout`——不新增 loop API,本批对 C 的唯一改动是修一处既有崩溃(loop 回调原本落在创建句柄的线程上,协程里 setTimeout 会 pcall 到挂起线程,见[契约与边界](#契约与边界))。脚本尾自动排水([事件循环](/guide/loop)的 maybeDrain 契约)让睡着的任务活着;REPL 不排水,sleep 流属于 script / `-e` 用法。

## API

| 调用 | 说明 |
| --- | --- |
| `task.run(fn, ...)` | 立即起跑协程任务(fn 执行到首个 await 或完成才返回),返回句柄 |
| `h:cancel()` | 协作取消:只对挂起在 await 上的任务生效,返回是否送达 |
| `task.await(p)` | 挂起当前任务等 promise;resolve 的值原样返回,拒绝则 raise 原因 |
| `task.sleep(ms)` | 一发 timer + await;取消时 clearTimeout,定时器不空转 |
| `task.promise()` | 一次性 promise:`:resolve(...)` / `:reject(err)`,二次 settle 抛错 |
| `task.promisify(fn)` | err-first 回调风格函数 → 返回 promise 的包装;同步抛错转拒绝 |
| `task.list()` | 活任务句柄数组,id 升序 |
| `task.stats()` | 累计 `started`/`done`/`error`/`cancelled`,当前 `live`/`awaiting`/`running` |

句柄是普通数据表:`id`、`status`(`running`/`done`/`error`/`cancelled`)、`src`(来源 `文件:行`)、`awaiting`(挂起在等什么:`sleep` 或 `promise`)、`results`(done 后的返回值表)、`err`(失败原因,错误同时上 stderr 一次)。

## 任务与句柄

```lua
local task = require "task"
local loop = require "loop"
local h = task.run(function(name)
  task.sleep(30)
  return "hello " .. name
end, "luna")
print(h.id, h.status, h.awaiting)   -- 起跑即返回:挂起在 sleep 上
loop.run()                          -- 跑到活句柄清空再回来
print(h.status, h.results[1])
```

```text
1	running	sleep
done	hello luna
```

不显式 `loop.run()` 也行——脚本尾自动排水。但排水发生在 main chunk 之后:想在 main chunk 里接着读 `results`,就得像上面这样显式跑一把,否则第 10 行读到的还是 `nil`。

多个任务的唤醒按 timer deadline 排,不按起跑序:

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

## promise 与 promisify

`task.promise()` 给出一次性结算点,`task.await` 等它;`task.promisify` 把 err-first 回调风格的函数包成返回 promise 的版本:

```lua
local task = require "task"
local f = assert(io.open("/tmp/task-demo.txt", "w"))
f:write("luna\n")
f:close()

local function readit(path, cb)            -- err-first 回调风格
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

拒绝原因从 `await` 以错误形式抛出(`pcall` 承接);resolve 的多个值原样回到 `await` 的返回。回调同步抛错同样转成拒绝——首个结算赢,结算之后回调再抛只会上一行 stderr,不被吞掉。

## 取消

`h:cancel()` 只对挂起在 await 上的任务生效:CANCELLED 错误落在 await 点,睡眠的 timer 先被 clearTimeout:

```lua
local task = require "task"
local t = task.run(function() task.sleep(10000) print("never") end)
print(t:cancel(), t.status)
```

```text
true	cancelled
```

取消是协作的:任务体可以 pcall 住 CANCELLED 做清理后继续跑,`status` 照实反映结局(吞掉取消还活着就是 `running`)。任务体内的自取消返回 `false`——正在跑的代码没法取消自己。

## 错误口径

任务体抛错不掀循环:stderr 打一行(一次),`h.err` 可复读,退出码不变:

```lua
local task = require "task"
local h = task.run(function() error("kaput") end)
print("status:", h.status, "err?", h.err ~= nil)
```

```text
task #1 failed (err.lua:2): err.lua:2: kaput
status:	error	err?	true
```

(stderr 行来自任务退出,stdout 行来自 print;任务退出后无活句柄,排水立即返回。)

## %tasks

REPL 里看活任务与累计计数(被动读 `package.loaded.task`,不自动加载,`require "task"` 之前是占位提示):

```text
In [1]: task = require "task"
In [2]: H = task.run(function() task.sleep(3000) end)
In [3]: %tasks
  #1    awaiting sleep   [string "repl[2]"]:1
1 live (1 awaiting), 0 done, 0 failed, 0 cancelled of 1 started
```

已完成任务随即出册(错误已上过 stderr,句柄留在调用者手里),所以列表只列活的;累计数看末行。REPL 不排水,这个 sleep 到进程退出也不会触发——要看任务真的跑完,用 script / `-e`。

## 契约与边界

- await 通道只有一条:`task.await`/`task.sleep` 在任务体外调用抛错;任务体内裸 `coroutine.yield` 记为任务错误——不存在第二通道,收到的值不用猜。
- cancel 语义见上节:挂起才可取消,CANCELLED 落在 await 点,可被任务体捕获;取消中的 sleep 先 clearTimeout。
- promise 一次性:二次 settle 抛 `task.promise: settled twice`;多个等待者全部唤醒。
- 任务错误一次上 stderr、存进 `h.err`,不改退出码——要 fail-fast 得自己在 await 点接。
- 全部句柄走 `loop.setTimeout`/`loop.clearTimeout`,`require "task"` 即拉入 loop(与 `require "loop"` 同门的 opt-in);同步标准库不受影响。
- 本批对 C 的唯一改动是 `luna_loop.c` 的回调落点:此前回调 pcall 在创建句柄的线程上,在协程里 setTimeout 会把回调 pcall 到挂起(或已死、或已被 GC)的线程栈上,回调里再 resume 它就是段错误;现在落在驱动循环的线程(g_L,与 attach 轮询同款)。在 main 线程建句柄的既有用法行为不变。
