# stdlib · zlib

压缩。后端 [lua-zlib](https://github.com/brimworks/lua-zlib)(绑系统 zlib;流式 inflate/deflate,gzip 支持)。C 核注册为 `zlib.core`,流式闭包经 `__index` 原样可达,luna 在上面加一发式便利。

## API

| 调用 | 说明 |
| --- | --- |
| `zlib.compress(data, level?)` | 一发式 deflate;`level` 0-9(zlib 口径),缺省 6 |
| `zlib.decompress(data)` | 一发式 inflate |
| `zlib.deflate([level], [windowSize])` | lua-zlib 原样:返回流式压缩闭包。`windowSize` 缺省 `15`(zlib 格式);gzip 传 `15+16`,raw deflate 传 `-15` |
| `zlib.inflate([windowSize])` | lua-zlib 原样:返回流式解压闭包。不传 `windowSize` 即自动探测 |
| (其余) | lua-zlib C 核心函数经 `__index` 透传:`deflate`/`inflate`/`adler32`/`crc32`/`version` |

## 用法

```lua
local zlib = require "zlib"
local blob = ("luna"):rep(1000)
local packed = zlib.compress(blob)
print(#blob, #packed, #packed < #blob / 10)
```

```text
4000	32	true
```

解压还原:

```lua
local zlib = require "zlib"
print(zlib.decompress(zlib.compress("hello luna")))
```

```text
hello luna
```

流式面(数据分片、增量产出)走 lua-zlib 原样闭包:

```lua
local zlib = require "zlib"
local deflate = zlib.deflate()
local part1 = deflate("chunk-one ", "sync")
local part2 = deflate("chunk-two", "finish")
print(#zlib.inflate()(part1 .. part2, "finish"))
```

```text
19
```

## 契约与边界

- `compress`/`decompress` 是一次性进出——大文件请用流式闭包分片,别整块读内存。
- 一发式便利产出的是 **zlib 格式流**(2 字节头 + adler32 尾,不是 gzip)。窗口位在**创建闭包时**定死,不在调用时:`zlib.deflate(level, windowSize)`,闭包只吃 `(data, flush)`。要 gzip 就 `zlib.deflate(level, 15+16)`——传 `31`,别在闭包调用上补第三个参数(raw deflate 是 `-15`)。
