# stdlib · xml

XML 的 DOM 三件套(`decode`/`encode`)加 SAX 透传。后端 expat(经 lua-expat 的 `lxp` 绑定)。

## API

| 调用 | 说明 |
| --- | --- |
| `xml.decode(str)` | 解析为 DOM;元素 = `{ tag, attrs, kids }`,文本节点是 `kids` 里的普通字符串 |
| `xml.encode(node)` | DOM 编码回 XML 文本 |
| `xml.sax(handlers)` | 返回 lxp parser(`:parse(str)` 逐段喂,空参 `:parse()` 收尾);回调名即 lxp 的,首参恒是 parser:`StartElement(p, name, attrs)` / `EndElement(p, name)` / `CharacterData(p, str)` |

属性值恒为字符串;命名空间前缀原样保留(`<x:a>` 的 tag 是 `"x:a"`)。

## 用法

```lua
local xml = require "xml"
local doc = xml.decode('<root id="1"><name>luna</name>rocks<nested/></root>')
print(doc.tag, doc.attrs.id, doc.kids[1].tag, doc.kids[1].kids[1], doc.kids[2])
```

```text
root	1	name	luna	rocks
```

编码对称(文本与属性自动转义):

```lua
local xml = require "xml"
print(xml.encode({ tag = "msg", attrs = { from = "luna" },
                   kids = { 'fish & chips <今日>' } }))
```

```text
<msg from="luna">fish &amp; chips &lt;今日&gt;</msg>
```

SAX:不想建 DOM 时按事件流走(回调名与首参都是 lxp 原味;喂完空参 `parse()` 收流):

```lua
local xml = require "xml"
local p = xml.sax({
  StartElement = function(_, name) print("open", name) end,
  CharacterData = function(_, s)
    if #s > 0 then print("text", s) end
  end,
  EndElement = function(_, name) print("close", name) end,
})
p:parse("<a><b>hi</b></a>")
p:parse()
```

```text
open	a
open	b
text	hi
close	b
close	a
```

坏数据 `nil, err`(行列来自 expat):

```lua
local xml = require "xml"
local doc, err = xml.decode("<a><b></a>")
print(doc, err)
```

```text
nil	xml: mismatched tag at line 1, column 9
```

## 契约与边界

- SAX 面是 lxp parser 对象的原样透传(`:parse(str)` 逐段喂,空参 `:parse()` 收流,`:close()` 释放)——需要底层控制(如 CDATA/注释回调)时直接用它的完整回调集。
- DOM 不保留注释与处理指令;文本中的实体(`&amp;` 等)解码为字符,编码时按需转义。
