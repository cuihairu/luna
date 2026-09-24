# 事件循环

luna 的事件循环是对 libuv 的直接暴露,Node 命名、Lua 语义。它是**显式选择**:同步标准库不受影响,只有 `require "loop"` 的代码进入异步世界——这是架构笔记里"脚本模式可选异步、REPL 逐步暴露"的第一步。

```lua
local loop = require("loop")

local ticks = 0
loop.setInterval(function() ticks = ticks + 1 end, 1000)
loop.setTimeout(function()
    print("a minute passed, ticks = " .. ticks)
end, 60000, "unused-arg")     -- 尾随参数原样传给回调

loop.run()                    -- 驱动循环,直到没有任何句柄
```

## API

| 调用 | 语义 |
| --- | --- |
| `loop.setTimeout(fn, ms?, ...)` | `ms` 毫秒后调用一次;返回句柄 |
| `loop.setInterval(fn, ms?, ...)` | 每 `ms` 毫秒重复;返回句柄 |
| `loop.setImmediate(fn, ...)` | 下一轮循环的 check 阶段执行 |
| `loop.clearTimeout(h)` / `loop.clearInterval(h)` / `loop.clearImmediate(h)` | 清除句柄;对已触发的 one-shot 是无害空操作 |
| `loop.run(mode?)` | `"default"`(默认,跑到空)、`"once"`(一轮,阻塞等就绪)、`"nowait"`(一轮,不阻塞) |
| `loop.stop()` | 让当前 `run` 尽快返回;句柄保持已调度状态 |
| `loop.now()` | 循环毫秒时钟(`uv_now`) |

行为约定:

- **回调隔离**:回调抛错只打到 stderr,循环继续——和 REPL 隔离一次求值错误的方式一致;
- **keep-alive**:`run("default")` 在最后一个句柄关闭后自然返回;未清除的 interval 会让它永不返回,和 Node 一样;
- **`^C` 中断**:循环阻塞在 `uv_run` 时,计数钩子看不见信号——prepare 钩子接管:置起的 `^C` 让 `run` 以 `interrupted` 抛错,脚本照常以退出码 130 结束;
- **attach 仍然可达**:同一 prepare 钩子顺带轮询 attach 套接字——事件循环跑着的进程照常被 `luna --attach` 检查、求值、改状态,`serve.step` 的全部约束不变(只在轮询点到达)。
