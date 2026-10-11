-- querystring: Node querystring 面 (v24.21.0 lib/querystring.js, 纯 Lua)。
--   parse/stringify (别称 decode/encode) + escape/unescape。
--   parse 把 '+' 收成空格 (urlencoded 约定, 经 plusChar), escape 不转义
--   !'()*-._~ 与数字字母; 重复键聚合为数组; maxKeys 默认 1000, <=0 视为
--   无限 (Node 同款怪癖: 0 不是 0 是无限); 键序确定性: 数字键升序在前,
--   其余字符串字节序 (Node 是插入序, 此为账记偏差); 空表 {} 视为空数组
--   (字段整体省略), 带非序列键的表视为对象 (值出 '')。

local M = {}

local function is_hex(b)
    return (b >= 48 and b <= 57) or (b >= 65 and b <= 70) or (b >= 97 and b <= 102)
end

-- escape 的不转义字节 (noEscape Int8Array 的净效果)
local no_escape = {}
for _, c in ipairs({33, 39, 40, 41, 42, 45, 46, 95, 126}) do
    no_escape[c] = true
end
for b = 48, 57 do no_escape[b] = true end
for b = 65, 90 do no_escape[b] = true end
for b = 97, 122 do no_escape[b] = true end

function M.escape(v)
    if type(v) ~= "string" then
        v = tostring(v) -- Node: 对象 String(v), 其余 v + ''
    end
    local out = {}
    for i = 1, #v do
        local b = v:byte(i)
        if no_escape[b] then
            out[#out + 1] = v:sub(i, i)
        else
            out[#out + 1] = ("%%%02X"):format(b)
        end
    end
    return table.concat(out)
end

-- 宽松百分号解码: 合法 %XX 解出字节, 残缺 % 原样保留 (Node 走
-- decodeURIComponent 失败后退 unescapeBuffer, 净效果同字节级宽松解码)
function M.unescape(s)
    local out = {}
    local i = 1
    while i <= #s do
        local b = s:byte(i)
        if b == 37 and i + 2 <= #s and is_hex(s:byte(i + 1)) and is_hex(s:byte(i + 2)) then
            out[#out + 1] = string.char(tonumber(s:sub(i + 1, i + 2), 16))
            i = i + 3
        else
            out[#out + 1] = s:sub(i, i)
            i = i + 1
        end
    end
    return table.concat(out)
end

-- 键/值是否含有效 %XX (Node encodeCheck 三连击的净效果)
local function has_pct(s)
    for i = 1, #s - 2 do
        if s:byte(i) == 37 and is_hex(s:byte(i + 1)) and is_hex(s:byte(i + 2)) then
            return true
        end
    end
    return false
end

local function add_key_val(obj, key, value)
    -- Node 的 keyEncoded/valEncoded 循环态跟踪净效果: 含有效 %XX 才解码
    if #key > 0 and has_pct(key) then
        key = M.unescape(key)
    end
    if #value > 0 and has_pct(value) then
        value = M.unescape(value)
    end
    local cur = obj[key]
    if cur == nil then
        obj[key] = value
    elseif type(cur) == "table" then
        cur[#cur + 1] = value
    else
        obj[key] = { cur, value }
    end
end

-- 键序确定性: 整数键升序在前, 字符串键字节序 (Node 是插入序)
local function sorted_keys(obj)
    local nums, strs, others = {}, {}, {}
    for k in pairs(obj) do
        if math.type(k) == "integer" then
            nums[#nums + 1] = k
        elseif type(k) == "string" then
            strs[#strs + 1] = k
        else
            others[#others + 1] = k
        end
    end
    table.sort(nums)
    table.sort(strs)
    table.sort(others, function(a, b) return tostring(a) < tostring(b) end)
    local out = {}
    for _, k in ipairs(nums) do out[#out + 1] = k end
    for _, k in ipairs(strs) do out[#out + 1] = k end
    for _, k in ipairs(others) do out[#out + 1] = k end
    return out
end

function M.parse(qs, sep, eq, options)
    local obj = {}
    if type(qs) ~= "string" or #qs == 0 then
        return obj
    end

    -- 分隔符转字节码组 (Node charCodes(String(sep)); nil/'' 走默认,
    -- 与 Node 的 `sep || '&'` 一致: 空串 falsy 也回落)
    local sep_codes
    if not sep or sep == "" then
        sep_codes = {38}
    else
        sep_codes = {}
        local s = tostring(sep)
        for i = 1, #s do sep_codes[#sep_codes + 1] = s:byte(i) end
    end
    local eq_codes
    if not eq or eq == "" then
        eq_codes = {61}
    else
        eq_codes = {}
        local s = tostring(eq)
        for i = 1, #s do eq_codes[#eq_codes + 1] = s:byte(i) end
    end
    local sep_len, eq_len = #sep_codes, #eq_codes

    local pairs_left = 1000
    if type(options) == "table" and type(options.maxKeys) == "number" then
        -- Node: maxKeys > 0 才生效, 否则 -1 = 无限 (0 也是无限)
        if options.maxKeys > 0 then
            pairs_left = options.maxKeys
        else
            pairs_left = -1
        end
    end

    local plus_char = " " -- 默认解码器: '+' 收成空格
    -- last_pos/sep_i/eq_i 一律 JS 0-based (与 Node 源码同坐标系);
    -- 切片统一 sub(last_pos + 1, e0) —— e0 为 0-based 排他端点。
    local last_pos, sep_i, eq_i = 0, 0, 0
    local key, value = "", ""
    local len = #qs
    local i = 0
    while i < len do
        local code = qs:byte(i + 1)
        i = i + 1 -- i 为当前字符的 1-based 位置 (= js_i + 1)
        if sep_i < sep_len and code == sep_codes[sep_i + 1] then
            sep_i = sep_i + 1
            if sep_i == sep_len then
                local e = i - sep_len
                local skip = false
                if eq_i < eq_len then
                    if last_pos < e then
                        key = key .. qs:sub(last_pos + 1, e)
                    elseif #key == 0 then
                        -- 分隔符之间的空串: 只计数不加键
                        pairs_left = pairs_left - 1
                        if pairs_left == 0 then return obj end
                        last_pos, sep_i, eq_i = i, 0, 0
                        skip = true
                    end
                elseif last_pos < e then
                    value = value .. qs:sub(last_pos + 1, e)
                end
                if not skip then
                    add_key_val(obj, key, value)
                    pairs_left = pairs_left - 1
                    if pairs_left == 0 then return obj end
                    key, value = "", ""
                    last_pos, sep_i, eq_i = i, 0, 0
                end
            end
        else
            sep_i = 0
            if eq_i < eq_len then
                if code == eq_codes[eq_i + 1] then
                    eq_i = eq_i + 1
                    if eq_i == eq_len then
                        local e = i - eq_len
                        if last_pos < e then
                            key = key .. qs:sub(last_pos + 1, e)
                        end
                        last_pos = i
                    end
                else
                    eq_i = 0
                    if code == 43 then
                        if last_pos < i - 1 then
                            key = key .. qs:sub(last_pos + 1, i - 1)
                        end
                        key = key .. plus_char
                        last_pos = i
                    end
                end
            elseif code == 43 then
                if last_pos < i - 1 then
                    value = value .. qs:sub(last_pos + 1, i - 1)
                end
                value = value .. plus_char
                last_pos = i
            end
        end
    end

    -- 收尾: 残段归键或值; 末尾恰好在空串则直接返回
    if last_pos < len then
        if eq_i < eq_len then
            key = key .. qs:sub(last_pos + 1)
        elseif sep_i < sep_len then
            value = value .. qs:sub(last_pos + 1)
        end
    elseif eq_i == 0 and #key == 0 then
        return obj
    end
    add_key_val(obj, key, value)
    return obj
end

-- JS ''+v 的数字串 (整值浮点出整数串, 非有限数不走此处)
local function js_numstr(v)
    if math.type(v) == "integer" then
        return tostring(v)
    end
    if v == 0 then
        return "0" -- -0 与 +0 同出 '0'
    end
    if v == math.floor(v) and math.abs(v) < 1e21 then
        return string.format("%.0f", v)
    end
    return tostring(v)
end

-- 表即数组约定: 空表或带 [1] 的表为数组, 其余为对象
local function is_array(v)
    return type(v) == "table" and (next(v) == nil or v[1] ~= nil)
end

function M.stringify(obj, sep, eq, options)
    if not sep or sep == "" then sep = "&" end
    if not eq or eq == "" then eq = "=" end
    local encode = M.escape
    local convert
    if type(options) == "table" and type(options.encodeURIComponent) == "function" then
        encode = options.encodeURIComponent
        -- 自定义编码器: 一律先转串再编码 (encodeStringifiedCustom)
        convert = function(v)
            if type(v) == "string" then return encode(v) end
            if type(v) == "number" then
                if v ~= v or v == math.huge or v == -math.huge then return "" end
                return encode(js_numstr(v))
            end
            if type(v) == "boolean" then return encode(v and "true" or "false") end
            return ""
        end
    else
        -- 默认编码器: 数字与布尔出裸串不编码 (encodeStringified)
        convert = function(v)
            if type(v) == "string" then
                if #v == 0 then return "" end
                return encode(v)
            end
            if type(v) == "number" then
                if v ~= v or v == math.huge or v == -math.huge then return "" end
                if math.abs(v) < 1e21 then return js_numstr(v) end
                return encode(tostring(v)) -- ≥1e21 JS 出科学计数带 '+', 走编码
            end
            if type(v) == "boolean" then return v and "true" or "false" end
            return ""
        end
    end

    if type(obj) ~= "table" then
        return ""
    end
    local fields = ""
    for _, k in ipairs(sorted_keys(obj)) do
        local v = obj[k]
        local ks = convert(k)
        ks = ks .. eq
        if is_array(v) then
            if #v == 0 then
                -- 空数组: 整个字段省略 (Node continue)
            else
                if fields ~= "" then fields = fields .. sep end
                for j = 1, #v do
                    if j > 1 then fields = fields .. sep end
                    fields = fields .. ks .. convert(v[j])
                end
            end
        else
            if fields ~= "" then fields = fields .. sep end
            fields = fields .. ks .. convert(v)
        end
    end
    return fields
end

M.decode = M.parse
M.encode = M.stringify

return M
