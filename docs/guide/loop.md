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
| `handle:unref()` / `handle:ref()` | 摘掉/恢复句柄的 keep-alive(见下节) |

行为约定:

- **回调隔离**:回调抛错只打到 stderr,循环继续——和 REPL 隔离一次求值错误的方式一致;
- **keep-alive**:`run("default")` 在最后一个句柄关闭后自然返回;未清除的 interval 会让它永不返回,和 Node 一样;
- **`^C` 中断**:循环阻塞在 `uv_run` 时,计数钩子看不见信号——prepare 钩子接管:置起的 `^C` 让 `run` 以 `interrupted` 抛错,脚本照常以退出码 130 结束;
- **attach 仍然可达**:同一 prepare 钩子顺带轮询 attach 套接字——事件循环跑着的进程照常被 `luna --attach` 检查、求值、改状态,`serve.step` 的全部约束不变(只在轮询点到达)。

## handle:unref/ref:谁撑着循环

每个句柄默认**撑着**循环:只要有句柄在,`run("default")` 就不返回。`unref` 把一个句柄从这份账上摘掉——它照常运行(timer 照常到点、回调照常交付),只是**不再独自留住循环**;`ref` 把账加回来:

```lua
local loop = require("loop")

local iv
iv = loop.setInterval(function() log_flush() end, 30000)
iv:unref()                    -- 周期日志不阻止脚本自然结束

loop.run()                    -- 其余句柄清空后即返回,interval 随进程终止
```

| 调用 | 语义 |
| --- | --- |
| `handle:unref()` | 句柄继续运行,但不再撑着循环;幂等 |
| `handle:ref()` | 恢复撑持;幂等 |

适用面覆盖全部句柄:timer/interval/immediate 句柄、`fs.watch` 的 watcher、`loop.signal` 的 watcher、`loop.net` 的 sock 与 server、`loop.udp` 的 sock、`loop.process` 的 proc(以及 `process.spawn` 的三路 stdio sock)。

行为约定:

- **unref 不是暂停**:句柄的运行、回调交付、错误路径一概不变——唯一的变化是 keep-alive 计数不再算它;循环因其他句柄转着的时候,unref'd 的 timer 照常 fire;
- **这是 Node `unref` 的语义**:定时器、server、watcher 都可以"在,但不留人"——后台周期任务、不关心连接何时来的监听端、看一眼就走的观察者;
- **清账责任仍在**:unref 只影响循环何时退出,不替你清理——脚本结束前对不再需要的句柄照常 `close`/`clear*`(关着的句柄上调用是安全的空操作);
- **run 的返回不区分谁撑的**:全 unref 之后 `run("default")` 返回,和"没有句柄"无法区分——要判断"还有活没干完",自己在回调里记状态,别猜循环。

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
| `fs.stat(path, cb)` | `cb(nil, st)`,`st` 含 `size` / `mtime` / `mode` / `type` | `cb(err)` |
| `fs.readdir(path, cb)` | `cb(nil, names)`——文件名数组,不含 `.`/`..`,顺序不作保证 | `cb(err)` |
| `fs.mkdir(path, cb)` | `cb(nil)`(mode 0777,umask 照常生效;**非递归**,父目录须已存在) | `cb(err)` |
| `fs.rmdir(path, cb)` | `cb(nil)`(只删空目录) | `cb(err)` |
| `fs.unlink(path, cb)` | `cb(nil)` | `cb(err)` |
| `fs.rename(old, new, cb)` | `cb(nil)`(原子改名/移动;new 已存在则覆盖) | `cb(err)` |
| `fs.copyFile(src, dst, cb)` | `cb(nil)`(dst 已存在则静默覆盖) | `cb(err)` |
| `fs.access(path, cb)` | `cb(nil)`——路径存在且可 stat | `cb(err)` |
| `fs.realpath(path, cb)` | `cb(nil, resolved)`——解析 `..`/符号链接后的绝对路径 | `cb(err)` |
| `fs.truncate(path, len?, cb)` | `cb(nil)`;缺省 `len` 时截为 0(Node 式重载:`cb` 可直接跟在 `path` 后) | `cb(err)` |
| `fs.lstat(path, cb)` | `cb(nil, st)`,字段同 stat;**不跟随符号链接** | `cb(err)` |

- **IO 在线程池,回调在循环线程**:大文件读写不阻塞定时器;回调照常经隔离执行,抛错不影响循环;
- **错误是字符串**:libuv 的 `uv_strerror` 直出(如 `no such file or directory`),与 Node 的 Error 对象相比是刻意简化——Lua 里 `err ~= nil` 判定即可,stat 读不存在的路径同样走 `cb(err)`;
- **嵌套安全**:一个操作的回调里可以再发起下一个操作——keep-alive 计数容许在两个操作之间短暂归零,回调链从 `run()` 内一路接续到全部完成;
- **readdir 底层是 `uv_fs_scandir`**:一次线程池调用列出全部条目,名字在交付前拷入 Lua,列表由 libuv 自行回收;
- **`st.type` 是类型字符串**:`"file"` / `"dir"` / `"link"` / `"other"`——判类型优先读它,别去解 `mode` 位;`stat` 按定义跟随符号链接,`"link"` 只从 `lstat` 出来;
- **copyFile 覆盖、access 只探存在**:`copyFile` 没有"不覆盖"开关(dst 已在就换掉,Node 默认语义);`access` 只回答"在不在"——可读可写让 `read`/`write` 自己的 `err` 说话,不做预检;
- **truncate 也能增长**:`len` 超过原长时文件补零扩展(POSIX `ftruncate` 语义,与 Node 相同);libuv 只有 fd 版截断,实现走 `open` → `ftruncate` → `close` 三段,中途失败照常 `cb(err)` 且 fd 必然收尾。

### fs.watch:监听目录变化

`fs.watch` 是 `loop.fs` 的观察面:盯着一个目录,文件建立、改动、改名、删除都以事件交付——Linux 上是 inotify 的直接接线:

```lua
local fs = require("loop").fs

local w                       -- 拆开声明:回调里读 w(见 net 的作用域提醒)
w = fs.watch("/tmp/log", function(err, filename, event)
    if err then print(err) return end
    print(event, filename)    -- rename	note.txt
end)

loop.setTimeout(function() w:close() end, 5000)
loop.run()
```

| 调用 | 语义 |
| --- | --- |
| `fs.watch(path, onEvent)` | 监听目录 `path`(监听单文件不保证平台一致,目录是共同语义);路径不存在**同步抛错**;返回 watcher |
| `watcher:close()` | 幂等关闭;关闭后不再交付,打开的 watcher 撑着循环 |

行为约定:

- **事件回调常驻**:`onEvent(err, filename, event)` 引用由实现持有,像 `udp.bind` 的收包回调;`event` 是 `"rename"`(建立/改名/删除)或 `"change"`(内容/属性);
- **事件是合并的**:内核侧相邻变化会合成一次交付,`filename` 尽力而为(某些事件为 nil)——把它当提示,不当事实;要真相就 `fs.stat`;
- **不是递归的**:只监听目录本身,子目录内部的变化不上报——要递归就逐层 `fs.watch`;
- **watch 不保证送达**:进程崩溃前的最后一批变化、watch 建立之前的变化,一概不知——它与 tail/同步扫描是互补而非替代;
- **监视的删除以错误出场**:被监听的目录本身被删,`onEvent(err, ...)` 收到错误,watcher 随之失效。

## loop.signal:异步信号

`loop.signal` 是循环的第六块:把 Unix 信号接进回调世界——守护进程式脚本的优雅退出(收到 SIGTERM 先关 server、刷盘,再退)就是它的主场:

```lua
local loop = require("loop")

local closing = false
loop.signal(loop.sig.TERM, function(num)
    if closing then return end
    closing = true
    print("graceful shutdown on signal " .. num)
    -- 关 server、刷盘……然后最后一个句柄关闭,run() 自然返回
end)

loop.run()
```

| 调用 | 语义 |
| --- | --- |
| `loop.signal(signum, cb)` | 注册常驻信号回调 `cb(signum)`;同号多个 watcher **都会**收到(libuv 扇出);返回 watcher |
| `loop.sig.HUP / INT / QUIT / … / TERM / USR1 / USR2 / …` | 信号编号常量表(Linux 标准:`TERM`=15、`USR2`=12……) |
| `watcher:close()` | 幂等关闭;关闭后不再交付 |

行为约定:

- **SIGINT 与 SIGUSR1 拒绝注册**:`^C` 是循环自己的中断键(超时以 `interrupted` 抛错、退出码 130),SIGUSR1 是 attach 的门铃——这两路各有主人,`signal` 同步抛错,别碰;
- **最后一个 watcher 关闭 = 恢复默认处置**:对同一信号的最后一个 watcher `close()` 之后,libuv 撤销自家处置、恢复内核默认——再来的信号该终止进程就终止进程。想让进程对某信号"免疫",至少留一个 watcher 在——不想让它撑着循环就 `watcher:unref()`(见 [handle:unref/ref](#handlerefref谁撑着循环)):处置仍然装着,交付照常,只是不阻止 `run` 排空;
- **SIGKILL/SIGSTOP 编号在表里,但内核从不交付**:这两者不可捕获,是 Unix 的规矩;
- **回调隔离**:回调抛错只打到 stderr,循环继续;信号在循环间隙到达也不会丢——处置已装上,事件由 libuv 排队,下一轮 `run` 交付;
- **信号不是队列**:同号信号连发,内核不排队(标准信号合并)——回调收到的次数可能少于发送次数,要计数就在回调里自己数。

## loop.dns:异步域名解析

`loop.dns` 是循环的第七块:把 `net.connect` 内部用的线程池解析器独立成面——域名查地址、地址查域名,同一张"回调首参"契约:

```lua
local dns = require("loop").dns

dns.lookup("example.com", function(err, addr)
    assert(err == nil, err)
    print(addr)               -- 93.184.216.34(或首个 v6 地址)
end)

dns.reverse("127.0.0.1", function(err, name)
    assert(err == nil, err)
    print(name)               -- localhost
end)

require("loop").run()
```

| 调用 | 语义 |
| --- | --- |
| `dns.lookup(host, cb)` | `cb(nil, addr)`——返回列表里**第一个**地址(v4/v6 都可);解析失败 `cb(err)` |
| `dns.reverse(addr, cb)` | `cb(nil, hostname)`;`addr` 必须是**数字地址**(v4/v6),否则同步抛错;查不到名字走 `cb(err)` |

行为约定:

- **解析在线程池,回调在循环线程**:真实 DNS 查询(不命中 `/etc/hosts` 时)可能要几百毫秒,期间定时器照常转;
- **lookup 只给第一个地址**:不给全家、不做轮询——要选地址就自己 `net.connect`,连接层本来就按同样规则解析;
- **错误是字符串**:`uv_strerror` 直出(`name or service not known` 等),与 `loop.fs`/`loop.net` 相同;
- **reverse 只收数字地址**:传域名进来直接同步抛错——它不是 lookup 的反义糖,是 `getnameinfo` 的直通车。

## loop.os:系统信息(同步面)

`loop.os` 是循环的第八块,也是唯一**同步**的一面:调用即返回,不需要 `loop.run()`——它归在 loop 名下是因为同样出自 libuv(Node 把这面叫 `os`),而不是因为它进循环:

```lua
local os = require("loop").os

print(os.hostname(), os.type())         -- coding	Linux
print(os.home())                        -- /home/cui
print(#os.cpus() .. " cpus")            -- 16 cpus
for name, addrs in pairs(os.networkInterfaces()) do
    for _, a in ipairs(addrs) do
        print(name, a.family, a.address, a.internal)
    end
end
```

| 调用 | 返回 |
| --- | --- |
| `os.home()` | 用户主目录(`$HOME` 或 passwd 条目) |
| `os.tmpdir()` | 临时目录(`$TMPDIR` 或 `/tmp`) |
| `os.hostname()` | 主机名 |
| `os.type()` | 内核名(`"Linux"`) |
| `os.uptime()` | 系统运行秒数(number) |
| `os.loadavg()` | 三个负载值 `{1min, 5min, 15min}` |
| `os.freemem()` / `os.totalmem()` | 空闲/总物理内存(字节) |
| `os.cpus()` | 每逻辑 CPU 一项:`{model, speed, times={user, nice, sys, idle, irq}}`,times 单位毫秒 |
| `os.networkInterfaces()` | 以接口名为键:每个名字下是地址数组,每项 `{address=, family="IPv4"/"IPv6", mac=, internal=}`——Node 的形状 |

行为约定:

- **同步、无回调、无循环**:这些都是读系统状态,瞬间返回;错误(拿不到等)直接抛错而不是走回调——异步契约只属于会等待的操作;
- **cpus 的 times 是累计值**:从开机起累加,要算利用率取两次采样差值;
- **networkInterfaces 的 `internal`** 标记回环类接口;`mac` 恒为 `xx:xx:xx:xx:xx:xx` 形状(没有 mac 的虚拟接口为全零)。

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

## loop.net:TLS(connectTls / listenTls)

net 面的 TLS 一对:客户端 `connectTls` 与服务端 `listenTls`。连接、证书校验、握手全部走事件循环,握手完成后交付的 `sock` 与普通 TCP 套接字同一套 `write` / `read` / `close` 契约——载荷加密在实现里完成,回调首参约定不变:

```lua
local net = require("loop").net

net.connectTls("example.com", 443, function(err, sock)
    assert(err == nil, err)          -- 证书/握手失败从这里出来
    sock:write("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n", function(e)
        assert(e == nil, e)
        sock:read(function(e2, chunk)
            if chunk then io.write(chunk) end
            sock:close()
        end)
    end)
end)

loop.run()
```

服务端与 `net.listen` 同构:opts 带上 `cert` / `key` 文件路径,每个完成握手的连接交给常驻的 `onConn` 回调:

```lua
local net = require("loop").net

local srv
srv = net.listenTls("127.0.0.1", 0, {   -- 0 = 临时端口
    cert = "server-cert.pem",
    key = "server-key.pem",
}, function(err, sock)
    assert(err == nil, err)
    sock:read(function(e, chunk)
        if chunk then sock:write(chunk) else sock:close() end
    end)                                -- 一个 TLS echo 服务端
end)
print("listening on", srv:port())

loop.run()
```

| 调用 | 语义 |
| --- | --- |
| `net.connectTls(host, port, opts?, cb)` | 异步解析 + TCP 连接 + TLS 握手;`cb(err, sock)`;opts 可省略 |
| `net.listenTls(host, port, opts, onConn)` | 监听 TCP + TLS;opts 的 `cert`/`key` 必填;返回 server(`port` / `close` / `unref` / `ref` 同 net 面) |
| `sock:write(data, cb(err)?)` | 加密写出,送达后回调;握手未完时载荷先挂起,握手完成后自动冲出 |
| `sock:read(cb)` | 解密后的明文按块交付;对端 close_notify 是 `cb(nil, nil)`,且连接随即收尾 |
| `sock:close(cb?)` / `sock:unref()` / `sock:ref()` | 同 net 面语义 |

行为约定:

- **证书校验默认开启**:客户端按系统 CA 库 + 主机名核对(SNI 随 host 发出);校验不过(自签、域名不符、过期)握手失败,错误从 `cb(err)` 出来;
- **opts 定制校验**:`insecure = true` 跳过校验(自签开发服务器用);`ca = "路径"` 用指定 CA 文件替代系统库;服务端 `cert`/`key` 加载失败是**同步抛错**(与 listen 的 bind 失败同款);
- **失败即收尾**:客户端解析、连接、握手任一步失败,`cb(err, nil)` 一次,半建的 socket 自行清理;**服务端握手失败的连接被静默丢弃**,`onConn` 只收完成握手的对端;
- **EOF 即终结**:对端发 close_notify 后 read 拿到 `(nil, nil)`,连接整体关闭(与 TCP 的半关闭不同,TLS 面没有 `shutdown`);
- **关 listener 不及于存量**:server:close 只停监听,已建立的连接各自继续;
- **构建依赖**:luna 带 OpenSSL 构建时可用;不带时两个入口都报错说明如何启用,其余 loop 面不受影响。

## loop.udp:异步数据报

`loop.udp` 是循环的第四块:数据报一面:无连接、保序不做、送达不保——bind 一端常驻收包,socket 一端按包发送,同一张"回调首参"契约:

```lua
local udp = require("loop").udp

local r                       -- 拆开声明:回调里读 r(见 net 的作用域提醒)
r = udp.bind("0.0.0.0", 0, function(err, data, rinfo)   -- 0 = 临时端口
    if err then print(err) return end
    print(data, rinfo.addr, rinfo.port)
    r:close()
end)
print("listening on", r:port())

local s = udp.socket()        -- 未绑定:首次 send 自动绑临时端口
s:send("ping", "127.0.0.1", r:port(), function(err)
    assert(err == nil, err)
    s:close()
end)

loop.run()
```

| 调用 | 语义 |
| --- | --- |
| `udp.bind(host, port, onMsg)` | 绑定数字地址(`"0.0.0.0"` = 全接口),`port 0` = 临时端口(`sock:port()` 读回);每个数据报交付一次 `onMsg(nil, data, rinfo)`,rinfo = `{addr=, port=}` |
| `udp.socket()` | 未绑定的发送端;首次 `send` 时 libuv 自动绑临时端口 |
| `sock:send(data, host, port, cb(err)?)` | 发一个数据报(host 须为数字地址);回调按包一次性交付,载荷由实现持有到回调落地 |
| `sock:port()` | 实际绑定的端口(临时端口读回用) |
| `sock:close()` | 幂等关闭;打开的 sock 撑着循环——完事必须关 |

行为约定:

- **bind 冲突同步抛错**:端口已被占时 `bind` 直接 raise——与 `listen` 同款;
- **消息回调常驻**:引用由实现持有(收包版的 `onConn`);**收包错误也从它的 `err` 出来**,socket 不因此拆掉;
- **UDP 不保证送达**:回环内发出即到,跨网丢包、乱序、重复都是常态——重试与去重是协议层的事,`loop.udp` 不代劳;
- **send 后立刻 `close` 是安全的**:载荷由实现持有到回调落地;被关闭取消的 send 其回调以 `err` 收场(若给了回调);
- **错误是字符串**:与 `loop.net` 相同,`uv_strerror` 直出。

## loop.process:异步子进程

`loop.process` 是循环的第五块,两个入口:`run` 是 `child_process.exec` 的聚合兄弟——不开 shell,拉起子进程,把它的 stdout/stderr 收进内存,等**退出且双管道排空**后一次交付;`spawn` 则把三路 stdio 直接交成普通的 `loop.net` sock,流式收发:

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
