--- @module util
-- util: Node util 的 inspect/format/parseArgs/parseEnv/stripVTControlCharacters/
-- styleText/debuglog 面 (promisify/types 无 Lua 契约锚, 明确不做)。语义按
-- Node v24.21.0 实证钉版, 详见 docs/node-parity.md:
--   format: %s %d %i %f %j %o %O %c 与 %%; %d/%f 不截断 (v24 实测),
--   %i 向零截断; 无参单串原样返回; 未知转换符原样保留; 超参空格尾接。
--   扩展 (todo 契约要求, Node v24 util.format 无此对): %x/%X 十六进制。
--   inspect: 输出为 Lua 源形状 ({} 而非 JS []), 键排序保确定性
--   (pairs 序不定, 与 Node 插入序无法对齐, 记为适配)。
--   parseArgs: config 对象唯一形态 (v24 已弃两参形态); 见函数内口径注释
--   与偏差账 (args 缺省走 arg 尾段, 因 util 不引 process 以避环依赖)。
--   parseEnv/stripVTControlCharacters/styleText/debuglog: 见各函数前口径注释
--   (同样是 v24.21.0 差分对拍)。

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
        if f == nil then
            return "" -- Node: 零参数返回空串
        end
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

-- ------------------------------------------------------------------
-- parseArgs: Node util.parseArgs (v24.21.0 形状对拍, 纯 Lua)
-- ------------------------------------------------------------------
-- 口径: parseArgs({ args?, allowPositionals?, options?, strict?, tokens? })
--   * config 对象唯一形态 (v24.21.0 实测已弃 (args, options) 两参形态);
--   * args 缺省取脚本参数 (arg[1..#arg]); util 不 require process, 避免与
--     process 的环依赖 (process 反向 require util);
--   * options = { <name> = { type, short?, default? } }; type 仅
--     "string"/"boolean" (v24.21.0 实测数组型 type 被拒), short 须单字符,
--     default 须与 type 同型;
--   * values 只含实给选项与声明的 default; 重复选项末次胜; 布尔不消费值;
--   * 长选项 --name=value 为内联值; --name 后值以 "--" 开头判歧义;
--   * 短簇逐字符推进, 遇 string 选项则其后全串为值 (含 "=" 原样, 不判歧义);
--     短选项的值以独立参数给出时同样判歧义;
--   * "--" 终止选项解析, 其后一律位置参; 单 "-" 当位置参;
--   * strict (默认真) 遇未知选项报 ERR_PARSE_ARGS_UNKNOWN_OPTION; false 时
--     未知项按布尔真并入 values;
--   * 位置参须 allowPositionals 显式开启, 否则报 UNEXPECTED_POSITIONAL;
--   * tokens 出 {kind, name, rawName, index[, value][, inlineValue]} 序列;
--   * 错误对象是带 code/message 的表 (非真 TypeError 实例, 与 assert 同口径);
--   * 不立项: type 数组 (v24 亦拒)、--no- 前缀否定 (v24 不认, 报未知选项)。

local function args_fail(code, message)
    error(setmetatable(
        { code = code, message = message },
        { __tostring = function(self) return self.message end }), 0)
end

-- Node inspect 原语拼写: 串带双引号 (仅转义反斜杠与引号), nil 走 undefined
-- (v8 口径, 与 util.format 的 %s 分道)
local function js_prim(v)
    local t = type(v)
    if t == "string" then
        return '"' .. v:gsub('\\', "\\\\"):gsub('"', '\\"') .. '"'
    end
    if t == "number" then
        return numstr(v)
    end
    if t == "boolean" then
        return v and "true" or "false"
    end
    if t == "nil" then
        return "undefined"
    end
    return M.inspect(v)
end

local function sorted_names(t)
    local out = {}
    for k in pairs(t) do
        out[#out + 1] = k
    end
    table.sort(out, function(a, b)
        return tostring(a) < tostring(b)
    end)
    return out
end

-- 脚本参数: arg 尾段 (arg[0] 是脚本自身, 与 process.argv.slice(2) 对齐)
local function script_args()
    local a = rawget(_G, "arg")
    if type(a) ~= "table" then
        return {}
    end
    local out = {}
    for i = 1, #a do
        out[#out + 1] = a[i]
    end
    return out
end

local format = M.format -- M.format 已在上方定义, 取本地别名

local function missing_arg_label(spec, name)
    if spec.short ~= nil then
        return format("-%s, --%s <value>", spec.short, name)
    end
    return format("--%s <value>", name)
end

function M.parseArgs(config)
    config = type(config) == "table" and config or {}
    local args = config.args ~= nil and config.args or script_args()
    local options = type(config.options) == "table" and config.options or {}
    local strict = config.strict ~= false
    local allow_positionals = config.allowPositionals == true
    local want_tokens = config.tokens == true

    -- 选项声明校验 + 短名索引
    local declared, short_map = {}, {}
    for _, name in ipairs(sorted_names(options)) do
        local spec = options[name]
        if type(spec) ~= "table" then
            args_fail("ERR_INVALID_ARG_TYPE",
                format('The "options.%s" property must be of type object. ' ..
                    "Received type %s (%s)",
                    name, type(spec), js_prim(spec)))
        end
        if spec.type ~= nil and spec.type ~= "string" and spec.type ~= "boolean" then
            args_fail("ERR_INVALID_ARG_TYPE",
                format('The "options.%s.type" property must be (%s). ' ..
                    "Received type %s (%s)",
                    name, "'string|boolean'", type(spec.type), js_prim(spec.type)))
        end
        if spec.short ~= nil and (type(spec.short) ~= "string" or #spec.short ~= 1) then
            args_fail("ERR_INVALID_ARG_VALUE",
                format("The property 'options.%s.short' must be a single " ..
                    "character. Received '%s'", name, spec.short))
        end
        if spec.default ~= nil then
            local want = spec.type ~= nil and spec.type or "string"
            if type(spec.default) ~= want then
                args_fail("ERR_INVALID_ARG_TYPE",
                    format('The "options.%s.default" property must be of ' ..
                        "type %s. Received type %s (%s)",
                        name, want, type(spec.default), js_prim(spec.default)))
            end
        end
        declared[name] = spec
        if spec.short ~= nil then
            short_map[spec.short] = name
        end
    end

    local values = {}
    local positionals = {}
    local tokens = {}
    for _, name in ipairs(sorted_names(declared)) do
        local d = declared[name].default
        if d ~= nil then
            values[name] = d
        end
    end

    local pos_mode = false
    local i = 1
    while i <= #args do
        local arg = type(args[i]) == "string" and args[i] or tostring(args[i])
        if not pos_mode and arg == "--" then
            tokens[#tokens + 1] = { kind = "option-terminator", index = i }
            pos_mode = true
        elseif pos_mode or not (arg:sub(1, 1) == "-" and #arg > 1) then
            if allow_positionals or not strict then
                positionals[#positionals + 1] = arg
                tokens[#tokens + 1] = {
                    kind = "positional", index = i, value = arg,
                }
            else
                args_fail("ERR_PARSE_ARGS_UNEXPECTED_POSITIONAL",
                    format("Unexpected argument '%s'. This command does not " ..
                        "take positional arguments", arg))
            end
        elseif arg:sub(1, 2) == "--" then
            -- 长选项: --name[=value]
            local eq = arg:find("=", 3, true)
            local name
            local inline
            if eq ~= nil then
                name, inline = arg:sub(3, eq - 1), arg:sub(eq + 1)
            else
                name = arg:sub(3)
            end
            local raw = "--" .. name
            local spec = declared[name]
            if spec == nil then
                if strict then
                    args_fail("ERR_PARSE_ARGS_UNKNOWN_OPTION",
                        format("Unknown option '%s'", raw))
                end
                if inline == nil then
                    values[name] = true
                    tokens[#tokens + 1] = {
                        kind = "option", name = name, rawName = raw, index = i,
                    }
                else
                    values[name] = inline
                    tokens[#tokens + 1] = {
                        kind = "option", name = name, rawName = raw, index = i,
                        value = inline, inlineValue = true,
                    }
                end
            elseif spec.type == "boolean" then
                if inline ~= nil then
                    args_fail("ERR_PARSE_ARGS_INVALID_OPTION_VALUE",
                        format("Option '%s' does not take an argument", raw))
                end
                values[name] = true
                tokens[#tokens + 1] = {
                    kind = "option", name = name, rawName = raw, index = i,
                }
            else
                -- string
                if inline ~= nil then
                    values[name] = inline
                    tokens[#tokens + 1] = {
                        kind = "option", name = name, rawName = raw, index = i,
                        value = inline, inlineValue = true,
                    }
                else
                    local nxt = i + 1 <= #args and args[i + 1] or nil
                    if nxt == nil then
                        args_fail("ERR_PARSE_ARGS_INVALID_OPTION_VALUE",
                            format("Option '%s' argument missing",
                                missing_arg_label(spec, name)))
                    end
                    nxt = type(nxt) == "string" and nxt or tostring(nxt)
                    if #nxt > 1 and nxt:sub(1, 1) == "-" then
                        args_fail("ERR_PARSE_ARGS_INVALID_OPTION_VALUE",
                            format("Option '%s' argument is ambiguous.\n" ..
                                "Did you forget to specify the option " ..
                                "argument for '%s'?\n" ..
                                "To specify an option argument starting " ..
                                "with a dash use '--%s=-XYZ'.",
                                raw, raw, name))
                    end
                    values[name] = nxt
                    tokens[#tokens + 1] = {
                        kind = "option", name = name, rawName = raw, index = i,
                        value = nxt, inlineValue = false,
                    }
                    i = i + 1
                end
            end
        else
            -- 短簇: 逐字符, string 选项吞其后全串; 空则取下一参数
            local rest = arg:sub(2)
            local j = 1
            local consumed_next = false
            while j <= #rest do
                local c = rest:sub(j, j)
                local name = short_map[c]
                local spec = name ~= nil and declared[name] or nil
                if spec ~= nil and spec.type == "string" then
                    local v = rest:sub(j + 1)
                    if v == "" then
                        local nxt = i + 1 <= #args and args[i + 1] or nil
                        if nxt == nil then
                            args_fail("ERR_PARSE_ARGS_INVALID_OPTION_VALUE",
                                format("Option '%s' argument missing",
                                    missing_arg_label(spec, name)))
                        end
                        nxt = type(nxt) == "string" and nxt or tostring(nxt)
                        if #nxt > 1 and nxt:sub(1, 1) == "-" then
                            args_fail("ERR_PARSE_ARGS_INVALID_OPTION_VALUE",
                                format("Option '%s' argument is ambiguous.\n" ..
                                    "Did you forget to specify the option " ..
                                    "argument for '%s'?\n" ..
                                    "To specify an option argument starting " ..
                                    "with a dash use '--%s=-XYZ' or '-%s-XYZ'.",
                                    "-" .. c, "-" .. c, name, c))
                        end
                        v = nxt
                        tokens[#tokens + 1] = {
                            kind = "option", name = name,
                            rawName = "-" .. c, index = i, value = v,
                            inlineValue = false,
                        }
                        consumed_next = true
                    else
                        tokens[#tokens + 1] = {
                            kind = "option", name = name,
                            rawName = "-" .. c, index = i, value = v,
                            inlineValue = true,
                        }
                    end
                    values[name] = v
                    break
                end
                local raw = "-" .. c
                if spec ~= nil then
                    values[name] = true
                    tokens[#tokens + 1] = {
                        kind = "option", name = name, rawName = raw, index = i,
                    }
                elseif strict then
                    args_fail("ERR_PARSE_ARGS_UNKNOWN_OPTION",
                        format("Unknown option '%s'", raw))
                else
                    values[c] = true
                    tokens[#tokens + 1] = {
                        kind = "option", name = c, rawName = raw, index = i,
                    }
                end
                j = j + 1
            end
            if consumed_next then
                i = i + 1  -- 外层还会再加一
            end
        end
        i = i + 1
    end

    local out = { values = values, positionals = positionals }
    if want_tokens then
        out.tokens = tokens
    end
    return out
end

-- ------------------------------------------------------------------
-- parseEnv / stripVTControlCharacters / styleText / debuglog
-- (Node v24.21.0 差分对拍, 纯 Lua)
-- ------------------------------------------------------------------

-- parseEnv 口径 (Node 源 node_dotenv.cc 行为对拍):
--   * 文件级先剔 \r (CRLF 与孤立 CR 一并);
--   * 行首 (trim 后) # 为注释行; 无 = 的行丢弃; = 前键 trim 后为空丢弃;
--   * 键 trim 后若以 "export " (7 字符含空格) 开头则剥前缀再 trim
--     ("export" 单词无空格不剥; "export =1" 键为 export 不剥);
--   * 值 trim 后以 " ' ` 开头 → 在其后全文找同引号闭合 (可跨行, 闭合引号后
--     同行尾文全弃, 中间行整行吞掉); 双引号内字面 \n 展开为真换行, 其余
--     转义 (\r \\ 等) 原样; 未闭合则保留开引号只取本行;
--   * 无引号值: 首 # 截断再 trim; 键不剥引号; 重复键末次胜; 空输入 {}。
local function env_trim(s)
    return (s:gsub("^[ \t]+", ""):gsub("[ \t]+$", ""))
end

function M.parseEnv(content)
    if type(content) ~= "string" then
        error("util: parseEnv content must be a string", 2)
    end
    local s = content:gsub("\r", "")
    local out = {}
    local i, n = 1, #s
    while i <= n do
        local nl = s:find("\n", i, true) or (n + 1)
        local line = s:sub(i, nl - 1)
        local trimmed = env_trim(line)
        if trimmed ~= "" and trimmed:sub(1, 1) ~= "#" then
            local eq = line:find("=", 1, true)
            if eq ~= nil then
                local key = env_trim(line:sub(1, eq - 1))
                if key ~= "" then
                    if #key >= 7 and key:sub(1, 7) == "export " then
                        key = env_trim(key:sub(8))
                    end
                    local vstart = eq + 1
                    local value = env_trim(line:sub(vstart))
                    local q = value:sub(1, 1)
                    if q == '"' or q == "'" or q == "`" then
                        -- 全文找同引号闭合 (可跨行)
                        local vabs = i + vstart - 1
                        local close = s:find(q, vabs + 1, true)
                        if close ~= nil then
                            value = s:sub(vabs + 1, close - 1)
                            if q == '"' then
                                value = value:gsub("\\n", "\n")
                            end
                            -- 跳到闭合引号所在行的行尾之后
                            i = s:find("\n", close, true) or (n + 1)
                        end
                        -- 未闭合: 开引号保留, 值只取本行, 下一行照常解析
                    else
                        local hash = value:find("#", 1, true)
                        if hash ~= nil then
                            value = env_trim(value:sub(1, hash - 1))
                        end
                    end
                    out[key] = value
                end
            end
        end
        i = nl + 1
    end
    return out
end

-- stripVTControlCharacters: 剔 ANSI/VT 控制序列 (Node 内建 ansi-regex 语义):
--   OSC = ESC] ... 至 BEL | ESC\ | \156 (未闭合落到 CSI 分支);
--   CSI = (ESC|\155) + 中间符 []()#;? + 可选参数 (1-4 位数字, (;: 分隔可重复)
--   + 终符类 (数字与 A-PR-TZ cf-nq-uy=><~); 匹配失败的 ESC 原样保留。
local VT_INTERM = {}
for _, c in ipairs({ "[", "]", "(", ")", "#", ";", "?" }) do
    VT_INTERM[c] = true
end
local VT_FINAL = {} -- [\dA-PR-TZcf-nq-uy=><~]
for _, rg in ipairs({
    { 48, 57 }, { 65, 80 }, { 82, 84 }, { 90, 90 }, { 99, 99 },
    { 102, 110 }, { 113, 117 }, { 121, 121 }, { 61, 61 }, { 62, 62 },
    { 60, 60 }, { 126, 126 },
}) do
    for c = rg[1], rg[2] do
        VT_FINAL[string.char(c)] = true
    end
end

function M.stripVTControlCharacters(str)
    if type(str) ~= "string" then
        error("util: stripVTControlCharacters str must be a string", 2)
    end
    if not str:find("\27", 1, true) and not str:find("\155", 1, true) then
        return str
    end
    local out = {}
    local i, n = 1, #str
    while i <= n do
        local c = str:sub(i, i)
        if c ~= "\27" and c ~= "\155" then
            out[#out + 1] = c
            i = i + 1
        else
            local handled = false
            -- OSC: ESC] 惰性扫描首个终结符 (BEL | ESC\ | \156)
            if c == "\27" and str:sub(i + 1, i + 1) == "]" then
                local j = i + 2
                while j <= n do
                    local cc = str:sub(j, j)
                    if cc == "\7" then
                        i = j + 1
                        handled = true
                        break
                    elseif cc == "\27" and str:sub(j + 1, j + 1) == "\\" then
                        i = j + 2
                        handled = true
                        break
                    elseif cc == "\156" then
                        i = j + 1
                        handled = true
                        break
                    end
                    j = j + 1
                end
            end
            if not handled then
                -- CSI: 中间符 + 参数 (1-4 位数字, (;: 分隔) + 终符
                local j = i + 1
                while j <= n and VT_INTERM[str:sub(j, j)] do
                    j = j + 1
                end
                local k = j
                if str:sub(k, k):match("^%d") then
                    local cnt = 0
                    while k <= n and cnt < 4 and str:sub(k, k):match("^%d") do
                        k = k + 1
                        cnt = cnt + 1
                    end
                    while str:sub(k, k) == ";" or str:sub(k, k) == ":" do
                        k = k + 1
                        local cnt2 = 0
                        while k <= n and cnt2 < 4 and str:sub(k, k):match("^%d") do
                            k = k + 1
                            cnt2 = cnt2 + 1
                        end
                    end
                end
                if VT_FINAL[str:sub(k, k)] then
                    i = k + 1
                    handled = true
                end
            end
            if not handled then
                out[#out + 1] = c -- ESC 原样保留, 前进一位
                i = i + 1
            end
        end
    end
    return table.concat(out)
end

-- styleText: 色名/十六进制/数组 → SGR 码包裹 (Node v24 口径):
--   * 默认 validateStream=true 且无 TTY → 原样返回 (luna 默认非 TTY);
--     validateStream=false 恒出码, stream.isTTY == true 出码;
--   * 数组按序开码、逆序前插闭码 (["bold","red"] → 开 1,31 闭 39,22);
--   * none 与空数组原样; #RGB 三位每位 ×17 (CSS 口径), #RRGGBB 直接;
--   * 非法名/非法十六进制报 ERR_INVALID_ARG_VALUE (错误消息列全名);
--   * 偏差: Node 的 replaceCloseCode 嵌套重开不实现 (简单包裹)。
local STYLE_CODES = {
    reset = { 0, 0 }, bold = { 1, 22 }, dim = { 2, 22 }, italic = { 3, 23 },
    underline = { 4, 24 }, blink = { 5, 25 }, inverse = { 7, 27 },
    hidden = { 8, 28 }, strikethrough = { 9, 29 }, doubleunderline = { 21, 24 },
    black = { 30, 39 }, red = { 31, 39 }, green = { 32, 39 }, yellow = { 33, 39 },
    blue = { 34, 39 }, magenta = { 35, 39 }, cyan = { 36, 39 }, white = { 37, 39 },
    bgBlack = { 40, 49 }, bgRed = { 41, 49 }, bgGreen = { 42, 49 },
    bgYellow = { 43, 49 }, bgBlue = { 44, 49 }, bgMagenta = { 45, 49 },
    bgCyan = { 46, 49 }, bgWhite = { 47, 49 }, framed = { 51, 54 },
    overlined = { 53, 55 }, gray = { 90, 39 }, redBright = { 91, 39 },
    greenBright = { 92, 39 }, yellowBright = { 93, 39 }, blueBright = { 94, 39 },
    magentaBright = { 95, 39 }, cyanBright = { 96, 39 }, whiteBright = { 97, 39 },
    bgGray = { 100, 49 }, bgRedBright = { 101, 49 }, bgGreenBright = { 102, 49 },
    bgYellowBright = { 103, 49 }, bgBlueBright = { 104, 49 },
    bgMagentaBright = { 105, 49 }, bgCyanBright = { 106, 49 },
    bgWhiteBright = { 107, 49 }, grey = { 90, 39 }, blackBright = { 90, 39 },
    bgGrey = { 100, 49 }, bgBlackBright = { 100, 49 }, faint = { 2, 22 },
    crossedout = { 9, 29 }, strikeThrough = { 9, 29 }, crossedOut = { 9, 29 },
    conceal = { 8, 28 }, swapColors = { 7, 27 }, swapcolors = { 7, 27 },
    doubleUnderline = { 21, 24 },
}
-- 错误消息的合法名清单 (Node 声明序)
local STYLE_LIST = table.concat({
    "reset", "bold", "dim", "italic", "underline", "blink", "inverse",
    "hidden", "strikethrough", "doubleunderline", "black", "red", "green",
    "yellow", "blue", "magenta", "cyan", "white", "bgBlack", "bgRed",
    "bgGreen", "bgYellow", "bgBlue", "bgMagenta", "bgCyan", "bgWhite",
    "framed", "overlined", "gray", "redBright", "greenBright",
    "yellowBright", "blueBright", "magentaBright", "cyanBright",
    "whiteBright", "bgGray", "bgRedBright", "bgGreenBright",
    "bgYellowBright", "bgBlueBright", "bgMagentaBright", "bgCyanBright",
    "bgWhiteBright", "grey", "blackBright", "bgGrey", "bgBlackBright",
    "faint", "crossedout", "strikeThrough", "crossedOut", "conceal",
    "swapColors", "swapcolors", "doubleUnderline",
}, "', '")
STYLE_LIST = "'" .. STYLE_LIST .. "'"

local function style_fail(received)
    error(setmetatable(
        { code = "ERR_INVALID_ARG_VALUE",
          message = format("The argument 'format' must be one of: %s. " ..
            "Received %s", STYLE_LIST, js_prim(received)) },
        { __tostring = function(self) return self.message end }), 0)
end

local function hex_rgb(f)
    -- #RGB (每位 ×17, CSS 口径) / #RRGGBB; 非法返回 nil
    if #f == 4 then
        local r, g, b = tonumber(f:sub(2, 2), 16), tonumber(f:sub(3, 3), 16),
            tonumber(f:sub(4, 4), 16)
        if r and g and b then
            return r * 17, g * 17, b * 17
        end
        return nil
    end
    if #f == 7 then
        local r, g, b = tonumber(f:sub(2, 3), 16), tonumber(f:sub(4, 5), 16),
            tonumber(f:sub(6, 7), 16)
        if r and g and b then
            return r, g, b
        end
        return nil
    end
    return nil
end

function M.styleText(f, text, opts)
    if type(f) ~= "string" and type(f) ~= "table" then
        error("util: styleText format must be a string or an array", 2)
    end
    if type(text) ~= "string" then
        error("util: styleText text must be a string", 2)
    end
    opts = opts or {}
    if type(opts) ~= "table" then
        error("util: styleText opts must be a table", 2)
    end
    local colorize
    if opts.validateStream == false then
        colorize = true
    else
        local stream = opts.stream or {} -- luna 默认非 TTY (无 isTTY 字段)
        if type(stream) ~= "table" then
            error("util: styleText opts.stream must be a table", 2)
        end
        colorize = stream.isTTY == true
    end
    local opens, closes = {}, {}
    local fmts = type(f) == "table" and f or { f }
    for idx = 1, #fmts do
        local one = fmts[idx]
        if type(one) ~= "string" then
            style_fail(one)
        end
        if one ~= "none" then
            local open, close
            if one:sub(1, 1) == "#" then
                local r, g, b = hex_rgb(one)
                if r == nil then
                    style_fail(one)
                end
                open = format("\27[38;2;%d;%d;%dm", r, g, b)
                close = "\27[39m"
            else
                local pair = STYLE_CODES[one]
                if pair == nil then
                    style_fail(one)
                end
                open = format("\27[%dm", pair[1])
                close = format("\27[%dm", pair[2])
            end
            opens[#opens + 1] = open
            table.insert(closes, 1, close) -- 逆序前插: 闭码反序
        end
    end
    if #opens == 0 or not colorize then
        return text
    end
    return table.concat(opens) .. text .. table.concat(closes)
end

-- debuglog(section): NODE_DEBUG 门控的 stderr 调试日志 (Node v24 口径):
--   * 门控: 逗号分隔条目, 大小写不敏感, 条目不 trim (带空格不匹配),
--     "*" 全量, 尾 "*" 前缀通配 (plain 条目精确匹配);
--   * 每次调用现读 env (惰性, 不在创建时快照);
--   * 输出 "SECTION PID: msg\n" 至 io.stderr (SECTION 大写, msg 走
--     util.format 格式化); 未启用时 no-op;
--   * pid 经 kernel 模块 (util 不引 process 避环依赖, kernel 是 C 模块无环);
--   * 偏差: 回调第二参 (Node 传 stream) 不支持。
local kernel_pid
do
    local ok, kernel = pcall(require, "kernel")
    if ok and type(kernel) == "table" and type(kernel.pid) == "function" then
        kernel_pid = kernel.pid
    else
        kernel_pid = function() return 0 end
    end
end

function M.debuglog(section)
    if type(section) ~= "string" then
        error("util: debuglog section must be a string", 2)
    end
    local lower = section:lower()
    return function(...)
        local env = os.getenv("NODE_DEBUG")
        if env == nil then
            return
        end
        local on = false
        for raw_entry in env:gmatch("[^,]+") do
            local entry = raw_entry:lower()
            if entry == "*" or entry == lower then
                on = true
                break
            end
            if entry:sub(-1) == "*" and lower:sub(1, #entry - 1) == entry:sub(1, -2) then
                on = true
                break
            end
        end
        if not on then
            return
        end
        io.stderr:write(format("%s %d: %s\n",
            section:upper(), kernel_pid(), M.format(...)))
    end
end

return M
