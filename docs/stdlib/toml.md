# stdlib · toml

TOML 编解码。`decode` 走 C 面([tomlc17](https://github.com/cktan/tomlc17),严格实现 TOML v1.1,deps 内钉版);`encode` 是 luna 自己的 Lua 面(tomlc17 不带编码器)。

## API

| 调用 | 说明 |
| --- | --- |
| `toml.decode(str)` | 解析;值平直映射 Lua,**datetime 解码为组件表** |
| `toml.encode(tbl)` | 编码;标量键先出、子表后出(TOML 语法规则使然),纯表数组渲染为 `[[header]]`,其余内联 |

datetime 组件表:日期 `{year, month, day}`,时间 `{hour, minute, second, secfrac?}`,datetime 两者兼具,offset datetime 额外带 `offset`(距 UTC 的分钟数,`Z` 为 0)。`secfrac` 只在源数据确有小数位时出现。

## 用法

```lua
local toml = require "toml"
local t = toml.decode([[
title = "luna"
tags = ["lua", "repl"]

[owner]
name = "cui"
since = 2024-01-15
]])
print(t.title, t.tags[2], t.owner.name, t.owner.since.year, t.owner.since.month)
```

```text
luna	repl	cui	2024	1
```

编码:标量在前、子表在后(TOML 语法规则使然);纯表数组走 `[[header]]`:

```lua
local toml = require "toml"
io.write(toml.encode({
  name = "demo",
  servers = { { port = 1 }, { port = 2 } },
}))
```

```text
name = "demo"

[[servers]]
port = 1

[[servers]]
port = 2
```

(同一层里多个键的先后跟随 `pairs`,可能每次不同;表块之间空行分隔。上例每层只留一个键,输出才是确定的。)

坏数据 `nil, err`(tomlc17 只报行号,无列):

```lua
local toml = require "toml"
local t, err = toml.decode("key = ")
print(t, err)
```

```text
nil	toml: missing value at line 1
```

## 契约与边界

- "看起来像 datetime 的表"按 datetime 编码——键全部来自组件集且有 `year`(或 `hour`)即触发;普通数据表天然不满足,不会误伤。
- 错误消息形如 `toml: <原因> at line N`,无列号(后端能力所限,与 csv/ini 的行列齐备不同)。
