--- @module util
-- util: Node util 的 inspect/format 面 (promisify/types 无 Lua 契约锚,
-- 明确不做)。语义按 Node v24.21.0 实证钉版, 详见 docs/node-parity.md:
--   format: %s %d %i %f %j %o %O %c 与 %%; %d/%f 不截断 (v24 实测),
--   %i 向零截断; 无参单串原样返回; 未知转换符原样保留; 超参空格尾接。
--   扩展 (todo 契约要求, Node v24 util.format 无此对): %x/%X 十六进制。
--   inspect: 输出为 Lua 源形状 ({} 而非 JS []), 键排序保确定性
--   (pairs 序不定, 与 Node 插入序无法对齐, 记为适配)。

local M = {}

-- JS String(number) 拼写: NaN/Infinity 按原词, 整值浮点不带 .0
-- (Lua tostring(3.0) 是 "3.0", JS String(3) 是 "3")。
local function numstr(v)
    if v ~= v then
        return "NaN"
    end
    if v == math.huge then
        return "Infinity"
    end
    if v == -math.huge then
        return "-Infinity"
    end
    if math.type(v) == "float" and v == math.floor(v) and math.abs(v) < 2 ^ 53 then
        return string.format("%.0f", v)
    end
    return tostring(v)
end

local KEYWORDS = {}
for _, w in ipairs({
    "and", "break", "do", "else", "elseif", "end", "false", "for",
    "function", "goto", "if", "in", "local", "nil", "not", "or",
    "repeat", "return", "then", "true", "until", "while",
}) do
    KEYWORDS[w] = true
end

local function quote_string(s)
    local out = s:gsub('[%z\1-\31"\\\127]', function(c)
        if c == "\\" then
            return "\\\\"
        elseif c == '"' then
            return '\\"'
        elseif c == "\n" then
            return "\\n"
        elseif c == "\t" then
            return "\\t"
        elseif c == "\r" then
            return "\\r"
        end
        return ("\\%03d"):format(c:byte())
    end)
    return '"' .. out .. '"'
end

-- 键的确定性排序: 数字升序在前, 字符串字节序次之, 其余按 tostring
local function sort_keys(keys)
    local function rank(k)
        local t = type(k)
        if t == "number" then
            return 0
        end
        if t == "string" then
            return 1
        end
        return 2
    end
    table.sort(keys, function(a, b)
        local ra, rb = rank(a), rank(b)
        if ra ~= rb then
            return ra < rb
        end
        if ra == 2 then
            return tostring(a) < tostring(b)
        end
        return a < b
    end)
end

local function key_repr(k)
    local t = type(k)
    if t == "string" then
        if k:match("^[A-Za-z_][A-Za-z0-9_]*$") and not KEYWORDS[k] then
            return k
        end
        return "[" .. quote_string(k) .. "]"
    end
    if t == "number" then
        return "[" .. numstr(k) .. "]"
    end
    return "[" .. tostring(k) .. "]"
end

-- 函数的显示: Lua 函数取 debug.info 的短源与定义行, C 函数走 tostring
local function fn_repr(v)
    local ok, info = pcall(debug.getinfo, v, "S")
    if ok and info and info.what == "Lua" and info.short_src then
        return ("<function %s:%d>"):format(info.short_src, info.linedefined)
    end
    return tostring(v)
end

local inspect_value -- 前向声明 (递归)

local function inspect_table(t, o, depth, ctx)
    local parts = {}
    local n = #t
    local shown = n
    if n > o.maxArrayLength then
        shown = o.maxArrayLength
    end
    for i = 1, shown do
        parts[#parts + 1] = inspect_value(t[i], o, depth + 1, ctx)
    end
    if shown < n then
        parts[#parts + 1] = ("... %d more items"):format(n - shown)
    end
    local keys = {}
    for k in pairs(t) do
        if not (type(k) == "number" and k >= 1 and k <= n and k % 1 == 0) then
            keys[#keys + 1] = k
        end
    end
    sort_keys(keys)
    for _, k in ipairs(keys) do
        parts[#parts + 1] = key_repr(k) .. " = " .. inspect_value(t[k], o, depth + 1, ctx)
    end
    if #parts == 0 then
        return "{}"
    end
    return "{ " .. table.concat(parts, ", ") .. " }"
end

inspect_value = function(v, o, depth, ctx)
    local t = type(v)
    if t == "string" then
        if #v > o.maxStringLength then
            return quote_string(v:sub(1, o.maxStringLength))
                .. ("... %d more characters"):format(#v - o.maxStringLength)
        end
        return quote_string(v)
    elseif t == "number" then
        return numstr(v)
    elseif t == "boolean" then
        return tostring(v)
    elseif t == "nil" then
        return "nil"
    elseif t == "function" then
        return fn_repr(v)
    elseif t == "thread" or t == "userdata" then
        return tostring(v)
    end
    -- table: 深度塌缩先于环检测 (Node: 深度之外的环显示 [Object])
    if depth > o.depth then
        return #v > 0 and "[Array]" or "[Object]"
    end
    local id = ctx.ids[v]
    if not id then
        ctx.next = ctx.next + 1
        id = ctx.next
        ctx.ids[v] = id
    end
    if ctx.stack[v] then
        return ("[Circular *%d]"):format(id)
    end
    ctx.stack[v] = true
    local out = inspect_table(v, o, depth, ctx)
    ctx.stack[v] = nil
    return out
end

-- inspect(v [, opts]): depth (默认 2, math.huge 全展开, 负数顶层即塌缩)、
-- maxArrayLength (默认 100)、maxStringLength (默认 10000), 负值按 0。
function M.inspect(v, opts)
    opts = opts or {}
    if type(opts) ~= "table" then
        error("util: inspect opts must be a table", 2)
    end
    local function opt_num(name, default, clamp_zero)
        local x = opts[name]
        if x == nil then
            return default
        end
        if type(x) ~= "number" then
            error(("util: inspect opts.%s must be a number"):format(name), 2)
        end
        if clamp_zero and x < 0 then
            return 0
        end
        return x -- depth 允许负数 (全塌缩, Node 同口径)
    end
    local o = {
        depth = opt_num("depth", 2, false),
        maxArrayLength = opt_num("maxArrayLength", 100, true),
        maxStringLength = opt_num("maxStringLength", 10000, true),
    }
    return inspect_value(v, o, 0, { ids = {}, stack = {}, next = 0 })
end

-- %s / 超参尾接的显示: 字符串裸、数字/布尔直拼、其余走 inspect
local function display(v)
    local t = type(v)
    if t == "string" then
        return v
    end
    if t == "number" then
        return numstr(v)
    end
    if t == "boolean" then
        return tostring(v)
    end
    return M.inspect(v)
end

-- JS Number() 收窄: 数字直通、数字串转换、其余 NaN (布尔 1/0)
local function to_number(v)
    if type(v) == "number" then
        return v
    end
    if type(v) == "boolean" then
        return v and 1 or 0
    end
    return tonumber(v)
end

local function trunc_zero(v)
    if v >= 0 then
        return math.floor(v)
    end
    return math.ceil(v)
end

local function convert(spec, v)
    if spec == "s" then
        return display(v)
    end
    if spec == "d" or spec == "f" then
        local n = to_number(v)
        if n == nil or n ~= n then
            return "NaN"
        end
        return numstr(n)
    end
    if spec == "i" then
        local n = to_number(v)
        if n == nil or n ~= n then
            return "NaN"
        end
        return numstr(trunc_zero(n))
    end
    if spec == "x" or spec == "X" then
        -- 扩展 (Node v24 util.format 无 %x/%X; todo 契约要求)
        local n = to_number(v)
        if n == nil or n ~= n then
            return "NaN"
        end
        if n == math.huge or n == -math.huge then
            return numstr(n)
        end
        local i = trunc_zero(n)
        local hex = spec == "x" and "%x" or "%X"
        if i < 0 then
            return "-" .. string.format(hex, -i)
        end
        return string.format(hex, i)
    end
    if spec == "j" then
        local ok, json = pcall(require, "json")
        if ok and json and json.encode then
            local ok2, s = pcall(json.encode, v)
            if ok2 and type(s) == "string" then
                return s
            end
        end
        return display(v) -- dkjson 拒绝函数/nan 时退 inspect (JSON.stringify 会省略)
    end
    if spec == "o" or spec == "O" then
        return M.inspect(v)
    end
    if spec == "c" then
        return "" -- 样式转换符: 消费参数, 不产出 (无终端样式层)
    end
    return "%" .. spec
end

local CONSUMING = {
    s = true, d = true, i = true, f = true, j = true,
    o = true, O = true, c = true, x = true, X = true,
}

function M.format(f, ...)
    local n = select("#", ...)
    if n == 0 then
        if type(f) == "string" then
            return f -- Node: 单串参数原样返回 (含 %% 不折叠)
        end
        return display(f)
    end
    if type(f) ~= "string" then
        -- 首参非串: 全部参数空格连接 (Node 同口径)
        local parts = { display(f) }
        for i = 1, n do
            parts[#parts + 1] = display(select(i, ...))
        end
        return table.concat(parts, " ")
    end
    local argi = 0
    local out = {}
    local i, len = 1, #f
    while i <= len do
        local c = f:sub(i, i)
        if c ~= "%" then
            out[#out + 1] = c
            i = i + 1
        else
            local nx = f:sub(i + 1, i + 1)
            if nx == "" then
                out[#out + 1] = "%"
                i = i + 1
            elseif nx == "%" then
                out[#out + 1] = "%"
                i = i + 2
            elseif CONSUMING[nx] then
                if argi < n then
                    argi = argi + 1
                    out[#out + 1] = convert(nx, select(argi, ...))
                else
                    out[#out + 1] = "%" .. nx
                end
                i = i + 2
            else
                out[#out + 1] = "%" -- 未知转换符: '%' 原样, 继续扫描
                i = i + 1
            end
        end
    end
    for j = argi + 1, n do
        out[#out + 1] = " " .. display(select(j, ...))
    end
    return table.concat(out)
end

return M
