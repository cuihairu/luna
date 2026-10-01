# stdlib · csv

RFC 4180 风格 CSV 编解码。LPeg 语法驱动(luna 自带 lpeg,零新依赖)。

## API

| 调用 | 说明 |
| --- | --- |
| `csv.decode(str, opts?)` | 解析为**行数组,每行是字段数组**;字段恒为字符串(CSV 无类型,转换是调用者的事,同 Node csv-parse 默认) |
| `csv.encode(rows, opts?)` | 行数组编码;`opts.eol` 缺省 CRLF(RFC 4180 wire 格式),**不追加尾 eol** |
| `csv.lines(str)` | 逐行迭代器,产出每行字段数组;坏数据 **raise**(迭代器的 nil 即结束,没地方放错误) |

## 用法

```lua
local csv = require "csv"
local rows = csv.decode('name,age\n"Zhang, San",33\nLi,25\n')
for _, r in ipairs(rows) do print(r[1], r[2]) end
```

```text
name	age
Zhang, San	33
Li	25
```

引号转义对称地发生在编码端;行分隔按 RFC 4180 用 CRLF(`opts.eol` 可换),示例里显化出来:

```lua
local csv = require "csv"
print((csv.encode({ { "plain", 42 }, { "has, comma", 'has "quote"' } })
       :gsub("\r\n", "\\r\\n")))
```

```text
plain,42\r\n"has, comma","has ""quote"""
```

逐行迭代,大文件友好:

```lua
local csv = require "csv"
for r in csv.lines("a,b\n1,2\n3,4\n") do print(r[1] .. ":" .. r[2]) end
```

```text
a:b
1:2
3:4
```

坏数据返回 `nil, err`,消息带行列(列按 UTF-8 字符数):

```lua
local csv = require "csv"
local rows, err = csv.decode('a,"unterminated\nb,c\n')
print(rows, err)
```

```text
nil	csv: unterminated quoted field at line 3, column 1
```

## 契约与边界

- 接受的行尾:CRLF 与 LF;孤 CR 拒绝。字段不修剪——空白是数据。
- `lines()` 遇坏数据 raise,喂不可信输入请 `pcall`;`decode`/`encode` 永远 `nil, err`。
