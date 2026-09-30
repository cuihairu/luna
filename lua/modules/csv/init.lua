-- csv: RFC 4180-style CSV decode/encode.
--
-- LPeg grammar (lpeg ships with luna — zero new dependencies). Fields are
-- always strings: CSV has no types, conversion is the caller's business
-- (Node csv-parse default). EOLs accepted on decode are CRLF and LF; a
-- lone CR is rejected. Fields are not trimmed — whitespace is data.
--
-- Error contract shared with json (docs/node-parity.md): decode/encode
-- return nil, err instead of raising on bad data; the message carries
-- "<reason> at line N, column M" (columns count UTF-8 characters, bytes
-- when the prefix is not valid UTF-8). Bad *arguments* raise. lines() is
-- an iterator — nil means "done" — so it raises on malformed input;
-- pcall it when feeding untrusted data. encode joins rows with opts.eol
-- (default CRLF, the RFC 4180 wire format) and appends no trailing eol.
local lpeg = require "lpeg"

local P, S, Cs, Ct, Cp, Cmt = lpeg.P, lpeg.S, lpeg.Cs, lpeg.Ct, lpeg.Cp, lpeg.Cmt

-- byte offset -> "line L, column C" (inlined per the batch-2 decision:
-- the C-backed format modules get positions from their libraries, so
-- there is nothing to share beyond csv/ini yet)
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
    error(("csv: %s at %s"):format(why, where(s, pos)), 0)
end

local NL = P"\r\n" + P"\n"

-- grammars are cached per delimiter: doc decodes whole texts, step walks
-- one record for the lines() iterator
local cache = {}

local function grammar(delim)
    local hit = cache[delim]
    if hit then
        return hit
    end
    local d = P(delim)
    local q = P('"')

    -- error trampolines are Cmt(P(0), ...): the callback runs at the
    -- current match position (Cmt passes the position AFTER its pattern
    -- matches, and P(0) consumes nothing), so p is exactly where the
    -- offending character sits / where the token was expected
    local unterm = Cmt(P(0), function(s, p)
        fail(s, p, "unterminated quoted field")
    end)
    local qchar = (P(1) - q) + ((q * q) / '"')
    local qfield = q * Cs(qchar^0) * (q + unterm)
    local field = qfield + Cs((P(1) - q - d - S"\r\n")^0)

    local record = Ct(field * (d * field)^0)
    local junk = Cmt(P(0), function(s, p)
        fail(s, p, "unexpected character after field")
    end)

    local g = {}
    -- a record after a line break only starts when more input follows it:
    -- a trailing newline must not produce a phantom empty record
    g.doc = Ct(record * (NL * #(P(1)) * record)^0) * NL^-1 * (-1 + junk)
    g.step = record * Cp() * (NL + P(-1) + junk) * Cp()
    cache[delim] = g
    return g
end

local function check_delim(delim)
    if type(delim) ~= "string" or #delim ~= 1
        or delim == '"' or delim == "\r" or delim == "\n" then
        error("csv: delimiter must be one character, not '\"', CR or LF", 3)
    end
end

local function decode(s, opts)
    if type(s) ~= "string" then
        error("csv: decode expects a string", 2)
    end
    opts = opts or {}
    local delim = opts.delimiter or ","
    check_delim(delim)
    if s == "" then
        return {}
    end
    local ok, rows = pcall(function()
        return grammar(delim).doc:match(s)
    end)
    if not ok then
        return nil, rows
    end
    if opts.headers then
        local names = table.remove(rows, 1) or {}
        local out = {}
        for i, row in ipairs(rows) do
            local rec = {}
            for c = 1, #names do
                rec[names[c]] = row[c]
            end
            for c = #names + 1, #row do
                rec[c - #names] = row[c] -- extra columns keep positional keys
            end
            out[i] = rec
        end
        rows = out
    end
    return rows
end

local function needs_quote(v, delim)
    return v:find('"', 1, true) ~= nil
        or v:find("\r", 1, true) ~= nil
        or v:find("\n", 1, true) ~= nil
        or v:find(delim, 1, true) ~= nil
end

local function encode(rows, opts)
    if type(rows) ~= "table" then
        error("csv: encode expects a table of rows", 2)
    end
    opts = opts or {}
    local delim = opts.delimiter or ","
    check_delim(delim)
    local eol = opts.eol or "\r\n" -- RFC 4180 wire format; set "\n" for logs
    if type(eol) ~= "string" or eol == "" then
        error("csv: eol must be a non-empty string", 2)
    end

    local headers = opts.headers
    if headers == true then
        error("csv: headers must be an explicit array (table keys have no reliable order)", 2)
    end

    local out, n = {}, 0
    local function emit_row(row, r)
        if type(row) ~= "table" then
            return nil, ("csv: row %d is a %s, expected a table"):format(r, type(row))
        end
        local fields = {}
        for c = 1, #row do
            local f = row[c]
            local t = type(f)
            if t == "number" or t == "boolean" then
                f = tostring(f)
            elseif t == "nil" then
                f = ""
            elseif t ~= "string" then
                return nil, ("csv: unsupported field type %s at row %d, column %d")
                    :format(t, r, c)
            end
            if needs_quote(f, delim) then
                f = '"' .. f:gsub('"', '""') .. '"'
            end
            fields[#fields + 1] = f
        end
        n = n + 1
        out[n] = table.concat(fields, delim)
        return true
    end

    local plan = rows
    if type(headers) == "table" then
        plan = { headers }
        for r = 1, #rows do
            local row = rows[r]
            -- record-shaped rows (no [1]) map through the header names;
            -- array rows pass through unchanged
            if type(row) == "table" and row[1] == nil and next(row) ~= nil then
                local rec = {}
                for c, name in ipairs(headers) do
                    rec[c] = row[name]
                end
                row = rec
            end
            plan[#plan + 1] = row
        end
    end

    for r = 1, #plan do
        local ok, err = emit_row(plan[r], r)
        if not ok then
            return nil, err
        end
    end
    return table.concat(out, eol)
end

-- csv.lines(s, opts?) -> iterator over records (arrays of fields).
-- Handles quoted fields spanning newlines; raises on malformed input.
local function lines(s, opts)
    if type(s) ~= "string" then
        error("csv: lines expects a string", 2)
    end
    local delim = (opts and opts.delimiter) or ","
    check_delim(delim)
    local g = grammar(delim)
    local pos = 1
    return function()
        if pos > #s then
            return nil
        end
        local row, _, nextpos = g.step:match(s, pos)
        pos = nextpos
        return row
    end
end

return {
    decode = decode,
    encode = encode,
    lines = lines,
}
