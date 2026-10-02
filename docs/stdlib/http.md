# stdlib · http

HTTP 的"一行起服"面:`http.serve()` 把起一个服务压到一次调用——静态目录或可编程 handler,两种模式同一入口。客户端面是 luasocket 的 `socket.http` 原样再导出(`request()` 等);事件循环一侧的异步客户端(重定向、超时、流式、TLS)在 `loop.http`,见 [loop](/guide/loop)。

## serve:两种模式,一次调用

| 调用 | 模式 |
| --- | --- |
| `http.serve()` | 静态**当前目录**,端口 8000 |
| `http.serve("./docs")` | 静态目录,端口 8000 |
| `http.serve("./docs", 9000)` | 静态目录,指定端口 |
| `http.serve(handler)` | 可编程 handler,端口 8000 |
| `http.serve(handler, 9000)` | 可编程 handler,指定端口 |
| `http.serve{dir = "...", handler = fn, port = 9000, host = "127.0.0.1", quiet = true}` | 表单全参 |

默认值:`python -m http.server` 同款——目录 `"."`、端口 **8000**、host **0.0.0.0**(横幅显示 localhost);`dir` 与 `handler` 同给时 **handler 赢**。

返回服务对象:

| 字段/方法 | 说明 |
| --- | --- |
| `srv.port` | 真实端口(传 0 随机分配后回读) |
| `srv.url` | `"http://localhost:8000/"` 形态 |
| `srv.dir` / `srv.host` | 静态模式的根目录(绝对化后)/ 监听 host |
| `srv:address()` | `{address = "0.0.0.0", family = "inet", port = 8000}` |
| `srv:close(cb?)` | 清保活、关监听,此后事件循环正常收口 |

启动横幅走 **stderr**(管道不污染),`quiet` 关掉。

**错误形态**(抛,消息两条):目录不存在 → `http.serve: no such directory: /nope-dir-404`;端口占用 → `http.serve: cannot listen on 0.0.0.0:8000 (listen failed: address already in use)`——括号里是 loop.net 的原因原文。

CLI 快路:`luna serve [目录] [端口]`,零脚本起静态服务——目录与端口两个参数不限顺序(`luna serve 9000` = 当前目录、9000 端口)。参数面:`-h`/`--help` 出用法(退出码 0);未知开关、端口写两次 → 用法 + 退出码 **2**;目录不存在 → 退出码 **1**;端口须为 1–65535 整数。`^C` 停服,退出码 **130**。

## 静态模式

路由是一条判定链,每步的出口:

| 顺序 | 条件 | 响应 |
| --- | --- | --- |
| 1 | 方法非 GET/HEAD | `405` + `Allow: GET, HEAD` |
| 2 | 路径含 NUL 字节 | `400` |
| 3 | 任何 `..` 段(任何编码包裹) | `403`,正文带原路径 |
| 4 | 属性取不到 | `404`,正文带原路径 |
| 5a | 目录、URL 无尾斜杠 | `301` + `Location: <path>/` |
| 5b | 目录、有 `index.html` | 回 index 文件 |
| 5c | 目录、无 index | HTML 目录列表 |
| 6 | 文件 | 按扩展名定 Content-Type 回文件 |

一圈实测(服务起在 `./docs`,真实输出):

```text
HTTP/1.1 200 OK                        ← GET /a.txt
content-type: text/plain; charset=utf-8
Content-Length: 10

hello luna

HTTP/1.1 301 Moved Permanently         ← GET /sub(裸目录)
location: /sub/
Content-Length: 0

HTTP/1.1 200 OK                        ← HEAD /a.txt(空体,类型照发)
Content-Length: 0

404 Not Found: /nope.txt               ← GET /nope.txt
405 Method Not Allowed: POST           ← POST /a.txt(allow: GET, HEAD)
403 Forbidden: /../etc/passwd          ← GET /../etc/passwd
```

目录列表实文(`GET /sub/`):目录在前(带 `/` 后缀)、大小写不敏感字节序、非根路径附 `../` 上链、页脚 `luna serve`:

```html
<!doctype html><html><head><meta charset="utf-8">
<title>Index of /sub/</title></head><body>
<h1>Index of /sub/</h1><hr><ul>
<li><a href="../">../</a></li>
</ul><hr>luna serve</body></html>
```

Content-Type 按扩展名(html/css/js/json/md/txt/lua/yaml/svg/png/jpg/gif/webp/ico/woff/woff2/ttf/pdf/wasm/zip/gz/mp3/mp4/webm/csv/xml/map 等,文本类带 `charset=utf-8`),未知类型 `application/octet-stream`。

## 可编程模式

handler 收 `(req, res)`:

| 侧 | 字段/方法 | 说明 |
| --- | --- | --- |
| req | `method` / `url` | 方法 / 原始目标(**含**查询串) |
| req | `path` / `query` | 去查询串的路径 / 解码后的查询表 |
| req | `headers` | 请求头表,**键全小写** |
| req | `body` | 请求体字符串,可空 |
| res | `res.status = n` | 状态码(默认 200),发送前设置 |
| res | `res:setHeader(k, v)` | 自定义头;键转小写存,同名后写覆盖前写 |
| res | `res:send(body?)` | 文本响应,缺省 `text/plain; charset=utf-8`;`send()` 空体 |
| res | `res:json(value, status?)` | JSON 响应(`application/json; charset=utf-8`);**第二参直接定状态码**,比先赋 `res.status` 少一行 |

`query` 解码口径:`%XX` 还原、查询串里 `+` 视作空格、无 `=` 的裸旗标解码为空串、重名键**后到赢**(`?n=1&n=2` 得 `{n = "2"}`)。

`setHeader("Content-Type", ...)` 覆盖 `send`/`json` 的默认类型——响应头里的键以小写出场:

```lua
local http = require "http"
http.serve(function(req, res)
  if req.path == "/api" then
    res:json({ hello = "luna", youAsked = req.query.q })
  else
    res.status = 404
    res:json({ err = req.path .. " not found" })
  end
end, 9000)
```

```bash
$ curl 'http://localhost:9000/api?q=hi'
{"youAsked":"hi","hello":"luna"}
$ curl http://localhost:9000/other
{"err":"/other not found"}
```

键序跟随 `pairs`,以运行时为准——别对 JSON 对象键序做断言。

handler 抛错且还没发送任何东西时,调用方拿到 **500**(空体)——接口服务不用自己写兜底:

```text
HTTP/1.1 500 Internal Server Error
Content-Length: 0
```

## 起服与退出契约

- 服务是**活句柄**:监听与一个内部心跳一起把事件循环撑住,脚本尾部自动排水([loop](/guide/loop) 的 maybeDrain 契约)不会在服务活着时结束进程——服务器就该一直跑;
- `^C` 落到 `loop.run()` 的 `interrupted` 错误:`luna serve` 与 `luna -e` 起服都即应,退出码 **130**;
- `srv:close()` 之后心跳清掉,循环排空、进程正常收尾——测试与后台收尾用这个,不是 `^C`。

```lua
-- 测试姿势:起在随机口,收尾 close
local http = require "http"
local srv = http.serve{ handler = function(req, res) res:send("ok") end,
                        port = 0, quiet = true }
print(srv.port > 0, srv.url:find(":" .. srv.port .. "/", 1, true) ~= nil)
srv:close()
```

```text
true	true
```

## 客户端面(luasocket)

`require "http"` 同时再导出 luasocket `socket.http` 的全部(`request` 及其选项字段)。两种姿势:

```lua
local h = require "socket.http"     -- 或 require "http",同一张表
-- 简单形:返回 (body, code, headers, status_line),四个值全给
local body, code, headers, line = h.request("http://127.0.0.1:8000/a.txt")
print(body, code, line)             -- hello luna  200  HTTP/1.1 200 OK
-- 通用形:ltn12 sink 自己接(POST、流式、自定义头都走这里)
```

失败(连接被拒等)简单形返回 `nil, err`;非 2xx **不算失败**——`code` 带 404/500 回来,body 照给,分支自己写。

**HTTPS 不在面上**:请求 https 地址会以 `module 'ssl.https' not found` 抛错(luasocket 的 https 分支需要 LuaSec,luna 不捆绑)——TLS 客户端请走 `loop.http` 的 `connectTls` 面。
