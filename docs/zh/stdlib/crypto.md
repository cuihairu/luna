# stdlib · crypto

摘要、HMAC、随机字节。后端 [luaossl](https://github.com/wahern/luaossl)(William Ahern 的完整 OpenSSL 绑定)。**仅当构建时有 OpenSSL 开发头文件**(`libssl-dev`,macOS `brew install openssl@3`)才编入;缺失时 `require("crypto")` 给出指引性错误(构建照常成功)。

## API

| 调用 | 说明 |
| --- | --- |
| `crypto.sha1(data)` / `sha256` / `sha384` / `sha512` | 摘要,返回**小写十六进制**串 |
| `crypto.hmac(alg, key, data)` | HMAC,同为小写十六进制;`alg` 如 `"sha256"` |
| `crypto.randombytes(n)` | 密码学安全随机字节,n 长字符串 |

## 用法

```lua
local crypto = require "crypto"
print(crypto.sha256("luna"))
```

```text
970ec274ca867815174ebe4eff19282000f9495a6c7254e94991d1fb4dc3df30
```

```lua
local crypto = require "crypto"
local mac = crypto.hmac("sha256", "secret-key", "payload")
print(#mac, mac:sub(1, 12) ~= crypto.hmac("sha256", "wrong-key", "payload"):sub(1, 12))
```

```text
64	true
```

随机字节(十六进制化前是原始字节串):

```lua
local crypto = require "crypto"
local a, b = crypto.randombytes(16), crypto.randombytes(16)
print(#a, a ~= b)
```

```text
16	true
```

## 契约与边界

- 需要原始字节而非十六进制时自己 `:byte()` 拆——便利层只出 hex,命名不撒谎。
- 构建 OpenSSL 缺席时,本页 API 全部不可用;`require` 的报错直接给出补构建的指引。
