-- crypto: digests, HMAC, random bytes.
--
-- Backed by luaossl (William Ahern's comprehensive OpenSSL binding).
-- luna is built with crypto support only when OpenSSL development
-- headers are present; otherwise this module fails with guidance.
local ok, openssl = pcall(require, "openssl")
if not ok then
    error("crypto: luna was built without OpenSSL support — rebuild with "
        .. "OpenSSL development headers installed (libssl-dev) to enable it",
        2)
end

local crypto = setmetatable({}, { __index = openssl })

local function tohex(s)
    return (s:gsub(".", function(c)
        return string.format("%02x", c:byte())
    end))
end

-- crypto.sha256(data) -> lowercase hex digest (and sha1/sha384/sha512)
local function digest_hex(alg)
    return function(data)
        local d = openssl.digest.new(alg)
        d:update(data)
        return tohex(d:final())
    end
end

crypto.sha1 = digest_hex("sha1")
crypto.sha256 = digest_hex("sha256")
crypto.sha384 = digest_hex("sha384")
crypto.sha512 = digest_hex("sha512")

-- crypto.hmac(alg, key, data) -> lowercase hex
function crypto.hmac(alg, key, data)
    local h = openssl.hmac.new(key, alg)
    return tohex(h:final(data))
end

-- crypto.randombytes(n) -> n random bytes
crypto.randombytes = openssl.rand.bytes

return crypto
