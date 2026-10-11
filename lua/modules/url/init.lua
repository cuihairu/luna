-- url: Node url legacy 面 (v24.21.0 lib/url.js + lib/internal/url.js, 纯 Lua)。
--   parse/format/resolve/resolveObject; 字段缺省一律 nil (Node 的 null);
--   parse 第二参为真时 query 走 querystring.parse。
--   WHATWG URL/URLSearchParams/domainToASCII/pathToFileURL/urlToHttpOptions
--   未立项 (账记); 不立 Url 构造器 (parse 结果即等价对象)。
--   已知偏差: IDNA toASCII 不做 (非 ASCII 主机名不 punycode, 原样保留);
--   DEP0170 非法端口告警不发 (行为同款: 冒号后非数字段并入 pathname);
--   NBSP/ZWNBSP 不计空白 (多字节, 字节级不可辨)。

local qs = require("querystring")

local M = {}

-- JS 真假: 字符串/表真; nil/false/''/0/NaN 假
local function jf(v)
    if v == nil or v == false or v == "" then return false end
    if type(v) == "number" then return v == v and v ~= 0 end
    return true
end

local FIELDS = {
    "protocol", "slashes", "auth", "host", "port", "hostname",
    "hash", "search", "query", "pathname", "path", "href",
}

local function copy_fields(src, dst)
    for _, k in ipairs(FIELDS) do
        dst[k] = src[k]
    end
end

-- JS ''.split('/') → ['']; Lua 版保持一致
local function jsplit(s, sep)
    local out, pos = {}, 1
    while true do
        local f = s:find(sep, pos, true)
        if not f then
            out[#out + 1] = s:sub(pos)
            break
        end
        out[#out + 1] = s:sub(pos, f - 1)
        pos = f + #sep
    end
    return out
end

-- 协议表 (lowerProto 形态含尾冒号, 去掉后查)
local slashed = {
    http = true, https = true, ftp = true, gopher = true,
    file = true, ws = true, wss = true,
}
local hostless = { javascript = true }
local unsafe = { javascript = true }

local function proto_name(p)
    if type(p) ~= "string" then return nil end
    local n = #p
    if n > 0 and p:sub(n, n) == ":" then return p:sub(1, n - 1) end
    return p
end

local is_ws_code = {
    [9] = true, [10] = true, [11] = true, [12] = true, [13] = true, [32] = true,
}

-- autoEscapeStr: RFC 2396 的 delim/unwise 与单引号强制转义
local auto_escape = {
    [9] = "%09", [10] = "%0A", [13] = "%0D", [32] = "%20", [34] = "%22",
    [39] = "%27", [60] = "%3C", [62] = "%3E", [92] = "%5C", [94] = "%5E",
    [96] = "%60", [123] = "%7B", [124] = "%7C", [125] = "%7D",
}

local function auto_escape_str(rest)
    local out, last = "", 0
    for i = 0, #rest - 1 do
        local esc = auto_escape[rest:byte(i + 1)]
        if esc then
            if i > last then out = out .. rest:sub(last + 1, i) end
            out = out .. esc
            last = i + 1
        end
    end
    if last == 0 then return rest end
    if last < #rest then out = out .. rest:sub(last + 1) end
    return out
end

local forbidden = {
    [0] = true, [9] = true, [10] = true, [13] = true, [32] = true, [35] = true,
    [37] = true, [47] = true, [58] = true, [60] = true, [62] = true, [63] = true,
    [64] = true, [91] = true, [92] = true, [93] = true, [94] = true, [124] = true,
}
local forbidden6 = {} -- IPv6 放宽 '[', ']', ':'
for b, _ in pairs(forbidden) do
    if b ~= 58 and b ~= 91 and b ~= 93 then
        forbidden6[b] = true
    end
end

local function has_forbidden(s, set)
    for i = 1, #s do
        if set[s:byte(i)] then return true end
    end
    return false
end

-- getHostname: 主机名里的 / \ # ? : 切回 pathname 前缀
local function get_hostname(self, rest, hostname)
    for i = 0, #hostname - 1 do
        local code = hostname:byte(i + 1)
        local valid = code ~= 47 and code ~= 92 and code ~= 35
            and code ~= 63 and code ~= 58
        if not valid then
            self.hostname = hostname:sub(1, i)
            return "/" .. hostname:sub(i + 1) .. rest
        end
    end
    return rest
end

-- parseHost: 末段冒号数字收为 port
local function parse_host(self)
    local host = self.host
    local port = host and host:match(":[0-9]*$")
    if port then
        if port ~= ":" then self.port = port:sub(2) end
        host = host:sub(1, #host - #port)
    end
    if host ~= "" then self.hostname = host end
end

-- simplePathPattern: ^(\/\/?(?!\/)[^?\s]*)(\?[^\s]*)?$
-- 返回 path 与 search (可 nil), 或 nil 表示不匹配
local function simple_path(rest)
    if rest == "" then return nil end
    local q = rest:find("?", 1, true)
    local path, search
    if q then
        path = rest:sub(1, q - 1)
        search = rest:sub(q)
    else
        path = rest
    end
    for i = 1, #rest do
        if is_ws_code[rest:byte(i)] then return nil end -- [^?\s]* 禁空白
    end
    if path:sub(1, 1) ~= "/" then return nil end
    if path:sub(2, 2) == "/" and path:sub(3, 3) == "/" then return nil end
    return path, search
end

local function require_str(v, what, fn)
    if type(v) ~= "string" then
        error(("url.%s: %s must be a string, got %s"):format(fn, what, type(v)), 0)
    end
end

local format_tbl -- 前置声明, 实现见下

function M.parse(url, parse_qs, slashes_denote)
    require_str(url, "url", "parse")

    -- 前后空白裁剪 + 问号/井号前的反斜杠转正斜杠 (与 Chrome/IE/Opera 同款)
    local has_hash, has_at = false, false
    local start, endp = -1, -1
    local rest = ""
    local last_pos = 0
    local in_ws, split = false, false
    local len = #url
    for i = 0, len - 1 do
        local code = url:byte(i + 1)
        local is_ws = code < 33 or is_ws_code[code]
        local skip
        if start == -1 then
            if is_ws then
                skip = true
            else
                last_pos, start = i, i
            end
        elseif in_ws then
            if not is_ws then
                endp, in_ws = -1, false
            end
        elseif is_ws then
            endp, in_ws = i, true
        end
        if not skip then
            if not split then
                if code == 64 then
                    has_at = true
                elseif code == 35 then
                    has_hash, split = true, true
                elseif code == 63 then
                    split = true
                elseif code == 92 then
                    if i - last_pos > 0 then
                        rest = rest .. url:sub(last_pos + 1, i)
                    end
                    rest = rest .. "/"
                    last_pos = i + 1
                end
            elseif not has_hash and code == 35 then
                has_hash = true
            end
        end
    end

    if start ~= -1 then
        if last_pos == start then
            if endp == -1 then
                if start == 0 then
                    rest = url
                else
                    rest = url:sub(start + 1)
                end
            else
                rest = url:sub(start + 1, endp)
            end
        elseif endp == -1 and last_pos < len then
            rest = rest .. url:sub(last_pos + 1)
        elseif endp ~= -1 and last_pos < endp then
            rest = rest .. url:sub(last_pos + 1, endp)
        end
    end

    -- 简单路径快路径: 无协议/无 @ 无 # 时的纯路径或 //host 形式
    if not slashes_denote and not has_hash and not has_at then
        local p, s = simple_path(rest)
        if p then
            local out = { path = rest, href = rest, pathname = p }
            if s then
                out.search = s
                if parse_qs then
                    out.query = qs.parse(s:sub(2))
                else
                    out.query = s:sub(2)
                end
            elseif parse_qs then
                out.search = nil
                out.query = {}
            end
            return out
        end
    end

    local result = {}
    local proto = rest:match("^[a-zA-Z0-9.+-]+:")
    local lower_proto
    if proto then
        lower_proto = proto:lower()
        result.protocol = lower_proto
        rest = rest:sub(#proto + 1)
    end

    -- 有无 host: user@server 一律按主机名, //foo/bar 也按浏览器口径
    local slashes
    if slashes_denote or proto or rest:match("^//[^@/]+@[^@/]+") then
        slashes = rest:byte(1) == 47 and rest:byte(2) == 47
        if slashes and not (lower_proto and hostless[proto_name(lower_proto)]) then
            rest = rest:sub(3)
            result.slashes = true
        end
    end

    local hostless_p = lower_proto and hostless[proto_name(lower_proto)]
    local slashed_p = lower_proto and slashed[proto_name(lower_proto)]
    if not hostless_p and (slashes or (proto and not slashed_p)) then
        -- 主机扫描: tab/换行剔除, 非法字符定位, '/' '?' '#' 结束, '@' 归认证
        local host_end, at_sign, non_host = -1, -1, -1
        local i = 0
        while i < #rest do
            local code = rest:byte(i + 1)
            if code == 9 or code == 10 or code == 13 then
                rest = rest:sub(1, i) .. rest:sub(i + 2)
            elseif code == 32 or code == 34 or code == 37 or code == 39
                or code == 59 or code == 60 or code == 62 or code == 92
                or code == 94 or code == 96 or code == 123 or code == 124
                or code == 125 then
                if non_host == -1 then non_host = i end
            elseif code == 35 or code == 47 or code == 63 then
                if non_host == -1 then non_host = i end
                host_end = i
            elseif code == 64 then
                at_sign = i
                non_host = -1
            end
            if host_end ~= -1 then break end
            i = i + 1
        end

        local s0 = 0
        if at_sign ~= -1 then
            result.auth = qs.unescape(rest:sub(1, at_sign))
            s0 = at_sign + 1
        end
        if non_host == -1 then
            result.host = rest:sub(s0 + 1)
            rest = ""
        else
            result.host = rest:sub(s0 + 1, non_host)
            rest = rest:sub(non_host + 1)
        end

        parse_host(result)
        if type(result.hostname) ~= "string" then result.hostname = "" end
        local hostname = result.hostname
        local ipv6 = hostname:sub(1, 1) == "[" and hostname:sub(-1) == "]"

        if not ipv6 then
            rest = get_hostname(result, rest, hostname)
        end

        if #result.hostname > 255 then
            result.hostname = ""
        else
            result.hostname = result.hostname:lower()
        end

        if result.hostname ~= "" then
            if ipv6 then
                if has_forbidden(result.hostname, forbidden6) then
                    error("Invalid URL: " .. url, 0)
                end
            else
                -- IDNA toASCII 未做: 非 ASCII 主机名原样保留 (账记偏差)
                if has_forbidden(result.hostname, forbidden) then
                    error("Invalid URL: " .. url, 0)
                end
            end
        end

        local pport = result.port and (":" .. result.port) or ""
        result.host = (result.hostname or "") .. pport
        if ipv6 then
            result.hostname = result.hostname:sub(2, -2)
            if rest:sub(1, 1) ~= "/" then rest = "/" .. rest end
        end
    end

    -- post-host: 未安全协议先自动转义 delim/unwise 与单引号
    if not (lower_proto and unsafe[proto_name(lower_proto)]) then
        rest = auto_escape_str(rest)
    end

    local question_idx, hash_idx = -1, -1
    for i = 0, #rest - 1 do
        local code = rest:byte(i + 1)
        if code == 35 then
            result.hash = rest:sub(i + 1)
            hash_idx = i
            break
        elseif code == 63 and question_idx == -1 then
            question_idx = i
        end
    end

    if question_idx ~= -1 then
        if hash_idx == -1 then
            result.search = rest:sub(question_idx + 1)
            result.query = rest:sub(question_idx + 2)
        else
            result.search = rest:sub(question_idx + 1, hash_idx)
            result.query = rest:sub(question_idx + 2, hash_idx)
        end
        if parse_qs then
            result.query = qs.parse(result.query)
        end
    elseif parse_qs then
        result.search = nil
        result.query = {}
    end

    local use_q = question_idx ~= -1 and (hash_idx == -1 or question_idx < hash_idx)
    local first_idx = use_q and question_idx or hash_idx
    if first_idx == -1 then
        if #rest > 0 then result.pathname = rest end
    elseif first_idx > 0 then
        result.pathname = rest:sub(1, first_idx)
    end
    if slashed_p and jf(result.hostname) and not jf(result.pathname) then
        result.pathname = "/"
    end

    if jf(result.pathname) or jf(result.search) then
        result.path = (result.pathname or "") .. (result.search or "")
    end

    result.href = format_tbl(result)
    return result
end

function format_tbl(url_object)
    if type(url_object) == "string" then
        url_object = M.parse(url_object)
    elseif type(url_object) ~= "table" then
        error(("url.format: expects a table or string, got %s"):format(type(url_object)), 0)
    end

    local auth = url_object.auth or ""
    if auth ~= "" then
        -- Node noEscapeAuth: 同 qs.escape 不转义集但 ':' 放行
        -- (净效果: 先 qs.escape 再还原首个 %3A/%3a, 对拍 v24.21.0)
        auth = qs.escape(auth):gsub("%%3[Aa]", ":", 1) .. "@"
    end

    local protocol = url_object.protocol or ""
    if protocol ~= "" and protocol:sub(-1) ~= ":" then
        protocol = protocol .. ":"
    end

    local pathname = url_object.pathname or ""
    local hash = url_object.hash or ""
    local host = ""
    local query = ""

    if jf(url_object.host) then
        host = auth .. url_object.host
    elseif jf(url_object.hostname) then
        local h = url_object.hostname
        if h:find(":", 1, true) and not (h:sub(1, 1) == "[" and h:sub(-1) == "]") then
            h = "[" .. h .. "]"
        end
        host = auth .. h
        if jf(url_object.port) then
            host = host .. ":" .. url_object.port
        end
    end

    if type(url_object.query) == "table" then
        query = qs.stringify(url_object.query)
    end
    local search = url_object.search or ""
    if search == "" and query ~= "" then
        search = "?" .. query
    end

    if pathname:find("#", 1, true) or pathname:find("?", 1, true) then
        local new_path, last = "", 0
        for i = 0, #pathname - 1 do
            local code = pathname:byte(i + 1)
            if code == 35 or code == 63 then
                if i > last then
                    new_path = new_path .. pathname:sub(last + 1, i)
                end
                new_path = new_path .. (code == 35 and "%23" or "%3F")
                last = i + 1
            end
        end
        if last < #pathname then
            new_path = new_path .. pathname:sub(last + 1)
        end
        pathname = new_path
    end

    -- 只有带斜杠的协议才有 //, mailto:/xmpp: 等除非原本就有
    local slashed_p = protocol ~= "" and slashed[proto_name(protocol)]
    if jf(url_object.slashes) or slashed_p then
        if jf(url_object.slashes) or host ~= "" then
            if pathname ~= "" and pathname:byte(1) ~= 47 then
                pathname = "/" .. pathname
            end
            host = "//" .. host
        elseif protocol:sub(1, 4) == "file" then
            host = "//"
        end
    end

    if search:find("#", 1, true) then
        search = search:gsub("#", "%%%23")
    end
    if hash ~= "" and hash:byte(1) ~= 35 then
        hash = "#" .. hash
    end
    if search ~= "" and search:byte(1) ~= 63 then
        search = "?" .. search
    end

    return protocol .. host .. pathname .. search .. hash
end

local function resolve_object(self, relative)
    if type(relative) == "string" then
        relative = M.parse(relative, false, true)
    end

    local result = {}
    copy_fields(self, result)

    -- hash 无条件覆盖 (哪怕 href 为空)
    result.hash = relative.hash

    if relative.href == "" then
        result.href = format_tbl(result)
        return result
    end

    -- //foo/bar 一律截到协议
    if jf(relative.slashes) and not relative.protocol then
        for _, k in ipairs(FIELDS) do
            if k ~= "protocol" then result[k] = relative[k] end
        end
        if jf(result.protocol) and slashed[proto_name(result.protocol)]
            and jf(result.hostname) and not jf(result.pathname) then
            result.path = "/"
            result.pathname = "/"
        end
        result.href = format_tbl(result)
        return result
    end

    if relative.protocol and relative.protocol ~= result.protocol then
        -- 换协议: 非带斜杠协议整体替换
        if not slashed[proto_name(relative.protocol)] then
            for _, k in ipairs(FIELDS) do result[k] = relative[k] end
            result.href = format_tbl(result)
            return result
        end
        result.protocol = relative.protocol
        if not jf(relative.host)
            and not relative.protocol:match("^file:?$")
            and not hostless[proto_name(relative.protocol)] then
            local rel_path = jsplit(relative.pathname or "", "/")
            while #rel_path > 0 do
                relative.host = table.remove(rel_path, 1)
                if jf(relative.host) then break end
            end
            if not jf(relative.host) then relative.host = "" end
            if not jf(relative.hostname) then relative.hostname = "" end
            if rel_path[1] ~= "" then table.insert(rel_path, 1, "") end
            if #rel_path < 2 then table.insert(rel_path, 1, "") end
            result.pathname = table.concat(rel_path, "/")
        else
            result.pathname = relative.pathname
        end
        result.search = relative.search
        result.query = relative.query
        result.host = jf(relative.host) and relative.host or ""
        result.auth = relative.auth
        result.hostname = jf(relative.hostname) and relative.hostname
            or (jf(relative.host) and relative.host)
        result.port = relative.port
        if jf(result.pathname) or jf(result.search) then
            result.path = (result.pathname or "") .. (result.search or "")
        end
        if not result.slashes then result.slashes = relative.slashes end
        result.href = format_tbl(result)
        return result
    end

    local is_source_abs = jf(result.pathname) and result.pathname:sub(1, 1) == "/"
    local is_rel_abs = jf(relative.host)
        or (jf(relative.pathname) and relative.pathname:sub(1, 1) == "/")
    local must_end_abs = is_rel_abs or is_source_abs
        or (jf(result.host) and jf(relative.pathname))
    local remove_all_dots = must_end_abs
    local src_path = jf(result.pathname) and jsplit(result.pathname, "/") or {}
    local rel_path = jf(relative.pathname) and jsplit(relative.pathname, "/") or {}
    local no_leading = jf(result.protocol) and not slashed[proto_name(result.protocol)]

    -- 非带斜杠协议: 把首段路径挪进 host, 让 ../ 能一路爬到主机名
    if no_leading then
        result.hostname = ""
        result.port = nil
        if jf(result.host) then
            if src_path[1] == "" then
                src_path[1] = result.host
            else
                table.insert(src_path, 1, result.host)
            end
        end
        result.host = ""
        if jf(relative.protocol) then
            relative.hostname = nil
            relative.port = nil
            result.auth = nil
            if jf(relative.host) then
                if rel_path[1] == "" then
                    rel_path[1] = relative.host
                else
                    table.insert(rel_path, 1, relative.host)
                end
            end
            relative.host = nil
        end
        if must_end_abs then
            must_end_abs = rel_path[1] == "" or src_path[1] == ""
        end
    end

    if is_rel_abs then
        -- 绝对: 主机/端口/搜索一并覆盖
        if jf(relative.host) or relative.host == "" then
            if result.host ~= relative.host then result.auth = nil end
            result.host = relative.host
            result.port = relative.port
        end
        if jf(relative.hostname) or relative.hostname == "" then
            if result.hostname ~= relative.hostname then result.auth = nil end
            result.hostname = relative.hostname
        end
        result.search = relative.search
        result.query = relative.query
        src_path = rel_path
    elseif #rel_path > 0 then
        -- 相对: 丢掉现有末段, 换上新路径
        if not next(src_path) then src_path = {} end
        table.remove(src_path)
        for _, v in ipairs(rel_path) do
            src_path[#src_path + 1] = v
        end
        result.search = relative.search
        result.query = relative.query
    elseif relative.search ~= nil then
        -- 只换搜索 (href='?foo' 一类)
        if no_leading then
            result.hostname = table.remove(src_path, 1)
            result.host = result.hostname
            local at = result.host and result.host:find("@", 1, true) or nil
            if jf(result.host) and at and at > 1 then
                local parts = jsplit(result.host, "@")
                result.auth = table.remove(parts, 1)
                result.host = table.remove(parts, 1)
                result.hostname = result.host
            end
        end
        result.search = relative.search
        result.query = relative.query
        if result.pathname ~= nil or result.search ~= nil then
            result.path = (jf(result.pathname) and result.pathname or "")
                .. (jf(result.search) and result.search or "")
        end
        result.href = format_tbl(result)
        return result
    end

    if #src_path == 0 then
        -- 完全没有路径
        result.pathname = nil
        result.path = jf(result.search) and ("/" .. result.search) or nil
        result.href = format_tbl(result)
        return result
    end

    -- 结尾是 . 或 .. 时补尾斜杠; 以空段结尾同样补
    local last = src_path[#src_path]
    local has_trailing = ((jf(result.host) or jf(relative.host) or #src_path > 1)
        and (last == "." or last == "..")) or last == ""

    -- 单点剔除, 双点回退到父级 (越根则记下 up)
    local up = 0
    for idx = #src_path - 1, 0, -1 do
        local seg = src_path[idx + 1]
        if seg == "." then
            table.remove(src_path, idx + 1)
        elseif seg == ".." then
            table.remove(src_path, idx + 1)
            up = up + 1
        elseif up > 0 then
            table.remove(src_path, idx + 1)
            up = up - 1
        end
    end

    if not must_end_abs and not remove_all_dots then
        while up > 0 do
            table.insert(src_path, 1, "..")
            up = up - 1
        end
    end

    if must_end_abs and src_path[1] ~= ""
        and (src_path[1] == nil or src_path[1]:sub(1, 1) ~= "/") then
        table.insert(src_path, 1, "")
    end

    if has_trailing and (table.concat(src_path, "/"):sub(-1) ~= "/") then
        table.insert(src_path, "")
    end

    local is_absolute = src_path[1] == ""
        or (jf(src_path[1]) and src_path[1]:sub(1, 1) == "/")

    -- 非带斜杠协议: 把 host 放回去
    if no_leading then
        local h
        if is_absolute then
            h = ""
        elseif #src_path > 0 then
            h = table.remove(src_path, 1)
        else
            h = ""
        end
        result.hostname = h
        result.host = h
        local at = result.host and result.host:find("@", 1, true) or nil
        if jf(result.host) and at and at > 1 then
            local parts = jsplit(result.host, "@")
            result.auth = table.remove(parts, 1)
            result.host = table.remove(parts, 1)
            result.hostname = result.host
        end
    end

    if not must_end_abs then
        must_end_abs = jf(result.host) and #src_path > 0
    end
    if must_end_abs and not is_absolute then
        table.insert(src_path, 1, "")
    end

    if #src_path == 0 then
        result.pathname = nil
        result.path = nil
    else
        result.pathname = table.concat(src_path, "/")
    end

    if result.pathname ~= nil or result.search ~= nil then
        result.path = (jf(result.pathname) and result.pathname or "")
            .. (jf(result.search) and result.search or "")
    end
    result.auth = jf(relative.auth) and relative.auth or result.auth
    if not result.slashes then result.slashes = relative.slashes end
    result.href = format_tbl(result)
    return result
end

M.format = format_tbl

function M.resolve(source, relative)
    require_str(source, "source", "resolve")
    require_str(relative, "relative", "resolve")
    return resolve_object(M.parse(source, false, true), relative).href
end

function M.resolveObject(source, relative)
    if not jf(source) then return relative end
    require_str(source, "source", "resolveObject")
    return resolve_object(M.parse(source, false, true), relative)
end

-- JS format 的 URL 实例分支 (bindingUrl.format) 未立项
return M
