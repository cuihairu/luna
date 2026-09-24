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

## loop.fs:异步文件 IO

`loop.fs` 把 libuv 线程池上的文件操作接进同一套回调约定——Node 的 `fs` 三件套,错误沿用 Node 的"回调首参"风格:

```lua
local fs = require("loop").fs

fs.readFile("/etc/hostname", function(err, data)
    print(err and err or data:gsub("\n", ""))
end)

fs.writeFile("/tmp/note.txt", "hello\n", function(err)
    assert(err == nil, err)
    fs.stat("/tmp/note.txt", function(err, st)
        print(st.size)          -- 6
    end)
end)

loop.run()
```

| 调用 | 成功回调 | 失败回调 |
| --- | --- | --- |
| `fs.readFile(path, cb)` | `cb(nil, data)`(整个文件为一个字符串) | `cb(err)` |
| `fs.writeFile(path, data, cb)` | `cb(nil)` | `cb(err)` |
| `fs.stat(path, cb)` | `cb(nil, st)`,`st` 含 `size` / `mtime` / `mode` | `cb(err)` |

- **IO 在线程池,回调在循环线程**:大文件读写不阻塞定时器;回调照常经隔离执行,抛错不影响循环;
- **错误是字符串**:libuv 的 `uv_strerror` 直出(如 `no such file or directory`),与 Node 的 Error 对象相比是刻意简化——Lua 里 `err ~= nil` 判定即可;
- **嵌套安全**:一个操作的回调里可以再发起下一个操作——keep-alive 计数容许在两个操作之间短暂归零,回调链从 `run()` 内一路接续到全部完成。
