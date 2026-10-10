# stdlib · yaml

YAML 1.1 编解码、多文档流。后端 [lyaml](https://github.com/gvvaughan/lyaml) 6.2.9(deps 内 stage 副本,require 路径已适配)。

## API

| 调用 | 说明 |
| --- | --- |
| `yaml.decode(str, opts?)` | 解析首个文档;`opts.nullval = yaml.null` 可把 null 落成哨兵(缺省落 `nil`) |
| `yaml.decodeAll(str)` | 解析 `---` 分隔的全部文档,返回数组 |
| `yaml.encode(value, opts?)` | 编码为 YAML 文本;`opts.anchors = true` 开启自动锚点(共享引用发 `&aN`/`*aN`,环也可编码) |
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

null 两侧不对称:编码侧用 `yaml.null` 哨兵表达(落成 `~`);解码侧缺省把 `~`、空值、`null` 一律落 `nil`,与"字段缺席"不可区分——要区分就传 `opts.nullval` 把哨兵接回来:

```lua
local yaml = require "yaml"
local t = yaml.decode("value: ~\n")
print(t.value, t.value == nil)
local u = yaml.decode("value: ~\n", { nullval = yaml.null })
print(u.value == yaml.null)
print(yaml.encode({ v = yaml.null }))
```

```text
nil	true
true
---
v: ~
...
```

自动锚点(缺省关闭):缺省下同一张表被引用两次会各编码一份副本,自引用环直接报 `yaml: cyclic table reference`;`opts.anchors = true` 给被引用多次的表在首现处发 `&aN` 锚点、重复处发 `*aN` 别名,decode 侧把别名解回共享引用——引用恒等与环结构都能原样往返:

```lua
local yaml = require "yaml"
local shared = { 1, 2 }
io.write(yaml.encode({ shared, shared }, { anchors = true }))
local t = yaml.decode(yaml.encode({ shared, shared }, { anchors = true }))
print(t[1] == t[2])
local cyc = {}
cyc.self = cyc
io.write(yaml.encode(cyc, { anchors = true }))
local back = yaml.decode(yaml.encode(cyc, { anchors = true }))
print(back.self == back)
```

```text
---
- &a1
  - 1
  - 2
- *a1
...
true
--- &a1
self: *a1
...
true
```

锚点名按发现序编号(a1、a2…),多个共享表时与同层键的先后一样跟随 `pairs`(每次可能不同);不带共享表时输出与缺省模式逐字节一致。lyaml 原有的预声明形态 `opts.anchors = { name = value }` 意思不变(按类型区分两种形态)。

## 契约与边界

- YAML 1.1 口径(lyaml 所实现的子集):`yes/no/on/off` 等布尔字面量按 1.1 规则处理;需要严格 1.2 的项目请注意差异。
- `decode` 只取首个文档,多文档流请用 `decodeAll`。
- null 不对称(见上):编码侧的 null 只能是 `yaml.null` 哨兵;解码侧缺省落 `nil`,`opts.nullval = yaml.null` 才回流哨兵。
