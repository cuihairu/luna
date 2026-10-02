# stdlib · json

JSON 编解码。后端 [dkjson](https://dkolf.dk/dkjson)(纯 Lua,UTF-8 感知,零依赖)。Node 生态的 `parse`/`stringify` 别名同款提供,两边命名都顺手。

## API

| 调用 | 说明 |
| --- | --- |
| `json.encode(value, opts?)` | 编码为 JSON 字符串;`opts` 透传 dkjson(`indent`/`keyorder`,见下节) |
| `json.decode(str, pos?)` | 从 `pos`(默认 1)解码;坏数据返回 `nil, err`——`err` 是出错处的字符位置(1 基数字,dkjson 口径) |
| `json.parse(str)` | `decode` 的 Node 式别名 |
| `json.stringify(value)` | `encode` 的 Node 式别名 |

## encode 的选项

`opts` 原样透传给 dkjson,两个最常用:

- **`indent = true`**:多行美化输出(两空格缩进);
- **`keyorder = {...}`**:对象键按给定表排序输出。Lua 的 `pairs` 键序跨进程不稳定,要让编码结果**逐字节可复现**(写测试断言、生成要 diff/哈希的文件),这是正解:

```lua
local json = require "json"
print(json.encode({b=1,a=1}, {indent=true, keyorder={"a","b"}}))
```

```text
{
  "a":1,
  "b":1
}
```

不指定 `keyorder` 时键序跟随 `pairs`——每次运行都可能不同,别对输出顺序做断言;指定了的键按表序,没指定的仍随 `pairs`。


## 用法

数组按序还原;对象走哈希表,键序跟随 `pairs`——**每次运行都可能不同**,对键序敏感的输出请逐键拼装或改用数组:

```lua
local json = require "json"
print(json.encode({ "luna", true, 42 }))
local text = json.encode({ name = "luna", tags = { "lua", "repl" }, ok = true })
print(text:find("\"name\":\"luna\"", 1, true) ~= nil,
      text:find("\"tags\":[\"lua\",\"repl\"]", 1, true) ~= nil)
```

```text
["luna",true,42]
true	true
```

```lua
local json = require "json"
local t = json.decode('{"n": 3, "pi": 3.14, "nested": [1, {"deep": true}]}')
print(t.n, t.pi, t.nested[2].deep)
```

```text
3	3.14	true
```

坏数据返回 `nil, err`,不抛——`err` 不是消息串,是出错处的字符位置(`{"n": ` 共 6 个字符,第 7 位上断掉):

```lua
local json = require "json"
local v, err = json.decode('{"n": ')
print(v, err)
```

```text
nil	7
```

`stringify`/`parse` 是同义别名:

```lua
local json = require "json"
print(json.stringify({ x = 1 }) == json.encode({ x = 1 }))
print(json.parse('[1, 2, 3]')[2])
```

```text
true
2
```

## 契约与边界

- JSON `null` 解码为 Lua `nil`——"值为 null"与"字段缺席"不可区分,判空按业务口径;编码侧 `nil` 字段直接缺席,纯 table 表达不了 JSON null。
- 数字:整数保持整数;超出精度按 Lua 数值语义。
- 编码非法值(函数、循环表)**抛**(`pcall` 承接,消息形如 `type 'function' is not supported by JSON.`),与 `decode` 的 `nil, err` 口径不同。
