# 标准库总览

全部随二进制一体分发,`require` 即用,不需要装任何东西。绑定成熟 C 库或 LPeg 语法的模块负责格式与底层;纯 Lua 模块按 Node 语义补齐日常面。

| 模块 | 一句话 | 后端 | 页面 |
| --- | --- | --- | --- |
| json | JSON 编解码(附 Node 式 `parse`/`stringify` 别名) | dkjson | [json](/stdlib/json) |
| fs | 文件系统:Node 式同步便利 + 完整 lfs 面 | luafilesystem | [fs](/stdlib/fs) |
| path | 路径处理,Node `path` 同款 API | 纯 Lua | [path](/stdlib/path) |
| util | `format`/`inspect`,Node v24 语义 | 纯 Lua | [stdlib/util](/stdlib/util) |
| events | EventEmitter:on/once/emit/错误契约 | 纯 Lua | [events](/stdlib/events) |
| stream | Readable/Writable/Transform/pipe/背压 | 纯 Lua | [stream](/stdlib/stream) |
| net | TCP/UDP 同步 socket | luasocket | [net](/stdlib/net) |
| http | 一行起服 `serve()` + luasocket 客户端面 | luasocket + 纯 Lua | [http](/stdlib/http) |
| crypto | sha1/256/384/512、HMAC、随机字节(需 OpenSSL) | luaossl | [crypto](/stdlib/crypto) |
| zlib | compress/decompress 一发式 + 流式 deflate/inflate | lua-zlib | [zlib](/stdlib/zlib) |
| csv | RFC 4180 CSV 编解码与逐行迭代 | LPeg | [csv](/stdlib/csv) |
| ini | 经典 .ini 编解码 | LPeg | [ini](/stdlib/ini) |
| toml | TOML 1.0 编解码,datetime 组件表 | tomlc17 (C) | [toml](/stdlib/toml) |
| yaml | YAML 1.1 编解码、多文档 | lyaml | [yaml](/stdlib/yaml) |
| xml | DOM 编解码 + SAX 透传 | expat (lxp) | [xml](/stdlib/xml) |

两个贯穿契约:

- **数据错误不抛,参数错误才抛**:格式类模块(json/csv/ini/toml/xml/yaml)的 `decode`/`encode` 遇到坏数据返回 `nil, err`(消息带行列);传错参数类型(比如 `decode(42)`)直接 raise。循环/脚本里喂不可信数据建议 `pcall` 或显式接第二个返回值。
- **字符串按字节计**:所有模块对 Lua 字符串不做编码转换;UTF-8 只是"透传并尽量在错误定位里数对"。

事件循环一侧的异步面(TCP/TLS/异步 fs/子进程/定时器)在 [loop](/guide/loop) 与 `loop.http`,不在本区。
