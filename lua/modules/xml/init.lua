--- @module xml
-- xml: DOM 三件套 (decode/encode) + SAX 透传, 后端 expat (经 lua-expat
-- 的 lxp 绑定)。选型与契约详见 docs/node-parity.md。
--
-- 共同口径 (与 json/yaml/toml/csv/ini 对齐):
--   decode/encode 失败返回 nil, err (err 为字符串, 文本格式错误带行列);
--   参数类型错才 raise。
-- DOM 形状: 元素 = { tag, attrs, kids }, 文本节点是 kids 里的普通字符串;
-- 属性值恒为字符串; 命名空间前缀原样保留 (<x:a> 的 tag 是 "x:a")。

local lxp = require "lxp"

local function escape_text(s)
    return (s:gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;"))
end

local function escape_attr(s)
    return (s:gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;")
             :gsub('"', "&quot;"))
end

-- lxp 的 parse 失败返回 nil, errmsg, line, col, byteindex; 规整成
-- "xml: <原因> at line N, column M" (行列来自 expat, 列 1 基)。
local function fmt_err(err, line, col)
    if type(line) == "number" and type(col) == "number" then
        return string.format("xml: %s at line %d, column %d", err, line, col)
    end
    return "xml: " .. tostring(err)
end

-- decode: 文本 → DOM 表。内部用 lxp 的 SAX 回调攒 DOM; 相邻文本片段
-- (实体展开/CDATA 边界会分片送达) 在元素边界合并成单个字符串。
local function decode(s)
    if type(s) ~= "string" then
        error("xml: decode expects a string", 2)
    end
    local root, top, pending
    local stack = {}
    local function flush()
        if pending and top then
            table.insert(top.kids, pending)
        end
        pending = nil
    end
    local p = lxp.new {
        -- lxp 的 handler 调用约定带 self 作首参 (docall 推 self)。
        StartElement = function(_, name, attrs)
            flush()
            local el = { tag = name, attrs = {}, kids = {} }
            -- lxp 的 attrs 表带数字键 (文档序) 与字符串键 (名→值);
            -- DOM 只取字符串键, 数字键是 lxp 的实现细节。
            for k, v in pairs(attrs) do
                if type(k) == "string" then
                    el.attrs[k] = v
                end
            end
            if top then
                table.insert(top.kids, el)
            else
                root = el
            end
            table.insert(stack, el)
            top = el
        end,
        EndElement = function(_, name)
            flush()
            table.remove(stack)
            top = stack[#stack]
        end,
        CharacterData = function(_, text)
            if top then
                pending = (pending or "") .. text
            end
        end,
    }
    -- lxp 的 parse(s) 不终结流 (final=0), 空参 parse() 才收尾 ——
    -- 未闭合标签的错误在收尾时才浮现。
    local ok, err, line, col = p:parse(s)
    if ok then
        ok, err, line, col = p:parse()
    end
    -- 收尾失败的 parser 上 close 会再抛一次错, decode 对坏输入不 raise。
    pcall(function() p:close() end)
    if not ok then
        return nil, fmt_err(err, line, col)
    end
    return root
end

-- encode: DOM 表 → 文本。属性值强制 tostring; 文本节点转义
-- & < >, 属性值转义 & < > "。opts.indent = N 给美化输出 (N 空格缩进)。
-- 环引用 (元素是自身的祖先) → nil, "xml: cyclic table reference"。
local function encode_el(el, indent, depth, ancestors)
    -- level 0: 这些是"坏数据"错误, 经 pcall → nil, err, 消息不带
    -- file:line 前缀 (与 decode 的错误口径一致)
    if ancestors[el] then
        error("xml: cyclic table reference", 0)
    end
    ancestors[el] = true
    local tag = el.tag
    if type(tag) ~= "string" then
        error("xml: element tag must be a string", 0)
    end
    local attrs = ""
    local attrsel = el.attrs
    if attrsel ~= nil then
        if type(attrsel) ~= "table" then
            error("xml: attrs must be a table", 0)
        end
        for k, v in pairs(attrsel) do
            attrs = attrs .. " " .. tostring(k) .. '="'
                .. escape_attr(tostring(v)) .. '"'
        end
    end
    local kids = el.kids
    if kids == nil then
        kids = {}
    elseif type(kids) ~= "table" then
        error("xml: kids must be a table", 0)
    end
    local n = #kids
    local has_el_kids = false
    for i = 1, n do
        if type(kids[i]) == "table" then
            has_el_kids = true
            break
        end
    end
    local out
    if not indent then
        -- 紧凑: 一切内联
        local parts = { "<" .. tag .. attrs .. ">" }
        for i = 1, n do
            local kid = kids[i]
            if type(kid) == "table" then
                parts[#parts + 1] = encode_el(kid, nil, depth + 1, ancestors)
            else
                parts[#parts + 1] = escape_text(tostring(kid))
            end
        end
        parts[#parts + 1] = "</" .. tag .. ">"
        out = table.concat(parts)
    elseif n == 0 then
        out = string.rep(" ", indent * (depth - 1)) .. "<" .. tag .. attrs .. "/>"
    elseif not has_el_kids then
        -- 纯文本子节点: 内联 (美化模式也不折行)
        local text = ""
        for i = 1, n do
            text = text .. escape_text(tostring(kids[i]))
        end
        out = string.rep(" ", indent * (depth - 1)) .. "<" .. tag .. attrs .. ">"
            .. text .. "</" .. tag .. ">"
    else
        -- 含元素子节点: 每个子节点一行
        local pad = string.rep(" ", indent * (depth - 1))
        local parts = { pad .. "<" .. tag .. attrs .. ">" }
        for i = 1, n do
            local kid = kids[i]
            if type(kid) == "table" then
                parts[#parts + 1] = encode_el(kid, indent, depth + 1, ancestors)
            else
                parts[#parts + 1] = string.rep(" ", indent * depth)
                    .. escape_text(tostring(kid))
            end
        end
        parts[#parts + 1] = pad .. "</" .. tag .. ">"
        out = table.concat(parts, "\n")
    end
    ancestors[el] = nil
    return out
end

local function encode(v, opts)
    if type(v) ~= "table" then
        error("xml: encode expects a DOM table", 2)
    end
    opts = opts or {}
    if type(opts) ~= "table" then
        error("xml: encode opts must be a table", 2)
    end
    local indent = opts.indent
    if indent ~= nil then
        if type(indent) ~= "number" or indent < 0 then
            error("xml: encode indent must be a non-negative number", 2)
        end
        indent = math.floor(indent)
    end
    local ok, res = pcall(encode_el, v, indent, 1, {})
    if not ok then
        return nil, tostring(res)
    end
    return res
end

return {
    decode = decode,
    encode = encode,
    -- SAX 透传: handler 表就是 lxp 的形状 (StartElement/EndElement/
    -- CharacterData/...), 不做第二套抽象; decode 内部就是用它攒 DOM。
    sax = lxp.new,
}
