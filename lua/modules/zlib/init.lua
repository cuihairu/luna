-- zlib: compression.
--
-- Backed by lua-zlib (streaming inflate/deflate, gzip support, bound
-- to the system zlib). The C core registers as "zlib.core"; the
-- streaming closures are re-exported as-is, and one-shot
-- compress/decompress conveniences are added on top.
local zlib = require "zlib.core"

local z = setmetatable({}, { __index = zlib })

-- z.compress(data[, level]) -> compressed string (one-shot deflate)
function z.compress(data, level)
    return zlib.deflate(level)(data, "finish")
end

-- z.decompress(data) -> original string (one-shot inflate)
function z.decompress(data)
    local inflate = zlib.inflate()
    local out = inflate(data)
    local more = inflate()
    if more and #more > 0 then
        return out .. more
    end
    return out
end

return z
