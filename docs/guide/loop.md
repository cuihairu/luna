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

`loop.fs` 把 libuv 线程池上的文件操作接进同一套回调约定——Node 的 `fs` 风格,读、写、统计加目录管理,错误沿用 Node 的"回调首参"风格:

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
| `fs.writeFile(path, data, cb)` | `cb(nil)`(覆盖写,不存在则创建) | `cb(err)` |
| `fs.appendFile(path, data, cb)` | `cb(nil)`(追加写,不存在则创建) | `cb(err)` |
| `fs.stat(path, cb)` | `cb(nil, st)`,`st` 含 `size` / `mtime` / `mode` | `cb(err)` |
| `fs.readdir(path, cb)` | `cb(nil, names)`——文件名数组,不含 `.`/`..`,顺序不作保证 | `cb(err)` |
| `fs.mkdir(path, cb)` | `cb(nil)`(mode 0777,umask 照常生效;**非递归**,父目录须已存在) | `cb(err)` |
| `fs.rmdir(path, cb)` | `cb(nil)`(只删空目录) | `cb(err)` |
| `fs.unlink(path, cb)` | `cb(nil)` | `cb(err)` |
| `fs.rename(old, new, cb)` | `cb(nil)`(原子改名/移动;new 已存在则覆盖) | `cb(err)` |

- **IO 在线程池,回调在循环线程**:大文件读写不阻塞定时器;回调照常经隔离执行,抛错不影响循环;
- **错误是字符串**:libuv 的 `uv_strerror` 直出(如 `no such file or directory`),与 Node 的 Error 对象相比是刻意简化——Lua 里 `err ~= nil` 判定即可,stat 读不存在的路径同样走 `cb(err)`;
- **嵌套安全**:一个操作的回调里可以再发起下一个操作——keep-alive 计数容许在两个操作之间短暂归零,回调链从 `run()` 内一路接续到全部完成;
- **readdir 底层是 `uv_fs_scandir`**:一次线程池调用列出全部条目,名字在交付前拷入 Lua,列表由 libuv 自行回收。

## loop.net:异步套接字

`loop.net` 是循环的第三块:流式套接字,客户端与服务端一对入口 × 两种传输(TCP / unix domain),同一套"回调首参"约定。

客户端:connect 拿到 `sock`,写、读、半关、关:

```lua
local net = require("loop").net

net.connect("localhost", 8080, function(err, sock)
    assert(err == nil, err)
    sock:write("ping\n", function(e)
        assert(e == nil, e)
        sock:read(function(e2, chunk)
            if chunk then print(chunk) end
            sock:close()          -- 打开的 socket 撑着循环,完事必须关
        end)
    end)
end)

loop.run()
```

服务端:listen 的绑定/监听错误**同步抛出**(这两步在 libuv 本就同步返回),每个进来的连接交给**常驻**的连接回调——`setInterval` 同款"留住回调"语义:

```lua
local net = require("loop").net

local srv                      -- 先声明后赋值:见下方作用域提醒
srv = net.listen("127.0.0.1", 0, function(err, sock)  -- 0 = 临时端口
    if err then print(err) return end
    sock:read(function(e, chunk)
        if chunk then sock:write(chunk) else sock:close() end
    end)                       -- 一个 echo 服务端
end)
print("listening on", srv:port())

loop.run()
```

| 调用 | 语义 |
| --- | --- |
| `net.connect(host, port, cb)` | 异步 DNS 解析 + TCP 连接;`cb(err, sock)` |
| `net.connectPipe(path, cb)` | unix domain 流套接字;`cb(err, sock)` |
| `net.listen(host, port, onConn)` | 监听 TCP;host 须为**数字地址**(`"0.0.0.0"` = 全接口),`port 0` = 临时端口;返回 server |
| `net.listenPipe(path, onConn)` | 监听 unix domain;路径须不存在(先 unlink),关闭**不会**删除路径 |
| `server:port()` | 实际绑定的端口(临时端口读回用) |
| `server:close(cb?)` | 幂等关闭;回调在句柄真正关闭后落地 |
| `sock:write(data, cb(err)?)` | 写出载荷,送达后回调(载荷由实现持有到回调落地) |
| `sock:read(cb)` | 流式读:每块 `cb(nil, chunk)`;对端 EOF 是 `cb(nil, nil)`;出错 `cb(err)`;再次调用即换回调 |
| `sock:shutdown(cb(err)?)` | 半关闭(FIN):对端读到 EOF,本端仍可继续读;不叫 `end` 是因为它是 Lua 关键字(`sock:end()` 无法解析) |
| `sock:close()` | 幂等关闭;打开的 socket 或 server 让 `run()` 持续——和 Node 一样,完事必须关 |

行为约定:

- **连接失败走回调**:DNS 失败或拒连从 `cb(err)` 出来,失败的 socket 自行收尾,循环照常排空到自然返回;
- **EOF 是 `(nil, nil)`**:对端关闭让下一次 `read` 回调拿到 `err=nil, chunk=nil`——用 `chunk == nil` 判结束,语义对齐 Node 流的 end;
- **半关闭**:`shutdown()` 只关写侧,读侧继续——请求-应答协议用它说"我发完了";
- **连接回调常驻**:`onConn(err, sock)` 对每个连接交付一次,引用由实现持有;accept 出错(如 fd 耗尽)也从它的 `err` 出来,不掀翻循环;
- **错误是字符串**:与 `loop.fs` 相同,`uv_strerror` 直出(`connection refused`、`address already in use` 等);
- **Lua 作用域提醒**:`local srv = net.listen(..., function() ... srv ... end)` 里回调摸到的 `srv` 是**全局** nil——局部变量要等声明语句结束才进入作用域,而回调写在此语句内部;回调用到的句柄请拆成 `local srv` + `srv = net.listen(...)` 两行。

## loop.process:异步子进程

`loop.process` 是循环的第四块,两个入口:`run` 是 `child_process.exec` 的聚合兄弟——不开 shell,拉起子进程,把它的 stdout/stderr 收进内存,等**退出且双管道排空**后一次交付;`spawn` 则把三路 stdio 直接交成普通的 `loop.net` sock,流式收发:

```lua
local loop = require("loop")
local process = loop.process

process.run("sh", {"-c", "echo hi; exit 0"}, function(err, res)
    assert(err == nil, err)
    print(res.status, res.stdout)   -- 0	hi
end)

loop.run()
```

`run` **立刻返回** proc 句柄(不必等子进程结束):`p:pid()` 随时可读;`p:kill(sig?)` 默认 SIGTERM——被杀的子进程照常走退出回调,`res.signal` 是信号编号,此时 `status` 无意义(libuv 约定)。

| 调用 | 语义 |
| --- | --- |
| `process.run(cmd, args?, opts?, cb)` | 无 shell 执行 cmd;`args` 是 argv 尾部字符串表;`opts` 首片只收 `{cwd = 路径}`;stdin 被忽略;立刻返回 proc 句柄 |
| `proc:pid()` | 子进程 pid |
| `proc:kill(sig?)` | 发信号(默认 15/SIGTERM);退出回调照常落地,幂等性没有——进程已退出后再杀会抛错 |
| `res.status` | 退出码(`signal` 非空时无意义) |
| `res.signal` | 终止信号编号;正常退出为 nil |
| `res.stdout` / `res.stderr` | 捕获到的两路输出(字符串,可为空串) |

行为约定:

- **spawn 失败同步抛错**:命令不存在等 exec 失败由 libuv 在 `uv_spawn` 里同步带回,`run` 直接 raise——与 `listen` 的绑定错误同款;`pcall` 接住后循环照常排空,不留悬空句柄;
- **交付条件是"退出 + 双管道 EOF"**:子进程死了内核必然关掉它的 fd,所以聚合回调确定性地到达,输出不会因缓冲未排空而截断;
- **没有 shell**:`cmd` 不经 `/bin/sh`,管道、通配、`~` 一概不展开——要 shell 语义就 `run("sh", {"-c", "..."})`,和 Node `spawn`/`exec` 的分野一致;
- **stdin 被忽略**:`run` 不接子进程的 stdin——要写就用 `process.spawn`(下一节),三路 stdio 都是普通 sock;
- **错误是字符串**:与 `loop.fs`/`loop.net` 相同,`uv_strerror` 直出。

## process.spawn:流式子进程

`spawn` 是聚合的流式反面:三路 stdio 就是三个普通的 `loop.net` sock——`write`/`read`/`shutdown`/`close` 全套语义原样适用,读回调同样 `cb(nil, chunk)` 流式多块、`cb(nil, nil)` 是 EOF:

```lua
local process = require("loop").process

local p                       -- 拆开声明:回调里读 p(见 net 的作用域提醒)
p = process.spawn("cat", {}, function(err, res)
    print("exit", res.status)                 -- 退出即交付,不等流
end)
p:stdout():read(function(e, chunk)
    if chunk then io.write(chunk) else p:stdout():close() end
end)
p:stdin():write("ping", function(e2)
    p:stdin():shutdown(function() p:stdin():close() end)  -- 半关说"发完了"
end)
p:stderr():close()            -- 不读的流也要关

loop.run()
```

| 调用 | 语义 |
| --- | --- |
| `process.spawn(cmd, args?, opts?, cb)` | 同 `run` 的参数;立刻返回 proc 句柄;退出回调 `cb(nil, res)` 只带 `status`/`signal`,不含捕获输出 |
| `proc:stdin()` / `proc:stdout()` / `proc:stderr()` | 取三路 stdio 对应的 sock;**退出交付之后返回 nil** |

行为约定:

- **onExit 对齐 Node 的 `'exit'`**:子进程退出即交付,**不等** stdout/stderr 排空——没人读的管道永远不会 EOF,等它就死了;退出后流照常继续可读;
- **流是普通的 net sock**:EOF 之外没有隐式收尾——读完了要 `close`,不读的流也要 `close`(打开的 sock 撑着循环,和 `loop.net` 的"完事必须关"同一条);
- **句柄只能取到交付前**:`p:stdout()` 在退出回调里已经是 nil——要在回调里用 sock,先把局部变量抓在 spawn 之后:`local out = p:stdout()`;
- **写侧说完用 `shutdown`**:cat 这类读到 EOF 才退出的程序,靠半关闭(不是 `close`,那会连读侧一起扔)说"我发完了";
- **stdin/stdout/stderr 独立**:三路互不相干,stderr 的 EOF 不影响 stdout——分路读正是 spawn 相对 run 的意义。
