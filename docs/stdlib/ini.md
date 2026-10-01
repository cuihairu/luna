# stdlib · ini

经典 .ini 编解码。LPeg 语法驱动(luna 自带 lpeg,零新依赖)。

## API

| 调用 | 说明 |
| --- | --- |
| `ini.decode(str, opts?)` | 解析为表:`[name]` 段落,重复段合并,段落前的键落在根表;`opts.cast` 为真时把数字与小写 `true`/`false` 转型 |
| `ini.encode(tbl, opts?)` | 编码回 ini 文本 |

方言:整行注释(`#` 或 `;`,可有前导空白),**没有行内注释**——值里的 `#` 是数据;`"..."` 引号值支持 `\"` 与 `\\` 转义,其余反斜杠原样;重复键取最后;键不以 `[` 开头且不含 `=`/换行,否则先按段落行、再按键行回退解析。

## 用法

```lua
local ini = require "ini"
local conf = ini.decode([[
# 全局默认
debug = true
[server]
host = 127.0.0.1
port = 8080
[log]
level = info
]])
print(conf.debug, conf.server.host, conf.server.port, conf.log.level)
```

```text
true	127.0.0.1	8080	info
```

不加 `opts.cast`,一切皆字符串(上面 `debug` 是字符串 `"true"`);开 cast 转型:

```lua
local ini = require "ini"
local conf = ini.decode("debug=true\nport=8080\n", { cast = true })
print(conf.debug, type(conf.debug), conf.port, type(conf.port))
```

```text
true	boolean	8080	number
```

编码对称(节序与节内键序都跟随 `pairs`,要确定的文本就把要输出的表按序排好):

```lua
local ini = require "ini"
io.write(ini.encode({ server = { host = "127.0.0.1" } }))
```

```text
[server]
host = 127.0.0.1
```

坏数据 `nil, err`(带行列):

```lua
local ini = require "ini"
local t, err = ini.decode("[sec\nkey=val\n")
print(t, err)
```

```text
nil	ini: section header missing ']' at line 1, column 5
```

## 契约与边界

- `cast` 只转数字与 `true`/`false`(小写);其余字面量保持字符串。
- 编码不含注释——注释是解析期的损耗,encode 不做往返保真。
