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
| `net` | luasocket | `tcp`/`udp`/`connect`/`bind`/`select`/`dns` |
| `http` | luasocket | `request`、`get` 等完整 http.client 面 |
| `zlib` | lua-zlib | 流式 `deflate`/`inflate` + 一次性 `compress`/`decompress` 便捷层 |
| `crypto` | luaossl | `sha256` 系列、`hmac`、`rand.bytes`(需构建期 OpenSSL) |

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
