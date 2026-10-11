--- @module path
-- path: Node path 语义, 双面提供——posix (默认, 即本表) 与 win32
-- (path.win32, 平台无关的文本语义)。选型与勘定见 docs/node-parity.md。
-- 语义按 Node v24.21.0 实证钉版 (lib/path.js 逐段移植):
--   posix: normalize/resolve 折叠连续斜杠 (含前导 //); dirname 是文本
--   操作 (a//b → a/); format 的 dir 原样拼接; ext 的"全点前缀"规则;
--   relative 沿 Node 的字符级算法 (公共分隔符前的段才回退 ..)。
-- win32 面: 分隔符 \ 与 / 解析通用; 设备根 (盘符/完整 UNC/\?\ 命名
-- 空间) 语义与 relative 的大小写折叠见 win32 段首注释。
-- 所有函数对非字符串路径参数 raise (Node TypeError 口径; 例外:
-- win32.toNamespacedPath 沿 Node 收紧为 raise, Node 原样返回非字符串)。

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

-- 进程 cwd: 每次现取 (Node 的 process.cwd() 不缓存, chdir 后不读旧值);
-- luna 随附 lfs 恒在, pcall 兜底给脱离宿主的使用场景。
local function cwd()
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

-- Node 的字符级 relative (win32 与 posix 同构): 比较用 f/t (可为折叠
-- 形), 输出切片取 f_orig/t_orig 原形; win32ish 决定首尾分隔符削减与
-- 根判定索引 (win32 为 2, posix 为 0)。计数器沿用 Node 的 0-based。
-- 返回 nil 表示无公共分隔符可比 (win32), 调用方回退 t 的原形。
local function rel_chars(f, t, f_orig, t_orig, sep, win32ish, root_i)
    local fs = win32ish and 0 or 1
    local fe = #f
    local ts = win32ish and 0 or 1
    local te = #t
    if win32ish then
        while fs < fe and f:sub(fs + 1, fs + 1) == sep do
            fs = fs + 1
        end
        while fe - 1 > fs and f:sub(fe, fe) == sep do
            fe = fe - 1
        end
        while ts < te and t:sub(ts + 1, ts + 1) == sep do
            ts = ts + 1
        end
        while te - 1 > ts and t:sub(te, te) == sep do
            te = te - 1
        end
    end
    local flen = fe - fs
    local tlen = te - ts
    local length = flen < tlen and flen or tlen
    local last_sep = -1
    local i = 0
    while i < length do
        local c = f:sub(fs + i + 1, fs + i + 1)
        if c ~= t:sub(ts + i + 1, ts + i + 1) then
            break
        end
        if c == sep then
            last_sep = i
        end
        i = i + 1
    end
    if i ~= length then
        if win32ish and last_sep == -1 then
            return nil
        end
    else
        if tlen > length then
            if t:sub(ts + i + 1, ts + i + 1) == sep then
                return t_orig:sub(ts + i + 2)
            end
            if i == root_i then
                return t_orig:sub(ts + i + 1)
            end
        elseif flen > length then
            if f:sub(fs + i + 1, fs + i + 1) == sep then
                last_sep = i
            elseif i == root_i then
                last_sep = win32ish and 3 or 0
            end
        end
    end
    local out = ""
    for j = fs + last_sep + 1, fe do
        if j == fe or f:sub(j + 1, j + 1) == sep then
            out = out .. (out == "" and ".." or sep .. "..")
        end
    end
    local tostart = ts + last_sep
    if out ~= "" then
        return out .. t_orig:sub(tostart + 1, te)
    end
    if win32ish then
        if t_orig:sub(tostart + 1, tostart + 1) == sep then
            tostart = tostart + 1
        end
        return t_orig:sub(tostart + 1, te)
    end
    return t_orig:sub(tostart + 1)
end

function M.relative(from, to)
    check_string("relative", from)
    check_string("relative", to)
    if from == to then
        return ""
    end
    local f = M.resolve(from)
    local t = M.resolve(to)
    if f == t then
        return ""
    end
    local r = rel_chars(f, t, f, t, "/", false, 0)
    return r ~= nil and r or t
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

-- Node posix.toNamespacedPath: 校验后原样返回 (posix 无设备名空间)
function M.toNamespacedPath(s)
    check_string("toNamespacedPath", s)
    return s
end

M.posix = M

-- ============ win32 面 ============
-- path.win32: Node path.win32 的文本语义 (平台无关——Node 在 Linux 上
-- 同样提供)。语义按 Node v24.21.0 lib/path.js 逐段移植:
--   分隔符 \ 与 / 解析通用, normalize/resolve 输出统一 \;
--   dirname/basename/extname/parse 是文本操作 (原样保留输入斜杠风格);
--   设备根: 盘符 ([A-Za-z]:) / 完整 UNC (\\s\sh) / \?\ 命名空间;
--   不完整 UNC (//a) 折叠为普通根; 设备相对路径 (C:foo) 无根;
--   resolve: 设备认领后异设备段整段丢弃, 已绝对且无设备时更左的段
--   丢弃; 设备相对走 cwd 尾随 (env '=X:' 在 posix 宿主恒缺);
--   relative 大小写折叠比较 (仅大小写异 → '', 大小写混异 → 分段比较,
--   无公共分隔符 → 返回目标原形); basename 的 ext 大小写敏感;
--   保留设备名 (COM1 等) 走防误折叠分支; CVE-2024-36139 防护保留。

local W = {}

W.sep = "\\"
W.delimiter = ";"

local function w_issep(c)
    return c == "/" or c == "\\"
end

local function w_alpha(c)
    return c:match("^[A-Za-z]$") ~= nil
end

-- Windows 保留设备名 (Node WINDOWS_RESERVED_NAMES; UTF-8 上标变体
-- COM\xb9 等 upper() 无法复现, 不列)
local w_reserved = { CON = true, PRN = true, AUX = true, NUL = true }
for i = 1, 9 do
    w_reserved[("COM%d"):format(i)] = true
    w_reserved[("LPT%d"):format(i)] = true
end

-- normalizeString: 折叠 . 与 .. (allow_above 允许 .. 溢出到根之上)
local function w_normstr(s, allow_above)
    local stack = {}
    for seg in s:gmatch("[^/\\]+") do
        if seg ~= "." then
            if seg ~= ".." then
                stack[#stack + 1] = seg
            elseif #stack > 0 and stack[#stack] ~= ".." then
                table.remove(stack)
            elseif allow_above then
                stack[#stack + 1] = ".."
            end
        end
    end
    return table.concat(stack, "\\")
end

-- 按 \ 分段 (保留空段, Node StringPrototypeSplit 口径)
local function split_bs(s)
    local out = {}
    local prev = 1
    for i = 1, #s do
        if s:sub(i, i) == "\\" then
            out[#out + 1] = s:sub(prev, i - 1)
            prev = i + 1
        end
    end
    out[#out + 1] = s:sub(prev)
    return out
end

function W.normalize(s)
    check_string("win32.normalize", s)
    local n = #s
    if n == 0 then
        return "."
    end
    local c1 = s:sub(1, 1)
    if n == 1 then
        return w_issep(c1) and "\\" or s
    end
    local dev -- nil = 无设备 (JS undefined 口径)
    local root_end = 0
    local abs = false
    if w_issep(c1) then
        abs = true
        if w_issep(s:sub(2, 2)) then
            local j = 3
            local last = j
            while j <= n and not w_issep(s:sub(j, j)) do
                j = j + 1
            end
            if j <= n and j ~= last then
                local first = s:sub(last, j - 1)
                last = j
                while j <= n and w_issep(s:sub(j, j)) do
                    j = j + 1
                end
                if j <= n and j ~= last then
                    last = j
                    while j <= n and not w_issep(s:sub(j, j)) do
                        j = j + 1
                    end
                    if j > n or j ~= last then
                        if first == "." or first == "?" then
                            -- 设备名空间 (\\.\x / \\?\x); \\?\COM1: 之类
                            -- 保留名把设备延长到冒号后
                            dev = "\\\\" .. first
                            root_end = 4
                            local ci = s:find(":", 1, true)
                            if ci then
                                local possible = s:sub(5, ci)
                                if w_reserved[possible:sub(1, #possible - 1):upper()] then
                                    dev = "\\\\?\\" .. possible
                                    root_end = 4 + #possible
                                end
                            end
                        elseif j > n then
                            -- UNC 根独占全串
                            return "\\\\" .. first .. "\\" .. s:sub(last) .. "\\"
                        else
                            dev = "\\\\" .. first .. "\\" .. s:sub(last, j - 1)
                            root_end = j - 1
                        end
                    end
                end
            end
        else
            root_end = 1
        end
    else
        local ci = s:find(":", 1, true)
        if ci and ci > 1 then
            if w_alpha(c1) and ci == 2 then
                dev = s:sub(1, 2)
                root_end = 2
                if n > 2 and w_issep(s:sub(3, 3)) then
                    abs = true
                    root_end = 3
                end
            elseif w_reserved[s:sub(1, ci - 1):upper()] then
                -- 设备相对的保留名 (COM1:x)
                dev = s:sub(1, ci)
                root_end = ci
            end
        end
    end
    local out = ""
    if root_end < n then
        out = w_normstr(s:sub(root_end + 1), not abs)
    end
    if out == "" and not abs then
        out = "."
    end
    if out ~= "" and w_issep(s:sub(n)) then
        out = out .. "\\"
    end
    if not abs and dev == nil and s:find(":", 1, true) then
        -- CVE-2024-36139: 相对尾段可能被 Windows 当作设备相对根
        if #out >= 2 and w_alpha(out:sub(1, 1)) and out:sub(2, 2) == ":" then
            return ".\\" .. out
        end
        local idx = s:find(":", 1, true)
        while idx do
            if idx == n or w_issep(s:sub(idx + 1, idx + 1)) then
                return ".\\" .. out
            end
            idx = s:find(":", idx + 1, true)
        end
    end
    local ci = s:find(":", 1, true)
    if ci and w_reserved[s:sub(1, ci - 1):upper()] then
        return ".\\" .. (dev or "") .. out
    end
    if dev == nil then
        return abs and ("\\" .. out) or out
    end
    return abs and (dev .. "\\" .. out) or (dev .. out)
end

function W.join(...)
    local n = select("#", ...)
    local parts = {}
    for i = 1, n do
        local a = select(i, ...)
        check_string("win32.join", a)
        if a ~= "" then
            parts[#parts + 1] = a
        end
    end
    if #parts == 0 then
        return "."
    end
    local first = parts[1]
    local joined = table.concat(parts, "\\")
    -- 归一化会把前导 // 误当 UNC 根: 折叠成单个 \, 除非首段明显就是 UNC
    local slash_count = 0
    local needs_replace = true
    if w_issep(first:sub(1, 1)) then
        slash_count = 1
        local fl = #first
        if fl > 1 and w_issep(first:sub(2, 2)) then
            slash_count = 2
            if fl > 2 then
                if w_issep(first:sub(3, 3)) then
                    slash_count = 3
                else
                    needs_replace = false
                end
            end
        end
    end
    if needs_replace then
        local i = slash_count
        while i < #joined and w_issep(joined:sub(i + 1, i + 1)) do
            i = i + 1
        end
        slash_count = i
        if slash_count >= 2 then
            joined = "\\" .. joined:sub(slash_count + 1)
        end
    end
    -- 保留名设备 (COM1: 等): 跳过归一化, 仅把 / 换成 \
    local segs = {}
    local part = ""
    local i = 1
    while i <= #joined do
        if joined:sub(i, i) == "\\" then
            if part ~= "" then
                segs[#segs + 1] = part
            end
            part = ""
            while i + 1 <= #joined and joined:sub(i + 1, i + 1) == "\\" do
                i = i + 1
            end
        else
            part = part .. joined:sub(i, i)
        end
        i = i + 1
    end
    if part ~= "" then
        segs[#segs + 1] = part
    end
    for _, p in ipairs(segs) do
        local ci = p:find(":", 1, true)
        if ci and w_reserved[p:sub(1, ci - 1):upper()] then
            local out = ""
            for k = 1, #joined do
                local c = joined:sub(k, k)
                out = out .. (c == "/" and "\\" or c)
            end
            return out
        end
    end
    return W.normalize(joined)
end

function W.isAbsolute(s)
    check_string("win32.isAbsolute", s)
    local n = #s
    if n == 0 then
        return false
    end
    if w_issep(s:sub(1, 1)) then
        return true
    end
    return n > 2 and w_alpha(s:sub(1, 1)) and s:sub(2, 2) == ":"
        and w_issep(s:sub(3, 3))
end

-- resolve 的根扫描: 返回 dev ('' | 'C:' | '\\s\sh' | '\\?'/'\.'),
-- root_end (根消耗的字符数, Node 0b rootEnd 口径), 是否绝对
local function w_root_scope(s)
    local n = #s
    local c1 = s:sub(1, 1)
    if n == 1 then
        if w_issep(c1) then
            return "", 1, true
        end
        return "", 0, false
    end
    if w_issep(c1) then
        if w_issep(s:sub(2, 2)) then
            local j = 3
            local last = j
            while j <= n and not w_issep(s:sub(j, j)) do
                j = j + 1
            end
            if j <= n and j ~= last then
                local first = s:sub(last, j - 1)
                last = j
                while j <= n and w_issep(s:sub(j, j)) do
                    j = j + 1
                end
                if j <= n and j ~= last then
                    last = j
                    while j <= n and not w_issep(s:sub(j, j)) do
                        j = j + 1
                    end
                    if j > n or j ~= last then
                        if first ~= "." and first ~= "?" then
                            return "\\\\" .. first .. "\\" .. s:sub(last, j - 1), j - 1, true
                        end
                        -- 设备名空间 (\\.\x / \\?\x): 设备只取前缀
                        return "\\\\" .. first, 4, true
                    end
                end
            end
        end
        return "", 1, true
    end
    if w_alpha(c1) and s:sub(2, 2) == ":" then
        if n > 2 and w_issep(s:sub(3, 3)) then
            return s:sub(1, 2), 3, true
        end
        return s:sub(1, 2), 2, false
    end
    return "", 0, false
end

function W.resolve(...)
    local n = select("#", ...)
    for i = 1, n do
        check_string("win32.resolve", select(i, ...))
    end
    local dev, tail, abs = "", "", false
    for i = n, 0, -1 do
        local p
        local skip = false
        if i >= 1 then
            p = select(i, ...)
            if p == "" then
                skip = true
            end
        elseif dev == "" then
            p = cwd()
            -- Node 快路径: 无参或单参 (空/点) 且 cwd 以分隔符起头 →
            -- cwd 的 / 全部替换为 \ 直接返回
            if (n == 0 or (n == 1 and (select(1, ...) == "" or select(1, ...) == ".")))
                and w_issep(p:sub(1, 1)) then
                return (p:gsub("/", "\\"))
            end
        else
            -- 设备相对: env '=X:' 在 posix 宿主恒缺, cwd 不匹配设备也
            -- 不以 \ 起头 → Node 核验失败仍落 cwd, 尾段接在设备之后
            p = cwd()
        end
        if not skip then
            local pdev, root_end, pabs = w_root_scope(p)
            local drop = false
            if pdev ~= "" then
                if dev ~= "" then
                    if pdev:lower() ~= dev:lower() then
                        -- 异设备: 整段丢弃
                        drop = true
                    end
                else
                    dev = pdev
                end
            end
            if not drop then
                if abs then
                    if dev ~= "" then
                        break
                    end
                    -- 已绝对且无设备: 更左的段整段丢弃
                else
                    tail = p:sub(root_end + 1) .. "\\" .. tail
                    abs = pabs
                    if pabs and dev ~= "" then
                        break
                    end
                end
            end
        end
    end
    local out = w_normstr(tail, not abs)
    if abs then
        return dev .. "\\" .. out
    end
    local r = dev .. out
    return r ~= "" and r or "."
end

function W.relative(from, to)
    check_string("win32.relative", from)
    check_string("win32.relative", to)
    if from == to then
        return ""
    end
    local f = W.resolve(from)
    local t = W.resolve(to)
    if f == t then
        return ""
    end
    local fl = f:lower()
    local tl = t:lower()
    if fl == tl then
        -- 大小写不敏感: 仅大小写不同视为同路径
        return ""
    end
    if #f ~= #fl or #t ~= #tl then
        -- 大小写混异: 按分隔符分段比较, 输出保留原形
        local fsegs, tsegs = split_bs(f), split_bs(t)
        if fsegs[#fsegs] == "" then
            table.remove(fsegs)
        end
        if tsegs[#tsegs] == "" then
            table.remove(tsegs)
        end
        local fn, tn = #fsegs, #tsegs
        local length = fn < tn and fn or tn
        local k = 0
        while k < length and fsegs[k + 1]:lower() == tsegs[k + 1]:lower() do
            k = k + 1
        end
        if k == 0 then
            return t
        end
        if k == length then
            if tn > length then
                local rest = {}
                for j = k + 1, tn do
                    rest[#rest + 1] = tsegs[j]
                end
                return table.concat(rest, "\\")
            end
            if fn > length then
                return ("..\\"):rep(fn - 1 - k) .. ".."
            end
            return ""
        end
        local rest = {}
        for j = k + 1, tn do
            rest[#rest + 1] = tsegs[j]
        end
        return ("..\\"):rep(fn - k) .. table.concat(rest, "\\")
    end
    local r = rel_chars(fl, tl, f, t, "\\", true, 2)
    return r ~= nil and r or t
end

function W.dirname(s)
    check_string("win32.dirname", s)
    if #s == 0 then
        return "."
    end
    local d = W.parse(s).dir
    return d ~= "" and d or "."
end

function W.basename(s, ext)
    check_string("win32.basename", s)
    if ext ~= nil and type(ext) ~= "string" then
        bad_arg("win32.basename", ext)
    end
    local n = #s
    -- 盘符前的分隔符不算 (C:/a/b 从盘符后起扫); 计数器沿用 Node 0-based
    local start = 0
    if n >= 2 and w_alpha(s:sub(1, 1)) and s:sub(2, 2) == ":" then
        start = 2
    end
    if ext ~= nil and #ext > 0 and #ext <= n then
        if ext == s then
            return ""
        end
        local ext_idx = #ext -- ext 里下一个待比字符 (1-based, 从尾起)
        local first_non_sep = -1
        local matched = true
        local e = -1
        local i = n
        while i >= start + 1 do
            local c = s:sub(i, i)
            if w_issep(c) then
                if not matched then
                    start = i -- 0b start = i (Lua pos i → 0b i)
                    break
                end
            else
                if first_non_sep == -1 then
                    matched = false
                    first_non_sep = i
                end
                if ext_idx >= 1 then
                    if c == ext:sub(ext_idx, ext_idx) then
                        ext_idx = ext_idx - 1
                        if ext_idx == 0 then
                            e = i - 1
                        end
                    else
                        -- 后缀不匹配: 结果是整个尾段
                        ext_idx = 0
                        e = first_non_sep
                    end
                end
            end
            i = i - 1
        end
        if start == e then
            e = first_non_sep
        elseif e == -1 then
            e = n
        end
        return s:sub(start + 1, e)
    end
    local matched = true
    local e = -1
    local i = n
    while i >= start + 1 do
        local c = s:sub(i, i)
        if w_issep(c) then
            if not matched then
                start = i
                break
            end
        elseif e == -1 then
            matched = false
            e = i
        end
        i = i - 1
    end
    if e == -1 then
        return ""
    end
    return s:sub(start + 1, e)
end

function W.extname(s)
    check_string("win32.extname", s)
    return W.parse(s).ext
end

-- parse 的根长 (Node parse 的 UNC 扫描): 文本语义原样保留输入斜杠,
-- UNC 根不标准化; 根独占全串时到串尾, 有余段时含 share2 后一个分隔符
local function w_parse_rootlen(s)
    local n = #s
    if n == 1 then
        return w_issep(s:sub(1, 1)) and 1 or 0
    end
    local c1 = s:sub(1, 1)
    if w_issep(c1) then
        if w_issep(s:sub(2, 2)) then
            local j = 3
            local last = j
            while j <= n and not w_issep(s:sub(j, j)) do
                j = j + 1
            end
            if j <= n and j ~= last then
                last = j
                while j <= n and w_issep(s:sub(j, j)) do
                    j = j + 1
                end
                if j <= n and j ~= last then
                    last = j
                    while j <= n and not w_issep(s:sub(j, j)) do
                        j = j + 1
                    end
                    if j > n then
                        return j - 1 -- UNC 根独占全串
                    end
                    if j ~= last then
                        return j -- 根含 share2 后的一个分隔符
                    end
                end
            end
        end
        return 1
    end
    if w_alpha(c1) and s:sub(2, 2) == ":" then
        if n <= 2 then
            return 2
        end
        if w_issep(s:sub(3, 3)) then
            return 3
        end
        return 2
    end
    return 0
end

function W.parse(s)
    check_string("win32.parse", s)
    local n = #s
    if n == 1 then
        if w_issep(s:sub(1, 1)) then
            return { root = s, dir = s, base = "", name = "", ext = "" }
        end
        return { root = "", dir = "", base = s, name = s, ext = "" }
    end
    local rl = w_parse_rootlen(s)
    local root = s:sub(1, rl)
    local tail = s:sub(rl + 1)
    local e -- 尾段最后一个非分隔符 (尾随分隔符不算)
    for i = #tail, 1, -1 do
        if not w_issep(tail:sub(i, i)) then
            e = i
            break
        end
    end
    local base, dir
    if not e then
        base = ""
        dir = root
    else
        local p -- 尾段之前的最后一个分隔符
        for i = e, 1, -1 do
            if w_issep(tail:sub(i, i)) then
                p = i
                break
            end
        end
        if p then
            base = tail:sub(p + 1, e)
            dir = s:sub(1, rl + p - 1)
        else
            base = tail:sub(1, e)
            dir = root
        end
    end
    local ext = ext_of(base)
    local name = #ext > 0 and base:sub(1, #base - #ext) or base
    return { root = root, dir = dir, base = base, name = name, ext = ext }
end

function W.format(o)
    if type(o) ~= "table" then
        error(("path.win32.format: expects a table, got %s"):format(type(o)), 2)
    end
    -- JS 真值口径同 posix: 空串视为缺席; d = dir||root, d==root 时不
    -- 加分隔符 (Node _format)
    local function field(k)
        local v = o[k]
        if type(v) == "string" and v ~= "" then
            return v
        end
        if v ~= nil and type(v) ~= "string" then
            error(("path.win32.format: %s must be a string, got %s"):format(k, type(v)), 2)
        end
        return nil
    end
    local root, dir = field("root"), field("dir")
    local base, name, ext = field("base"), field("name"), field("ext")
    if ext and ext:sub(1, 1) ~= "." then
        ext = "." .. ext
    end
    local file = base or (name or "") .. (ext or "")
    local d = dir or root
    if not d then
        return file
    end
    if d == root then
        return d .. file
    end
    return d .. "\\" .. file
end

function W.toNamespacedPath(s)
    check_string("win32.toNamespacedPath", s)
    if #s == 0 then
        return s
    end
    local r = W.resolve(s)
    if #r < 3 then
        -- 裸根 (resolve 到 '/' → '\') 原样返回入参
        return s
    end
    if r:sub(1, 2) == "\\\\" and r:sub(3, 3) ~= "?" and r:sub(3, 3) ~= "." then
        return "\\\\?\\UNC" .. r:sub(2)
    end
    if w_alpha(r:sub(1, 1)) and r:sub(2, 2) == ":" and r:sub(3, 3) == "\\" then
        return "\\\\?\\" .. r
    end
    return r
end

M.win32 = W

return M
