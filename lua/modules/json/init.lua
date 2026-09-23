-- json: JSON encode/decode.
--
-- Backed by dkjson (David Kolf's pure-Lua JSON, UTF-8 aware, no
-- dependencies). Node-style aliases (parse/stringify) are provided so
-- either ecosystem's naming works.
local dkjson = require "dkjson"

return {
    decode = dkjson.decode,
    encode = dkjson.encode,
    parse = dkjson.decode,     -- Node-style alias
    stringify = dkjson.encode, -- Node-style alias
}
