# stdlib · crypto

Digests, HMAC, random bytes. Backed by [luaossl](https://github.com/wahern/luaossl) (William Ahern's comprehensive OpenSSL binding). Compiled **only when OpenSSL development headers are present** (`libssl-dev`, or on macOS `brew install openssl@3`); when absent, `require("crypto")` gives an instructive error (the build still succeeds).

## API

| Call | Description |
| --- | --- |
| `crypto.sha1(data)` / `sha256` / `sha384` / `sha512` | Digest, returns a **lowercase hexadecimal** string |
| `crypto.hmac(alg, key, data)` | HMAC, also lowercase hex; `alg` such as `"sha256"` |
| `crypto.randombytes(n)` | Cryptographically secure random bytes, an n-length string |

## Usage

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

Random bytes (raw byte string before hexadecimalizing):

```lua
local crypto = require "crypto"
local a, b = crypto.randombytes(16), crypto.randombytes(16)
print(#a, a ~= b)
```

```text
16	true
```

## Boundary rules

- When you need raw bytes rather than hex, split with `:byte()` yourself — the convenience layer emits only hex, and its name does not lie.
- When OpenSSL is missing from the build, none of this page's APIs are available; the `require` error directly explains how to rebuild.

