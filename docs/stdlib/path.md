# stdlib · path

路径处理。纯 Lua,API 与 Node 的 `path` 同款;当前平台 POSIX(`path.sep` = `/`,`path.delimiter` = `:`,另有 `path.posix` 同物)。

## API

| 调用 | 说明 |
| --- | --- |
| `path.normalize(s)` | 折叠 `.`/`..` 与重复分隔符 |
| `path.join(...)` | 拼接并 normalize |
| `path.resolve(...)` | 从右往左消解到绝对路径,缺位补 cwd |
| `path.relative(from, to)` | 从 from 到 to 的相对路径 |
| `path.dirname(s)` / `path.basename(s, ext?)` / `path.extname(s)` | 三件套 |
| `path.isAbsolute(s)` | 判绝对 |
| `path.parse(s)` / `path.format(o)` | 拆解/重组 `{root, dir, base, ext, name}` |
| `path.sep` / `path.delimiter` / `path.posix` | 常量与 POSIX 面 |

## 用法

```lua
local path = require "path"
print(path.join("docs", "guide", "cli-repl.md"))
print(path.normalize("docs//guide/../guide/./cli-repl.md"))
print(path.extname("archive.tar.gz"), path.basename("a/b/c.lua", ".lua"))
```

```text
docs/guide/cli-repl.md
docs/guide/cli-repl.md
.gz	c
```

```lua
local path = require "path"
print(path.resolve("docs", "..", "README.md"))
print(path.relative("/usr/local", "/usr/share/lua"))
print(path.isAbsolute("/etc/hosts"), path.isAbsolute("etc/hosts"))
```

```text
<cwd>/README.md
../share/lua
true	false
```

`parse`/`format` 互逆:

```lua
local path = require "path"
local p = path.parse("/home/cui/work/report.final.pdf")
print(p.dir, p.name, p.ext)
print(path.format({ dir = p.dir, base = "report.pdf" }))
```

```text
/home/cui/work	report.final	.pdf
/home/cui/work/report.pdf
```

## 契约与边界

- 与 Node 一致:`normalize` 不消解中间 `..`(保守,不碰软链接语义),`resolve` 才消;`resolve` 不保留尾斜杠(Node 与 `normalize` 在这点分叉)。
- Windows 路径(`C:\`)不在当前面内——需要时 `path.posix` 之外另有 Node `win32` 的对应物未提供。
