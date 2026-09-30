-- toml: TOML 1.0 decode/encode.
--
-- decode is a passthrough to the C face (toml.core over tomlc17,
-- deps/tomlc17 pinned at R260821; registered by luna_main). Values map
-- plainly to Lua, and datetimes decode to component tables — a date is
-- {year, month, day}, a time {hour, minute, second, secfrac?}, a
-- datetime both, an offset datetime additionally offset (minutes from
-- UTC, 0 for Z). secfrac is a fraction of a second and appears only
-- when the source had sub-second digits: tomlc17 stores whole
-- microseconds, so "07:32:00.0" and "07:32:00" decode identically.
--
-- encode is this file's own Lua face (tomlc17 ships no encoder):
--   - every table emits its scalar keys first and its subtables after:
--     TOML requires a table's key=value lines to precede its [sub]
--     headers, and reordering here makes that rule unbreakable;
--   - an array whose elements are all plain tables becomes [[header]]
--     entries; anything else renders inline ([1, 2], {a = 1});
--   - a table whose keys all come from the datetime component set and
--     that has a year (date part) or an hour (time part) encodes as a
--     datetime — ordinary data tables steer clear by carrying any
--     other key.
--
-- Error contract shared with csv/ini/json (docs/node-parity.md):
-- decode/encode return nil, err on bad data; bad *arguments* raise.
-- The message shape is "toml: <reason> at line N" — no column clause:
-- tomlc17 reports line numbers only (guide/modules.md).
local core = require "toml.core"

local decode = core.decode

-- ---- datetime component tables -----------------------------------------

local TS_KEY = {
    year = true, month = true, day = true,
    hour = true, minute = true, second = true, secfrac = true, offset = true,
}

-- "date" | "time" | "datetime" | nil (nil: an ordinary table)
local function datetime_shape(t)
    local has_date, has_time, n = false, false, 0
    for k in pairs(t) do
        if not TS_KEY[k] then
            return nil
        end
        n = n + 1
        if k == "year" then
            has_date = true
        elseif k == "hour" then
            has_time = true
        end
    end
    if has_date and has_time then
        return "datetime"
    elseif has_date then
        return "date"
    elseif has_time then
        return "time"
    end
    return nil
end

local function is_int(v, lo, hi)
    return math.type(v) == "integer" and v >= lo and v <= hi
end

local function fmt_frac(f)
    -- tomlc17 keeps whole microseconds, so 6 decimals round-trip exactly
    local s = ("%.6f"):format(f):sub(2):gsub("0+$", "")
    return s ~= "." and s or ""
end

-- component table -> RFC 3339 spelling (nil, err when malformed)
local function fmt_datetime(v, shape)
    local function bad(why)
        return nil, ("toml: datetime table is malformed (%s)"):format(why)
    end
    if shape == "date" or shape == "datetime" then
        if not (is_int(v.year, 0, 9999) and is_int(v.month, 1, 12)
            and is_int(v.day, 1, 31)) then
            return bad("year/month/day must be integers in range")
        end
    end
    if shape == "time" or shape == "datetime" then
        if not (is_int(v.hour, 0, 23) and is_int(v.minute, 0, 59)
            and is_int(v.second, 0, 59)) then
            return bad("hour/minute/second must be integers in range")
        end
        if v.secfrac ~= nil and (type(v.secfrac) ~= "number"
            or v.secfrac < 0 or v.secfrac >= 1) then
            return bad("secfrac must be a fraction in [0, 1)")
        end
    end
    if v.offset ~= nil and shape ~= "datetime" then
        return bad("offset is only valid on a datetime")
    end
    if shape == "datetime" and v.offset ~= nil
        and not is_int(v.offset, -23 * 60 + 1, 23 * 60 - 1) then
        return bad("offset must be minutes within +/-23:59")
    end

    local out = ""
    if shape == "date" or shape == "datetime" then
        out = ("%04d-%02d-%02d"):format(v.year, v.month, v.day)
    end
    if shape == "time" or shape == "datetime" then
        local t = ("%02d:%02d:%02d"):format(v.hour, v.minute, v.second)
            .. (v.secfrac and fmt_frac(v.secfrac) or "")
        out = shape == "datetime" and (out .. "T" .. t) or t
    end
    if shape == "datetime" and v.offset ~= nil then
        if v.offset == 0 then
            out = out .. "Z"
        else
            local m = v.offset < 0 and -v.offset or v.offset
            out = out .. ("%s%02d:%02d"):format(v.offset < 0 and "-" or "+",
                math.floor(m / 60), m % 60)
        end
    end
    return out
end

-- ---- scalars -------------------------------------------------------------

local ESCAPE = {
    ['"'] = '\\"', ["\\"] = "\\\\", ["\b"] = "\\b", ["\t"] = "\\t",
    ["\n"] = "\\n", ["\f"] = "\\f", ["\r"] = "\\r",
}

local function fmt_string(s)
    -- %c covers 0x00-0x1f; DEL (0x7f) is unprintable in TOML strings too
    return '"' .. s:gsub('[%c\127"\\]', function(c)
        return ESCAPE[c] or ("\\u%04X"):format(c:byte())
    end) .. '"'
end

local function fmt_float(v)
    if v ~= v then
        return "nan"
    elseif v == math.huge then
        return "inf"
    elseif v == -math.huge then
        return "-inf"
    end
    -- shortest spelling that reads back as the exact same double
    -- (Lua's tostring uses %.14g, which silently loses round-trips)
    local s
    for _, p in ipairs { "%.14g", "%.15g", "%.16g", "%.17g" } do
        s = p:format(v)
        if tonumber(s) == v then
            break
        end
    end
    if not s:find "[.eE]" then
        s = s .. ".0" -- "1" would read back as an integer
    end
    return s
end

local function fmt_key(k)
    if type(k) ~= "string" then
        return nil
    end
    if k:match "^[A-Za-z0-9_-]+$" then
        return k
    end
    return fmt_string(k)
end

-- dense-array check: all keys are 1..n with no holes. Returns n, or nil
-- when the table is not a dense array (maps, sparse, mixed shapes).
local function array_len(t)
    local count, max = 0, 0
    for k in pairs(t) do
        if math.type(k) ~= "integer" or k < 1 then
            return nil
        end
        count = count + 1
        if k > max then
            max = k
        end
    end
    return count == max and max or nil
end

-- ---- inline rendering (scalars, arrays, inline tables) -------------------

local inline_value

local function fmt_inline_table(t, anc)
    anc[t] = true
    local parts = {}
    for k, v in pairs(t) do
        local fk = fmt_key(k)
        if not fk then
            anc[t] = nil
            return nil, ("toml: key must be a string (got %s)"):format(type(k))
        end
        local s, err = inline_value(v, anc)
        if s == nil then
            anc[t] = nil
            return nil, err
        end
        parts[#parts + 1] = ("%s = %s"):format(fk, s)
    end
    anc[t] = nil
    return "{ " .. table.concat(parts, ", ") .. " }"
end

inline_value = function(v, anc)
    local t = type(v)
    if t == "string" then
        return fmt_string(v)
    elseif t == "boolean" then
        return tostring(v)
    elseif t == "number" then
        if math.type(v) == "integer" then
            return tostring(v)
        end
        return fmt_float(v)
    elseif t ~= "table" then
        return nil, ("toml: unsupported value type %s"):format(t)
    end
    local shape = datetime_shape(v)
    if shape then
        return fmt_datetime(v, shape)
    end
    if anc[v] then
        return nil, "toml: cyclic table reference"
    end
    if next(v) == nil then
        return "{}"
    end
    local n = array_len(v)
    if n then
        local parts = {}
        for i = 1, n do
            local s, err = inline_value(v[i], anc)
            if s == nil then
                return nil, err
            end
            parts[#parts + 1] = s
        end
        return "[" .. table.concat(parts, ", ") .. "]"
    end
    return fmt_inline_table(v, anc)
end

-- ---- section rendering ----------------------------------------------------

-- a dense array of plain (non-datetime) tables becomes [[header]]
-- entries; everything else inlines
local function is_array_of_tables(v)
    local n = array_len(v)
    if not n or n == 0 then
        return false
    end
    for i = 1, n do
        if type(v[i]) ~= "table" or datetime_shape(v[i]) then
            return false
        end
    end
    return true
end

-- "section" (own [header] body) | "aot" ([[header]] per element) |
-- "scalar" (renders inline at the parent's key=value spot)
local function classify(v)
    if type(v) ~= "table" then
        return "scalar"
    end
    if datetime_shape(v) then
        return "scalar"
    end
    if next(v) == nil then
        return "scalar" -- {} inline
    end
    if array_len(v) == nil then
        return "section"
    end
    if is_array_of_tables(v) then
        return "aot"
    end
    return "scalar" -- inline array
end

-- path + one key, as a fresh dense array. NOT {table.unpack(path), fk}:
-- a call not in the constructor's last position is adjusted to a single
-- value, and table.unpack({}) adjusted to one is a bare nil — the child
-- path would become {nil, fk} and its # would collapse to 0, flattening
-- nested headers ([sub.deep] into [deep]).
local function child_path(path, fk)
    local c = {}
    for i = 1, #path do
        c[i] = path[i]
    end
    c[#path + 1] = fk
    return c
end

local function encode(root)
    if type(root) ~= "table" then
        error("toml: encode expects a table", 2)
    end
    local parts = {}
    local function add(line)
        parts[#parts + 1] = line
    end

    -- emits t's body; header (if any) was already added by the caller.
    -- path is the formatted key path for error context and child headers.
    local function emit_table(t, path, anc)
        if anc[t] then
            return nil, "toml: cyclic table reference"
        end
        anc[t] = true
        -- partition first, then emit: TOML requires all of a table's
        -- key=value lines to come before any of its [sub] headers, and
        -- pairs order alone cannot promise that
        local scalars, subs = {}, {}
        for k, v in pairs(t) do
            local fk = fmt_key(k)
            if not fk then
                anc[t] = nil
                return nil, ("toml: key must be a string (got %s)"):format(type(k))
            end
            if classify(v) == "scalar" then
                scalars[#scalars + 1] = { fk, v }
            else
                subs[#subs + 1] = { fk, v }
            end
        end
        for _, kv in ipairs(scalars) do
            local s, err = inline_value(kv[2], anc)
            if s == nil then
                anc[t] = nil
                return nil, err
            end
            add(("%s = %s"):format(kv[1], s))
        end
        for _, sub in ipairs(subs) do
            local fk, v = sub[1], sub[2]
            local keypath = #path > 0 and (table.concat(path, ".") .. "." .. fk) or fk
            if is_array_of_tables(v) then
                local n = array_len(v)
                for i = 1, n do
                    if #parts > 0 then
                        add("")
                    end
                    add(("[[%s]]"):format(keypath))
                    local ok, err = emit_table(v[i], child_path(path, fk), anc)
                    if not ok then
                        anc[t] = nil
                        return nil, err
                    end
                end
            else
                if #parts > 0 then
                    add("")
                end
                add(("[%s]"):format(keypath))
                local ok, err = emit_table(v, child_path(path, fk), anc)
                if not ok then
                    anc[t] = nil
                    return nil, err
                end
            end
        end
        anc[t] = nil
        return true
    end

    local ok, err = emit_table(root, {}, {})
    if not ok then
        return nil, err
    end
    return table.concat(parts, "\n")
end

return {
    decode = decode,
    encode = encode,
}
