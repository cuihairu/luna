-- ini: classic .ini decode/encode.
--
-- LPeg grammar (lpeg ships with luna — zero new dependencies). Dialect:
-- full-line comments only (`#` or `;` after optional blank), no inline
-- comments — a value's `#` is data; `[name]` sections, repeated sections
-- merge into one table; keys before any section land in the root table;
-- values are strings unless opts.cast converts numbers and lowercase
-- true/false; `"..."` quoted values honor `\"` and `\\` escapes, other
-- backslashes are literal; duplicate keys keep the last value; keys must
-- not start with `[` nor contain `=`/newlines — such lines parse as a
-- section first, then fall back to key.
--
-- Error contract shared with csv/json (docs/node-parity.md): decode/encode
-- return nil, err instead of raising on bad data; the message carries
-- "<reason> at line N, column M". Bad *arguments* raise.
local lpeg = require "lpeg"

local P, S, R, C, Cs, Cmt = lpeg.P, lpeg.S, lpeg.R, lpeg.C, lpeg.Cs, lpeg.Cmt

-- byte offset -> "line L, column C" (same contract as csv: columns count
-- UTF-8 characters, bytes when the prefix is not valid UTF-8)
local function where(s, pos)
    pos = math.min(math.max(pos, 1), #s + 1)
    local line, bol = 1, 1
    for i = 1, pos - 1 do
        if s:sub(i, i) == "\n" then
            line = line + 1
            bol = i + 1
        end
    end
    local col = pos - bol
    local ok, chars = pcall(utf8.len, s, bol, pos - 1)
    if ok and chars then
        col = chars
    end
    return ("line %d, column %d"):format(line, col + 1)
end

local function fail(s, pos, why)
    error(("ini: %s at %s"):format(why, where(s, pos)), 0)
end

local NL = P"\r\n" + P"\n" + P"\r" -- lenient: a lone CR ends a line
local LEND = NL + P(-1)            -- line end or end of document
local REST = (P(1) - S"\r\n")^0    -- everything up to a line break
local function trim(v)
    return (v:match("^%s*(.-)%s*$"))
end

-- quoted values: \" and \\ are escapes, any other backslash is literal.
-- Error trampolines are Cmt(P(0), ...): the callback runs at the current
-- match position (Cmt passes the position AFTER its pattern matches, and
-- P(0) consumes nothing), so p is exactly where the offending character
-- sits / where the token was expected
local vchar = (P'\\' * P'"' / '"') + (P'\\' * P'\\' / '\\') + (P(1) - S'"\r\n')
local unterm = Cmt(P(0), function(s, p)
    fail(s, p, "unterminated quoted value")
end)
local quoted = P'"' * Cs(vchar^0) * (P'"' + unterm)
    * S" \t"^0
    * (LEND + Cmt(P(0), function(s, p)
        fail(s, p, "unexpected data after quoted value")
    end))
local bare = C(REST) / trim * LEND

local KEY = C((P(1) - S"=\r\n")^1) / trim
local SECNAME = C((P(1) - S"]\r\n")^1) / trim

local section = S" \t"^0 * P"[" * SECNAME
    * (P"]" + Cmt(P(0), function(s, p)
        fail(s, p, "section header missing ']'")
    end))
    * S" \t"^0
    * (LEND + Cmt(P(0), function(s, p)
        fail(s, p, "unexpected data after section header")
    end))
local keyval = S" \t"^0 * KEY * S" \t"^0 * P"=" * S" \t"^0 * (quoted + bare)
-- a blank line: whitespace then a line break — or, at end of document,
-- trailing whitespace with no final newline (S"^1 keeps the alternative
-- from matching empty: the doc grammar repeats it, and LPeg rejects loop
-- bodies that may accept the empty string)
local blank = S" \t"^1 * (NL + P(-1)) + NL
local comment = S" \t"^0 * S"#;" * REST * LEND

-- decode pass 1: the grammar only accumulates (section, pair) events into
-- acc; errors carry exact positions via the Cmt trampolines above
local function decode(s, opts)
    if type(s) ~= "string" then
        error("ini: decode expects a string", 2)
    end
    opts = opts or {}
    local acc = {}
    local section_line = Cmt(section, function(_, _, name)
        acc[#acc + 1] = { "section", name }
        return true
    end)
    local kv_line = Cmt(keyval, function(_, _, k, v)
        acc[#acc + 1] = { "pair", k, v }
        return true
    end)
    local junk = Cmt(P(0), function(s, p)
        local rest = s:sub(p):match("^%s*(%[?[^\r\n]*)") or ""
        if rest:sub(1, 1) == "[" then
            fail(s, p, "section header missing ']'")
        elseif not rest:find("=", 1, true) then
            fail(s, p, "expected '[section]' or 'key = value'")
        else
            fail(s, p, "unexpected data")
        end
    end)
    local doc = (blank + comment + section_line + kv_line)^0 * (-1 + junk)

    local ok, err = pcall(function()
        return doc:match(s)
    end)
    if not ok then
        return nil, err
    end

    -- pass 2: assemble the table
    local cast = opts.cast
    local function value(v)
        if cast then
            local n = tonumber(v)
            if n then
                return n
            elseif v == "true" then
                return true
            elseif v == "false" then
                return false
            end
        end
        return v
    end

    local root = {}
    local cur = nil
    for i = 1, #acc do
        local ev = acc[i]
        if ev[1] == "section" then
            local name = ev[2]
            local tbl = root[name]
            if type(tbl) ~= "table" then
                tbl = {}
                root[name] = tbl
            end
            cur = tbl
        else
            (cur or root)[ev[2]] = value(ev[3])
        end
    end
    return root
end

-- encode: root scalars first, then sections (one level deep — nested
-- tables inside sections are rejected). Key order follows table traversal,
-- which Lua does not specify; consumers must not rely on it.
local function check_key(k)
    if type(k) ~= "string" or k == "" then
        return "key must be a non-empty string"
    elseif k:find("=", 1, true) or k:find("\r", 1, true) or k:find("\n", 1, true) then
        return ("key %q contains '=', CR or LF"):format(k)
    elseif k:sub(1, 1) == "[" or k:match"^%s" or k:match"%s$" then
        return ("key %q starts with '[' or has surrounding whitespace"):format(k)
    end
    return nil
end

local function check_section(name)
    if type(name) ~= "string" or name == "" or name ~= trim(name) then
        return "section name must be a non-empty trimmed string"
    elseif name:find("]", 1, true) or name:find("\r", 1, true) or name:find("\n", 1, true) then
        return ("section name %q contains ']', CR or LF"):format(name)
    end
    return nil
end

local function fmt_value(v)
    local t = type(v)
    if t == "number" or t == "boolean" then
        return tostring(v)
    elseif t ~= "string" then
        return nil
    end
    -- quote iff the value would not survive a bare roundtrip
    if v:match"^%s" or v:match"%s$" or v:find('"', 1, true) or v:find("\\", 1, true)
        or v:find("\r", 1, true) or v:find("\n", 1, true) then
        return '"' .. v:gsub('[\\"]', '\\%0') .. '"'
    end
    return v
end

local function encode(root, opts)
    if type(root) ~= "table" then
        error("ini: encode expects a table", 2)
    end
    local eol = (opts and opts.eol) or "\n"
    if type(eol) ~= "string" or eol == "" then
        error("ini: eol must be a non-empty string", 2)
    end

    local parts = {}
    local function add(line)
        parts[#parts + 1] = line
    end

    for k, v in pairs(root) do
        if type(v) ~= "table" then
            local err = check_key(k)
            if err then
                return nil, ("ini: %s (got key %s)"):format(err, tostring(k))
            end
            local fv = fmt_value(v)
            if fv == nil then
                return nil, ("ini: unsupported value type %s for key %q"):format(type(v), k)
            end
            add(("%s = %s"):format(k, fv))
        end
    end
    for name, sect in pairs(root) do
        if type(sect) == "table" then
            local err = check_section(name)
            if err then
                return nil, ("ini: %s (got %s)"):format(err, tostring(name))
            end
            add(("[%s]"):format(name))
            for k, v in pairs(sect) do
                local kerr = check_key(k)
                if kerr then
                    return nil, ("ini: %s (got key %s)"):format(kerr, tostring(k))
                end
                if type(v) == "table" then
                    return nil, ("ini: nested tables are not supported (section %q, key %q)")
                        :format(name, tostring(k))
                end
                local fv = fmt_value(v)
                if fv == nil then
                    return nil, ("ini: unsupported value type %s in section %q for key %q")
                        :format(type(v), name, k)
                end
                add(("%s = %s"):format(k, fv))
            end
        end
    end
    return table.concat(parts, eol)
end

return {
    decode = decode,
    encode = encode,
}
