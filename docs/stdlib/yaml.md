# stdlib · yaml

YAML 1.1 编解码、多文档流。后端 [lyaml](https://github.com/gvvaughan/lyaml) 6.2.9(deps 内 stage 副本,require 路径已适配)。

## API

| 调用 | 说明 |
| --- | --- |
| `yaml.decode(str)` | 解析首个文档 |
| `yaml.decodeAll(str)` | 解析 `---` 分隔的全部文档,返回数组 |
| `yaml.encode(value)` | 编码为 YAML 文本 |
| `yaml.null` | 编码侧的 null 哨兵:`encode` 时落成 `~`;`decode` 侧 `~`/空/`null` 一律落 `nil`,哨兵不回流 |
| `yaml._VERSION` | lyaml 版本串 |

## 用法

```lua
local yaml = require "yaml"
local t = yaml.decode([[
name: luna
features:
  - repl
  - serve
nested:
  deep: true
]])
print(t.name, t.features[2], t.nested.deep)
```

```text
luna	serve	true
```

多文档流:

```lua
local yaml = require "yaml"
local docs = yaml.decodeAll("---\nok: true\n---\nok: false\n")
print(#docs, docs[1].ok, docs[2].ok)
```

```text
2	true	false
```

编码(lyaml 风格,`---`/`...` 文档标记包边;同一层多个键的先后跟随 `pairs`,每次可能不同):

```lua
local yaml = require "yaml"
io.write(yaml.encode({ list = { 1, 2 } }))
io.write(yaml.encode({ map = { a = 1 } }))
```

```text
---
list:
- 1
- 2
...
---
map:
  a: 1
...
```

null 两侧不对称:编码侧用 `yaml.null` 哨兵表达(落成 `~`);解码侧 `~`、空值、`null` 一律落 `nil`,与"字段缺席"不可区分:

```lua
local yaml = require "yaml"
local t = yaml.decode("value: ~\n")
print(t.value, t.value == nil)
print(yaml.encode({ v = yaml.null }))
```

```text
nil	true
---
v: ~
...
```

## 契约与边界

- YAML 1.1 口径(lyaml 所实现的子集):`yes/no/on/off` 等布尔字面量按 1.1 规则处理;需要严格 1.2 的项目请注意差异。
- `decode` 只取首个文档,多文档流请用 `decodeAll`。
- null 不对称(见上):`yaml.null` 只在编码侧有意义,解码产物里不会出现它。
