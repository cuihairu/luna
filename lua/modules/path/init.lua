--- @module path
-- path: Node path.posix 语义 (Linux 只有 posix 面; win32 不提供)。
-- 选型与勘定见 docs/node-parity.md。语义按 Node v24.21.0 实证钉版:
--   normalize/resolve 折叠连续斜杠 (含前导 //, v24 实测);
--   dirname 是文本操作 (a//b → a/, //a//b → //a/);
--   format 的 dir 原样拼接 (dir=/a/b/ → /a/b//x), 仅 root 无 dir 时不加分隔符;
--   ext 的"全点前缀"规则 (.. 与 .bashrc 无扩展名, a.. → .)。
-- 所有函数对非字符串路径参数 raise (Node TypeError 口径)。

local M = {}

M.sep = "/"
M.delimiter = ":"

local function bad_arg(fn, v)
    error(("path.%s: path must be a string, got %s"):format(fn, type(v)), 3)
end

local function check_string(fn, s)
    if type(s) ~= "string" then
        bad_arg(fn, s)
    end
    return s
end

-- 进程 cwd: luna 随附 lfs 恒在; pcall 兜底给脱离宿主的使用场景。
local cwd_cache
local function cwd()
    if cwd_cache then
        return cwd_cache
    end
    local ok, lfs = pcall(require, "lfs")
    local dir
    if ok and lfs and lfs.currentdir then
        dir = lfs.currentdir()
    end
    if type(dir) ~= "string" or dir == "" then
        dir = os.getenv("PWD")
    end
    if type(dir) ~= "string" or dir == "" then
        dir = "/"
    end
    cwd_cache = dir
    return dir
end

-- 去掉尾随斜杠的 basename (斜杠本身是路径分隔符, 不是名字的一部分)
local function base_name(s)
    local stripped = s:gsub("/+$", "")
    return stripped:match("[^/]*$"), stripped
end

-- basename 的扩展名: Node 规则 —— 最后一个点起到结尾, 但点之前必须
-- 存在非点字符 ('.bashrc' 无扩展名, 'a..' → '.', '..' 无扩展名)。
local function ext_of(base)
    local p = base:match(".*()%.") -- 最后一个点的位置
    if not p or p == 1 then
        return ""
    end
    if not base:sub(1, p - 1):find("[^%.]") then
        return ""
    end
    return base:sub(p)
end

function M.normalize(s)
    check_string("normalize", s)
    local is_abs = s:sub(1, 1) == "/"
    local trailing = s:sub(-1) == "/"
    local stack = {}
    for seg in s:gmatch("[^/]+") do
        if seg == "." then
            -- 跳过
        elseif seg == ".." then
            if #stack > 0 and stack[#stack] ~= ".." then
                table.remove(stack)
            elseif not is_abs then
                stack[#stack + 1] = ".."
            end
        else
            stack[#stack + 1] = seg
        end
    end
    local out = table.concat(stack, "/")
    if is_abs then
        out = "/" .. out
        if trailing and out ~= "/" then
            out = out .. "/"
        end
        return out
    end
    if out == "" then
        out = "."
    end
    if trailing then
        out = out .. "/"
    end
    return out
end

function M.join(...)
    local n = select("#", ...)
    local parts = {}
    for i = 1, n do
        local a = select(i, ...)
        check_string("join", a)
        if a ~= "" then
            parts[#parts + 1] = a
        end
    end
    if #parts == 0 then
        return "."
    end
    return M.normalize(table.concat(parts, "/"))
end

function M.isAbsolute(s)
    check_string("isAbsolute", s)
    return s:sub(1, 1) == "/"
end

function M.resolve(...)
    local n = select("#", ...)
    for i = 1, n do
        check_string("resolve", select(i, ...))
    end
    local acc = ""
    local abs = false
    local i = n
    while i >= 1 and not abs do
        local seg = select(i, ...)
        acc = seg .. "/" .. acc
        abs = seg:sub(1, 1) == "/"
        i = i - 1
    end
    if not abs then
        acc = cwd() .. "/" .. acc
    end
    -- resolve 不保留尾斜杠 (Node 与 normalize 在这一点上分叉)
    local r = M.normalize(acc)
    if #r > 1 and r:sub(-1) == "/" then
        r = r:sub(1, -2)
    end
    return r
end

function M.relative(from, to)
    check_string("relative", from)
    check_string("relative", to)
    local f = M.resolve(from)
    local t = M.resolve(to)
    if f == t then
        return ""
    end
    local fsegs, tsegs = {}, {}
    for seg in f:gmatch("[^/]+") do
        fsegs[#fsegs + 1] = seg
    end
    for seg in t:gmatch("[^/]+") do
        tsegs[#tsegs + 1] = seg
    end
    local k = 0
    while fsegs[k + 1] and fsegs[k + 1] == tsegs[k + 1] do
        k = k + 1
    end
    local parts = {}
    for _ = k + 1, #fsegs do
        parts[#parts + 1] = ".."
    end
    for j = k + 1, #tsegs do
        parts[#parts + 1] = tsegs[j]
    end
    return table.concat(parts, "/")
end

function M.dirname(s)
    check_string("dirname", s)
    if #s == 0 then
        return "."
    end
    local root = s:sub(1, 1) == "/" and "/" or ""
    local matched_slash = true
    local cut
    local i = #s
    while i > #root do
        local c = s:sub(i, i)
        if c == "/" then
            if not matched_slash then
                cut = i
                break
            end
        else
            matched_slash = false
        end
        i = i - 1
    end
    if not cut then
        return root ~= "" and root or "."
    end
    return s:sub(1, cut - 1)
end

function M.basename(s, ext)
    check_string("basename", s)
    if ext ~= nil and type(ext) ~= "string" then
        bad_arg("basename", ext)
    end
    local base = base_name(s)
    if ext and #ext > 0 and #base >= #ext and base:sub(-#ext) == ext then
        base = base:sub(1, #base - #ext)
    end
    return base
end

function M.extname(s)
    check_string("extname", s)
    local base = base_name(s)
    return ext_of(base)
end

function M.parse(s)
    check_string("parse", s)
    local root = #s > 0 and s:sub(1, 1) == "/" and "/" or ""
    local base, stripped = base_name(s)
    local dir
    if base == "" and stripped == "" then
        -- 全斜杠输入: dir 就是根
        dir = root
    else
        local p = stripped:match(".*()/") -- 最后一个斜杠的位置
        if p then
            base = stripped:sub(p + 1)
            dir = stripped:sub(1, p - 1)
            if dir == "" then
                dir = "/"
            end
        else
            dir = root
        end
    end
    local ext = ext_of(base)
    local name = #ext > 0 and base:sub(1, #base - #ext) or base
    return { root = root, dir = dir, base = base, name = name, ext = ext }
end

function M.format(o)
    if type(o) ~= "table" then
        error(("path.format: expects a table, got %s"):format(type(o)), 2)
    end
    -- JS 真值口径: 空串视为缺席 (format({base:'', name:'x'}) → 'x')
    local function field(k)
        local v = o[k]
        if type(v) == "string" and v ~= "" then
            return v
        end
        if v ~= nil and type(v) ~= "string" then
            error(("path.format: %s must be a string, got %s"):format(k, type(v)), 2)
        end
        return nil
    end
    local root, dir = field("root"), field("dir")
    local base, name, ext = field("base"), field("name"), field("ext")
    if ext and ext:sub(1, 1) ~= "." then
        ext = "." .. ext
    end
    local file = base or (name or "") .. (ext or "")
    if dir then
        return dir .. "/" .. file
    end
    if root then
        return root .. file
    end
    return file
end

M.posix = M

return M
