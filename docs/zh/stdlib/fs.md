# stdlib · fs

文件系统。后端 [luafilesystem](https://lunarmage.co.uk/projects/luafilesystem/)(lfs):**lfs 的完整 API 原样可达**(`attributes`/`dir`/`mkdir` 等,经 `__index` 透传),上面再铺一层 Node 风格的同步便利。异步文件操作(读写/目录/静态)在 [loop](/guide/loop) 的 `loop.fs`,不在本页。

## API

| 调用 | 说明 |
| --- | --- |
| `fs.exists(path)` | 存在即真(文件或目录) |
| `fs.isFile(path)` / `fs.isDirectory(path)` | 类型判断 |
| `fs.readFileSync(path)` | 整读为字符串;打不开时 raise(消息带原因) |
| `fs.writeFileSync(path, data)` | 覆写;成功返回 `true`,打不开 raise |
| `fs.appendFileSync(path, data)` | 建有则追加 |
| `fs.readdirSync(dir)` | 目录条目名数组,**已排序**,不含 `.`/`..`(同 Node) |
| `fs.mkdirSync(path, recursive?)` | 建目录;`recursive` 时补齐缺失父级、已存在亦算成功(`mkdir -p` 语义) |
| `fs.attributes(path, what?)` | lfs 原样:`mode`/`size`/`mtime`… |
| `fs.dir(path)` | lfs 原样:条目迭代器(含 `.`/`..`) |
| (其余) | `lfs` 全部函数经 `__index` 透传 |

## 用法

```lua
local fs = require "fs"
fs.writeFileSync("/tmp/luna-fs-demo.txt", "hello ")
fs.appendFileSync("/tmp/luna-fs-demo.txt", "luna")
print(fs.readFileSync("/tmp/luna-fs-demo.txt"))
print(fs.exists("/tmp/luna-fs-demo.txt"), fs.isFile("/tmp/luna-fs-demo.txt"))
```

```text
hello luna
true	true
```

```lua
local fs = require "fs"
fs.mkdirSync("/tmp/luna-fs-demo/a/b", true)   -- 父级缺失一并建
print(fs.isDirectory("/tmp/luna-fs-demo/a/b"))
print(table.concat(fs.readdirSync("/tmp/luna-fs-demo/a"), ","))
```

```text
true
b
```

lfs 原样面照常可用。`attributes` 有两种形态:不给 `what` 返回整张属性表(`mode`/`size`/`mtime`/`permissions`…),给 `what` 直接返回单值:

```lua
local fs = require "fs"
local a = fs.attributes("/tmp/luna-fs-demo.txt")
print(a.mode, a.size > 0)
print(fs.attributes("/tmp/luna-fs-demo.txt", "mode"))
```

```text
file	true
file
```

路径不存在时 `attributes` 返回 `nil, err`(lfs 口径)——`fs.exists` 取的正是它的第一个返回值。`fs.dir` 迭代器含 `.`/`..`,而 `fs.readdirSync` 已替你滤掉并排序;要"读目录"就用后者,要"流式扫大目录"才用前者。

## 契约与边界

- 同步阻塞调用——脚本里顺手用没问题;事件循环里处理大文件请走 `loop.fs` 异步面,别堵住循环。
- 失败语义:便利层的"打不开/已存在"按 Node 风格 **raise**(消息含原因);lfs 透传部分保持 lfs 自身的返回值风格(`nil, err`)。
