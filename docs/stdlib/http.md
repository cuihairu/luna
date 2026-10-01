# stdlib · http

HTTP 的"一行起服"面:`http.serve()` 把起一个服务压到一次调用——静态目录或可编程 handler,两种模式同一入口。客户端面是 luasocket 的 `socket.http` 原样再导出(`request()` 等);事件循环一侧的异步客户端(重定向、超时、流式、TLS)在 `loop.http`,见 [loop](/guide/loop)。

## serve:两种模式,一次调用

| 调用 | 模式 |
| --- | --- |
| `http.serve("./docs")` | 静态目录,端口 8000 |
| `http.serve("./docs", 9000)` | 静态目录,指定端口 |
| `http.serve(handler)` | 可编程 handler,端口 8000 |
| `http.serve(handler, 9000)` | 可编程 handler,指定端口 |
| `http.serve{dir = "...", handler = fn, port = 9000, host = "127.0.0.1", quiet = true}` | 表单全参 |

返回服务对象:`srv.port`(真实端口,传 0 随机分配后回读)、`srv.url`、`srv.dir`(静态模式)、`srv:close()`(清保活、关监听,此后事件循环正常收口)、`srv:address()`。启动横幅走 **stderr**(管道不污染),`quiet` 关掉。

CLI 快路:`luna serve [目录] [端口]`,零脚本起静态服务——目录与端口两个参数不限顺序(`luna serve 9000` = 当前目录、9000 端口);`--help` 看用法。

## 静态模式

```bash
$ luna serve ./docs 8000
luna: serving /path/to/docs at http://localhost:8000/ (^C to stop)
```

- `/` 先找 `index.html`,没有则目录列表;子目录同规则,裸目录 url 301 补尾斜杠(相对链接才正确);
- `..` 段一律 **403**——不管怎么编码包裹,都不进文件系统;
- Content-Type 按扩展名(html/css/js/json/md/png/svg/pdf/字体/媒体等,文本类带 `charset=utf-8`),未知类型 `application/octet-stream`;
- 只答 GET/HEAD,其余 405;404 带原路径;`^C` 即停,退出码 130(与 luna 全局 `^C` 契约一致)。

## 可编程模式

handler 收 `(req, res)`:

| 侧 | 字段/方法 | 说明 |
| --- | --- | --- |
| req | `method` / `url` | 方法 / 原始目标(含查询串) |
| req | `path` / `query` | 去查询串的路径 / 解码后的查询表 |
| req | `headers` / `body` | 小写键头表 / 请求体字符串(可空) |
| res | `res.status = n` | 状态码(默认 200),发送前设置 |
| res | `res:setHeader(k, v)` | 自定义头 |
| res | `res:send(body?)` | 文本响应(缺省 `text/plain; charset=utf-8`) |
| res | `res:json(value)` | JSON 响应(`application/json; charset=utf-8`) |

handler 抛错且还没发送任何东西时,调用方拿到 500——接口服务不用自己写兜底。

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

`http.request(url)` / `http.request{url = ..., sink = ...}` 等与 luasocket 文档一致,原样可用;HTTPS 需要 LuaSec,luna 不捆绑——TLS 客户端请走 `loop.http` 的 `connectTls` 面。
