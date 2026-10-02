# stdlib · net

TCP/UDP socket。后端 [luasocket](https://github.com/lunarmodules/luasocket):**同步阻塞**模型,完整 `socket` 命名空间经 `require "socket"` 仍可达,本模块把常用工厂提到一层。事件循环一侧的异步面(`connect`/`listen`/`connectTls`/`listenTls`、Unix socket)在 [loop](/guide/loop) 的 `loop.net`,服务端与 TLS 都在那里——本页适合脚本里的一次性客户端场景。

## API

| 调用 | 说明 |
| --- | --- |
| `net.tcp()` | 新 TCP master socket(`:connect(host, port)` 后用) |
| `net.connect(host, port)` | 直连,返回 client socket;失败 `nil, err`(如 `connection refused`) |
| `net.bind(address, port)` | 绑 master socket(`:listen()` + `:accept()`) |
| `net.udp()` | 新 UDP socket |
| `net.select(socksT, socksR[, timeout])` | 就绪集轮询 |
| `net.dns` | DNS 面(`resolve`/`toip` …) |

socket 对象遵循 luasocket 语义:

| 方法 | 说明 |
| --- | --- |
| `s:send(str)` | 发送;返回字节数(如 `12.0`),失败 `nil, err` |
| `s:receive("*l" \| "*a" \| n)` | 收一行 / 收到连接关闭 / 收 n 字节;失败 `nil, err`(`timeout`/`closed`) |
| `s:settimeout(sec)` | 收发超时秒数;不设则块级阻塞 |
| `s:getsockname()` | 本端 `ip, port` |
| `s:close()` | 关闭 |

**错误形态**(luasocket 口径):失败一律 `nil, err`,错误是字符串——`net.connect` 拨不上的地址返回 `nil	connection refused`,`receive` 超时返回 `nil, "timeout"`。判定 `err ~= nil` 即可。

UDP 面一次走完(`setsockname` 绑定、`sendto`/`receivefrom` 收发;无连接,`receivefrom` 附带返回对端地址):

```lua
local net = require "net"
local u = net.udp()
u:setsockname("127.0.0.1", 0)          -- 0 = 系统挑空闲端口
local _, port = u:getsockname()
print(u:sendto("ping", "127.0.0.1", port))
print(u:receivefrom())
u:close()
```

```text
4.0
ping	127.0.0.1	42618
```

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
