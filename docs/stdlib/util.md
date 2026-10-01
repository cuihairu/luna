# stdlib · util

Node `util` 的 `format`/`inspect` 面(promisify/types 无 Lua 契约锚,明确不做)。语义按 Node v24.21.0 实证钉版,选型记录见[Node 方向选型](/node-parity)。

## API

| 调用 | 说明 |
| --- | --- |
| `util.format(fmt, ...)` | printf 风格:`%s %d %i %f %j %o %O %c` 与 `%%`;扩展 `%x`/`%X` 十六进制 |
| `util.inspect(value)` | 值渲染为 **Lua 源形状**,键排序保确定性 |

`format` 细节(与 Node v24 实测对齐):`%d`/`%f` 不截断,`%i` 向零截断;无格式符时参数以空格尾接;未知转换符原样保留;`%j` 走 [json](/stdlib/json) 编码。

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

## 契约与边界

- 数字拼写按 JS `String(number)` 口径:整值浮点不带 `.0`(`3.0` 打成 `3`),`NaN`/`Infinity` 按原词。
- `inspect` 的表键排序是刻意的:`pairs` 序不定,与 Node 的插入序无法对齐,确定性优先——记为适配,不记为缺陷。
