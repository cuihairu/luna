# 标准库总览

全部随二进制一体分发,`require` 即用,不需要装任何东西。标准库按**四类**组织——Core、Data-Format、Dev tooling、Ecosystem:每个新模块入类前先回答"属于哪一类",不属于任何一类的功能不进标准库(防"什么都往里塞")。

## Core — 运行时与通用面

| 模块 | 一句话 | 后端 | 页面 |
| --- | --- | --- | --- |
| json | JSON 编解码(附 Node 式 `parse`/`stringify` 别名) | dkjson | [json](/stdlib/json) |
| fs | 文件系统:Node 式同步便利 + 完整 lfs 面 | luafilesystem | [fs](/stdlib/fs) |
| path | 路径处理,Node `path` 同款 API | 纯 Lua | [path](/stdlib/path) |
| util | `format`/`inspect`,Node v24 语义 | 纯 Lua | [stdlib/util](/stdlib/util) |
| events | EventEmitter:on/once/emit/错误契约 | 纯 Lua | [events](/stdlib/events) |
| stream | Readable/Writable/Transform/pipe/背压 | 纯 Lua | [stream](/stdlib/stream) |
| net | TCP/UDP 同步 socket 与一行起服 `serve()` | luasocket | [net](/stdlib/net) |
| http | 一行起服 `serve()` + luasocket 客户端面 | luasocket + 纯 Lua | [http](/stdlib/http) |
| crypto | sha1/256/384/512、HMAC、随机字节(需 OpenSSL) | luaossl | [crypto](/stdlib/crypto) |

## Data-Format — 数据格式编解码

| 模块 | 一句话 | 后端 | 页面 |
| --- | --- | --- | --- |
| csv | RFC 4180 CSV 编解码与逐行迭代 | LPeg | [csv](/stdlib/csv) |
| ini | 经典 .ini 编解码 | LPeg | [ini](/stdlib/ini) |
| toml | TOML v1.1 编解码,datetime 组件表 | tomlc17 (C) | [toml](/stdlib/toml) |
| yaml | YAML 1.1 编解码、多文档 | lyaml | [yaml](/stdlib/yaml) |
| xml | DOM 编解码 + SAX 透传 | expat (lxp) | [xml](/stdlib/xml) |
| zlib | compress/decompress 一发式 + 流式 deflate/inflate | lua-zlib | [zlib](/stdlib/zlib) |

## Dev tooling — 开发工具面

| 模块 | 一句话 | 后端 | 页面 |
| --- | --- | --- | --- |
| logging | 级别化日志,console/文件/滚动文件/socket appender(上游原样透出) | lualogging | [logging](/stdlib/logging) |

REPL 自带的面(`repl`/`introspect`/`magic`/`complete`/`highlight`,内嵌策略模块)同属这一类,通过 REPL 与[CLI 与 REPL](/guide/cli-repl) 使用,不占 require 名。

## Ecosystem — 生态接入面

不设独立 stdlib 页,入口在指南:[LuaRocks 包装](/guide/rocks)(`luna install` 等,包管理)、[目录插件](/guide/plugins)(扩展点接入)、[模块系统](/guide/modules)(`luna_modules/` 解析)。这一类的职责是"把外面的代码接进来",不是"再造外面的东西"——LuaRocks 只包装不自建 registry 即此口径。

异步面(TCP/TLS/异步 fs/子进程/定时器,`require "loop"` 显式进入)单列在[事件循环](/guide/loop),不并入四类——它是 opt-in 的第二世界,见架构页 rationale。

两个贯穿契约:

- **数据错误不抛,参数错误才抛**:格式类模块(json/csv/ini/toml/xml/yaml)的 `decode`/`encode` 遇到坏数据返回 `nil, err`(消息带行列);传错参数类型(比如 `decode(42)`)直接 raise。循环/脚本里喂不可信数据建议 `pcall` 或显式接第二个返回值。
- **字符串按字节计**:所有模块对 Lua 字符串不做编码转换;UTF-8 只是"透传并尽量在错误定位里数对"。

## 随二进制分发的底层模块

上表之外,二进制里还注册着一层**上游原样透出**的 C 模块与随发 Lua 库——各包装层的核心(zlib 流式、TOML 解码、expat SAX、luaossl 全家)就踩在它们上面;需要越过包装层时可直接 `require`:

| 模块 | 一句话 | 上游 |
| --- | --- | --- |
| `lpeg` | LPeg 模式匹配;高亮、csv/ini 词法的底座 | [LPeg](http://www.inf.puc-rio.br/~roberto/lpeg/) |
| `socket.core` | luasocket 的 TCP/UDP 原始面 | [luasocket](https://github.com/diegonehab/luasocket) |
| `socket.unix` | unix 域 socket;attach 通道的传输层 | 同上 |
| `mime.core` | MIME 编解码(base64、quoted-printable) | 同上 |
| `zlib.core` | lua-zlib 原始流式面;`zlib` 包装层的一次式 API 在其上 | [lua-zlib](https://github.com/brimworks/lua-zlib) |
| `toml.core` | tomlc17 解码核心(无编码,encode 是包装层的 Lua 面) | [tomlc17](https://github.com/cktan/tomlc17) |
| `yaml.core` | lyaml 的 C 面(仅解码) | [lyaml](https://github.com/gvvaughan/lyaml) |
| `lxp` | expat 的 SAX 面;`xml` 的 DOM 构建在其上 | [lua-expat](https://github.com/tomasguisasola/luaexpat) |
| `_openssl.*` | luaossl 全家(约 20 个子模块,`crypto` 包装层之下) | [luaossl](https://github.com/wahern/luaossl) |
| `lfs` | luafilesystem 完整面(`fs` 的透传半边) | [luafilesystem](https://github.com/keplerproject/luafilesystem) |
| `argparse` | CLI 参数解析(入口自己用,脚本同样可 require) | [argparse](https://github.com/luarocks/argparse) |
| `linedit` | C 行编辑器;一般经 REPL 使用,自建交互面时可直取 | luna 内置 |

再往上是 `luna.*` 前缀的**内嵌策略模块**(`luna.magic`/`luna.complete`/`luna.introspect`/`luna.highlight`/`luna.plugins`/`luna.serve`/`luna.modules`/`luna.rocks` 等)——REPL 的 Lua 半身,见[架构设计](/architecture)的三层模型一节。上游原样面意味着:错误风格与 API 形状随上游(`nil, err` 为主),luna 不做再包装。
