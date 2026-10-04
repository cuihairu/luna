# 模块系统

luna 在官方 `require` 之上安装了**第二搜索器**(紧跟 `package.preload` 之后),实现 Node.js 风格的项目解析。原有搜索器语义不变,`package.loaded` 缓存照常。

## 解析规则

`require("名字")` 依次经过:

1. `package.preload`(内建策略层与插件注入都在这里命中);
2. **luna 搜索器**:
   - 相对名(`./x`、`../x`):相对**当前 chunk 所在目录**解析——`require("./util")` 拿到同目录的 `util.lua` 或 `util/init.lua`;
   - 裸名(`hello.sub`):从当前目录起**逐级上溯**,每一层找 `luna_modules/hello/` 与 `luna_modules/hello.lua`,直到文件系统根——与 node_modules 的查找方式一致;
   - **包内子路径**(`dep.sub` / `dep/sub`):先照整名找包,找不到再从**最长前缀**起把余段当包内文件解析——`luna_modules/dep/sub.lua`,退而 `luna_modules/dep/sub/init.lua`;子路径**不走**包的 `main`(与 Node 的 `node_modules/dep/sub` 一致),每一层都整名优先、子路径回退;
3. Lua 官方路径搜索器与 C 搜索器(`package.path` / `package.cpath`)。

## 包清单

一个目录形如:

```
luna_modules/hello/
├── package.json     {"name":"hello","version":"0.1.0","main":"init.lua"}
├── init.lua         ← 无 main 时的约定入口
└── lib/util.lua
```

- `main` 指向文件名(`"lib/main"` → `lib/main.lua`)或包目录(`"lib"` → `lib/init.lua`);
- 没有 `main` 时用 `init.lua` 约定;
- 清单经 dkjson 解析;结果照常进入 `package.loaded`,二次 `require` 直接命中缓存。

## 内置模块

全部随二进制一体分发,C 后端在内核注册,纯 Lua 薄层在 `luna_modules/`:

| 模块 | 后端 | 内容 |
| --- | --- | --- |
| `json` | dkjson | `encode`/`decode` 与 `stringify`/`parse` 别名 |
| `fs` | luafilesystem | lfs 全量 + `exists`/`isFile`/`isDirectory`/`readFileSync`/`writeFileSync`/`appendFileSync`/`readdirSync`/`mkdirSync` 便捷层 |
| `net` | luasocket | `tcp`/`udp`/`connect`/`bind`/`serve`(一行阻塞 TCP 服务)/`select`/`dns` |
| `http` | luasocket | `request`、`get` 等完整 http.client 面 |
| `csv` | LPeg | RFC 4180 式 `decode`/`encode`(引号内分隔符/换行/双写引号、`headers` 表键行、`delimiter`)与逐记录 `lines()` 迭代器 |
| `ini` | LPeg | 经典 `decode`/`encode`(段/段前裸键/两种注释/引号值转义/重复键/`cast` 数字与布尔) |
| `toml` | tomlc17 | TOML v1.1 `decode`/`encode`;`decode` 走 C 后端,`encode` 是包装层自有实现(标量键先行、`[[表数组]]`、内联数组/日期时间/空表) |
| `yaml` | libyaml + lyaml | YAML 1.1 `decode`/`decodeAll`/`encode`;null → nil 默认(`opts.nullval = yaml.null` 保留哨兵),anchors/aliases 解为共享表引用,`---` 分隔的多文档走 `decodeAll` |
| `xml` | expat + lua-expat | DOM 三件套 `decode`/`encode`(元素 = `{tag, attrs, kids}`,文本节点是 kids 里的字符串,属性恒字符串,命名空间前缀原样)+ `xml.sax` 流式透传(lxp handler 模型) |
| `path` | 纯 Lua | Node `path.posix` 语义:`join`/`resolve`/`normalize`/`relative`/`dirname`/`basename`/`extname`/`isAbsolute`/`parse`/`format` + `sep`/`delimiter`(按 Node v24 实证钉版,win32 面不做) |
| `util` | 纯 Lua | `util.inspect`(depth/循环 `[Circular *N]`/截断/键引号,Lua 语法形态)与 `util.format`(`%s %d %i %f %o %j %%` + 扩展 `%x %X`;无符连接、超参尾接,Node v24 口径) |
| `events` | 纯 Lua | `events.new()` EventEmitter:`on`/`once`/`off`/`prepend*`/`listeners`/`listenerCount`/`setMaxListeners`/`emit`(缺省 10 超限警告、`'error'` 无监听 raise、newListener/removeListener 内建事件) |
| `stream` | 纯 Lua | 基于 events 的 Node 流 v1 面:`readable`/`writable`/`duplex`/`transform`(`_read`→`push`、`_write`/`_transform`+`_flush`、`write`/`end_`)、`pipe` 背压(write false 停推等 `drain` 续推、`unpipe`)、`highWaterMark` 记账(缺省 16 KiB);适配器 `duplexFromSock`(loop.net sock)、`readableFromChunks`(内存块)、`pushReadable`(onData 推式) |
| `zlib` | lua-zlib | 流式 `deflate`/`inflate` + 一次性 `compress`/`decompress` 便捷层 |
| `crypto` | luaossl | `sha256` 系列、`hmac`、`rand.bytes`(需构建期 OpenSSL) |

**格式模块的错误口径**(csv/ini/toml/yaml/xml 与 json 一致,详见[Node 方向选型](/node-parity)):`decode`/`encode` 对坏数据**返回 `nil, err` 而不抛错**,错误消息带行列(`<fmt>: <原因> at line N, column M`,列按 UTF-8 字符计);参数类型错才 raise。toml 是例外中的例外:tomlc17 只报行号,消息是 `<原因> at line N`,没有列子句(数字实参照 `string.format` 的 `%s` 先转字符串,再按坏数据走 `nil, err`)。yaml 的行列也有分叉:坐标是**最后一个成功解析事件的起点**(lyaml 丢弃了 libyaml 自己的 problem_mark),常常不在出错那一行——实现语义如此,已用例钉住。`csv.lines` 是迭代器(nil 表示读完),坏输入只能 raise,喂不可信数据时自行 `pcall`。

**path/util/events/stream 是 Node 语义模块**(非格式模块,不属上面的 `nil, err` 口径):没有"坏数据"可言,只有参数类型错——一律 raise;行为按 **Node v24 机器实证**逐函数钉版,与 Node 的有意分叉(`util.format` 的 `%x`/`%X` 扩展、inspect 的 Lua 语法形态与确定性键序、`pipe` 的错误联动等)在 [Node 方向选型](/node-parity) 逐条勘定。stream 的 `end` 因关键字冲突拼作 `s:end_(...)`(`s["end"]` 同效,sock 同款先例)。

```lua
local fs = require("fs")
local text = fs.readFileSync("notes.txt")
local packed = require("zlib").compress(text)
```

## 与官方语义的关系

- 搜索器插在 preload 之后,意味着**插件注入永远优先于项目模块**——插件可当垫片(shim);
- 相对名的锚点是 chunk 文件路径(`debug.getinfo` 的 `@` 源),交互控制台里相对名锚定当前工作目录;
- 循环 require 没有官方 Lua 那样的递归保护(loaded 标记发生在 loader 返回之后),模块要求自身属约定禁用——与官方 Lua 相同;
- Lua 5.5 的 `require` 在**新加载**时返回两个值(模块本体 + 搜索器的 loader 数据),命中 `package.loaded` 缓存时只返回一个;`return require("x")` 会把两个值一起透传,只要模块本体时写 `return (require("x"))` 截断。
