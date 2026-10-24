# stdlib · util

Node `util` 的 `format`/`inspect`/`parseArgs`/`parseEnv`/`stripVTControlCharacters`/`styleText`/`debuglog` 面(promisify/types 无 Lua 契约锚,明确不做)。语义按 Node v24.21.0 实证钉版,选型记录见[Node 方向选型](/zh/node-parity)。

## API

| 调用 | 说明 |
| --- | --- |
| `util.format(fmt, ...)` | printf 风格:`%s %d %i %f %j %o %O %c` 与 `%%`;扩展 `%x`/`%X` 十六进制 |
| `util.inspect(value, opts?)` | 值渲染为 **Lua 源形状**,键排序保确定性;`opts.depth`(默认 2)、`opts.maxArrayLength`(默认 100)、`opts.maxStringLength`(默认 10000) |
| `util.parseArgs(config)` | CLI 参数解析:长/短选项、内联值、`--` 终止、strict、位置参、`tokens` |
| `util.parseEnv(content)` | `.env` 文本解析为键值表(注释/`export ` 前缀/引号值可跨行/`#` 截断/重复末次胜) |
| `util.stripVTControlCharacters(str)` | 剔 ANSI/VT 控制序列(OSC 与 CSI;非终符 ESC 原样保留) |
| `util.styleText(fmt, text, opts?)` | 色名/`#RGB`/`#RRGGBB`/数组 → SGR 码包裹;默认非 TTY 原样返回 |
| `util.debuglog(section)` | 返回函数;`NODE_DEBUG` 门控,命中时向 stderr 写 `SECTION PID: msg` |

`format` 细节(与 Node v24 实测对齐):`%d`/`%f` 不截断,`%i` 向零截断;无格式符时参数以空格尾接;未知转换符原样保留;`%j` 走 [json](/zh/stdlib/json) 编码。

`parseArgs`/`parseEnv`/`styleText`/`debuglog` 的逐项勘定与偏差账见[Node 方向选型](/zh/node-parity)的 util 节。

## 用法

```lua
local util = require "util"
print(util.format("%s has %d points (%x in hex)", "luna", 255, 255))
print(util.format("%i %f %j", 3.99, 3.99, { ok = true }))
```

```text
luna has 255 points (ff in hex)
3 3.99 {"ok":true}
```

```lua
local util = require "util"
print(util.format("no placeholders", "with", 2, "extras"))
```

```text
no placeholders with 2 extras
```

`inspect` 输出 Lua 形状(`{}` 而非 JS `[]`),键排序保证同一张表每次渲染一致:

```lua
local util = require "util"
print(util.inspect({ name = "luna", version = 8, nested = { 1, 2, 3 } }))
```

```text
{ name = "luna", nested = { 1, 2, 3 }, version = 8 }
```

`parseArgs`:

```lua
local util = require "util"
local r = util.parseArgs({
  options = { name = { type = "string", short = "n" }, verbose = { type = "boolean" } },
  args = { "-n", "luna", "--verbose" },
})
print(r.values.name, r.values.verbose)
```

```text
luna	true
```

`parseEnv`(引号值可跨行,闭合引号后同行尾文丢弃;无引号值首个 `#` 截断):

```lua
local util = require "util"
local env = util.parseEnv('# note\nexport HOST=localhost\nPORT=8321 # inline\n')
print(env.HOST, env.PORT)
```

```text
localhost	8321
```

`styleText`(默认非 TTY 原样返回;`validateStream = false` 或 `stream.isTTY = true` 才出码;数组按序开码、逆序闭合):

```lua
local util = require "util"
print(util.styleText("red", "x", { validateStream = false }))
print(util.styleText({ "bold", "red" }, "x", { validateStream = false }))
```

```text
[31mx[39m
[1m[31mx[39m[22m
```

(输出里的 `[31m` 等前有一个 ESC 字节,此处展示省略。)

`debuglog`(未设 `NODE_DEBUG` 时是 no-op;条目大小写不敏感,`*` 全量、尾 `*` 前缀通配):

```lua
local util = require "util"
local debuglog = util.debuglog("mysection")
debuglog("hello %d", 42)   -- NODE_DEBUG=mysection 时: MYSECTION 12345: hello 42
```

`stripVTControlCharacters`:

```lua
local util = require "util"
print(util.stripVTControlCharacters("\27[31mred\27[0m"))
```

```text
red
```

## 契约与边界

- 数字拼写按 JS `String(number)` 口径:整值浮点不带 `.0`(`3.0` 打成 `3`),`NaN`/`Infinity` 按原词。
- `inspect` 的表键排序是刻意的:`pairs` 序不定,与 Node 的插入序无法对齐,确定性优先——记为适配,不记为缺陷。
- `parseArgs` 的 `tokens.index` 用 Lua 1 序(Node 是 0 序);`config.args` 缺省从全局 `arg` 表取参。
- `styleText` 默认按非 TTY 处理(luna 的 stdout 无 TTY 探测),显式 `validateStream = false` 或 `stream = { isTTY = true }` 才输出颜色码;嵌套重开(replaceCloseCode)不实现,记为简单包裹。
- `debuglog` 的 `NODE_DEBUG` 条目**不做空白修剪**(带空格即不匹配,与 Node 一致);消息格式化走 `util.format` 同一套口径。
- `parseEnv` 的键不剥引号、文件级剔除 `\r`(CRLF 与孤立 CR 一并);错误口径:content 非字符串才 raise。
