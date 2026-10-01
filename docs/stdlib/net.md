# stdlib · net

TCP/UDP socket。后端 [luasocket](https://github.com/lunarmodules/luasocket):**同步阻塞**模型,完整 `socket` 命名空间经 `require "socket"` 仍可达,本模块把常用工厂提到一层。事件循环一侧的异步面(`connect`/`listen`/`connectTls`/`listenTls`、Unix socket)在 [loop](/guide/loop) 的 `loop.net`,服务端与 TLS 都在那里——本页适合脚本里的一次性客户端场景。

## API

| 调用 | 说明 |
| --- | --- |
| `net.tcp()` | 新 TCP master socket(`:connect(host, port)` 后用) |
| `net.connect(host, port)` | 直连,返回 client socket |
| `net.bind(address, port)` | 绑 master socket(`:listen()` + `:accept()`) |
| `net.udp()` | 新 UDP socket |
| `net.select(socksT, socksR[, timeout])` | 就绪集轮询 |
| `net.dns` | DNS 面(`resolve`/`toip` …) |

socket 对象遵循 luasocket 语义:`:send(str)`、`:receive("*l"|n)`、`:close()`、`:settimeout(sec)`。

## 用法

回环一发:连接本机 [http](/stdlib/http) 服务取个响应头,也是脚本里最常见的姿势——

```lua
local net = require "net"
local s = net.connect("127.0.0.1", 8000)
s:send("GET / HTTP/1.0\r\nHost: localhost\r\n\r\n")
local status = s:receive("*l")
print(status:find("200 OK", 1, true) ~= nil)
s:close()
```

```text
true
```

DNS 解析:

```lua
local net = require "net"
print((net.dns.toip("localhost")))
```

```text
127.0.0.1
```

(`toip` 实际返回两个值——IP 串和解析明细表,这里括号截取第一个。)

## 契约与边界

- **阻塞调用**:`receive` 没数据就挂着——事件循环里请用 `loop.net`,本页留给同步脚本。
- 超时缺省按 luasocket 全局(块级阻塞);严谨脚本显式 `:settimeout(5)`。
