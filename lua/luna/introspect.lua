-- luna.introspect: display and inspection.
--
-- repr(v) renders values the way the REPL echoes them: quoted strings,
-- tables with depth/cycle/length limits, function signatures from Lua
-- 5.4's debug.getinfo (nparams/isvararg). help(v) describes a value;
-- whos() lists globals. docs is an extensible docstring map plugins
-- can add to, seeded with a few stdlib entries.
local introspect = {}

introspect.docs = {}

local MAX_DEPTH = 3
local MAX_ITEMS = 20

local function quote_string(s)
    local body = s:gsub("\\", "\\\\")
        :gsub("'", "\\'")
        :gsub("\n", "\\n")
        :gsub("\t", "\\t")
        :gsub("\r", "\\r")
    return "'" .. body .. "'"
end

local function is_ident(k)
    return type(k) == "string" and k:match("^[%a_][%w_]*$") ~= nil
end

local function repr_value(v, depth, seen)
    local tv = type(v)
    if tv == "string" then
        return quote_string(v)
    end
    if tv == "table" then
        if seen[v] then
            return "<cycle>"
        end
        if depth >= MAX_DEPTH then
            return "{...}"
        end
        seen[v] = true
        local parts, total = {}, 0
        for k, val in pairs(v) do
            total = total + 1
            if total <= MAX_ITEMS then
                local key
                if is_ident(k) then
                    key = k .. " = "
                else
                    key = "[" .. repr_value(k, depth + 1, seen) .. "] = "
                end
                parts[#parts + 1] = key .. repr_value(val, depth + 1, seen)
            end
        end
        seen[v] = nil -- shared (non-cyclic) tables may appear twice
        if #parts == 0 then
            return "{}"
        end
        if total > MAX_ITEMS then
            parts[#parts + 1] = "... " .. (total - MAX_ITEMS) .. " more"
        end
        return "{ " .. table.concat(parts, ", ") .. " }"
    end
    if tv == "function" then
        return introspect.signature(v) or tostring(v)
    end
    return tostring(v) -- numbers, booleans, userdata, ...
end

function introspect.repr(v)
    return repr_value(v, 0, {})
end

-- "function(2 params)" / "function(1+ params)" / "function(...)";
-- parameter names are not recoverable without parsing sources.
function introspect.signature(fn)
    if type(fn) ~= "function" then
        return nil
    end
    local info = debug.getinfo(fn, "u")
    local n = info and info.nparams or 0
    if info and info.isvararg then
        if n == 0 then
            return "function(...)"
        end
        return "function(" .. n .. "+ params)"
    end
    return "function(" .. n .. (n == 1 and " param)" or " params)")
end

local function defined_at(fn)
    local info = debug.getinfo(fn, "S")
    if not info or info.what == "C" or info.what == "main" then
        return "builtin"
    end
    return "defined at " .. tostring(info.short_src) .. ":" .. tostring(info.linedefined)
end

-- Docstring lookup: docs is keyed by dotted global path ("string.rep",
-- "table.insert"). Without a path, reverse-scan the first two levels of
-- _G for a value identical to v (cheap enough on demand).
function introspect.doc_for(path_or_value)
    if type(path_or_value) == "string" and introspect.docs[path_or_value] then
        return introspect.docs[path_or_value]
    end
    local v = path_or_value
    for name, val in pairs(_G) do
        if val == v and introspect.docs[name] then
            return introspect.docs[name]
        end
        if type(val) == "table" then
            for k2, v2 in pairs(val) do
                if v2 == v and introspect.docs[name .. "." .. k2] then
                    return introspect.docs[name .. "." .. k2]
                end
            end
        end
    end
    return nil
end

-- help(v) -> text. Functions: signature, location, docstring. Tables:
-- one line per named field with type or signature, then a docstring if
-- the table itself is documented.
function introspect.help(v)
    local t = type(v)
    if t == "function" then
        local lines = { introspect.signature(v) .. "  -- " .. defined_at(v) }
        local doc = introspect.doc_for(v)
        if doc then
            lines[#lines + 1] = doc
        end
        return table.concat(lines, "\n") .. "\n"
    end
    if t == "table" then
        local fields = {}
        for k, val in pairs(v) do
            if type(k) == "string" then
                fields[#fields + 1] = { name = k, kind = type(val) }
            end
        end
        table.sort(fields, function(a, b)
            return a.name < b.name
        end)
        local lines = { "table with " .. #fields .. " named fields:" }
        for _, f in ipairs(fields) do
            local sig
            if f.kind == "function" then
                sig = introspect.signature(v[f.name]) or "function"
            else
                sig = f.kind
            end
            lines[#lines + 1] = string.format("  %-18s %s", f.name, sig)
        end
        local doc = introspect.doc_for(v)
        if doc then
            lines[#lines + 1] = doc
        end
        return table.concat(lines, "\n") .. "\n"
    end
    return t .. ": " .. introspect.repr(v) .. "\n"
end

-- whos() -> text: globals sorted by name with type and a preview.
function introspect.whos()
    local rows = {}
    for name, v in pairs(_G) do
        if type(name) == "string" then
            local preview = introspect.repr(v)
            if #preview > 48 then
                preview = preview:sub(1, 45) .. "..."
            end
            rows[#rows + 1] = { name = name, kind = type(v), preview = preview }
        end
    end
    table.sort(rows, function(a, b)
        return a.name < b.name
    end)
    local out = { string.format("%-14s %-9s %s", "name", "type", "value") }
    for _, r in ipairs(rows) do
        out[#out + 1] = string.format("%-14s %-9s %s", r.name, r.kind, r.preview)
    end
    return table.concat(out, "\n") .. "\n"
end

-- Bounded Levenshtein distance; returns cutoff + 1 as soon as both
-- strings are provably further apart than the cutoff (whole rows wider
-- than it cannot recover).
local function edit_distance(a, b, cutoff)
    if math.abs(#a - #b) > cutoff then
        return cutoff + 1
    end
    local prev, cur = {}, {}
    for j = 0, #b do
        prev[j + 1] = j
    end
    for i = 1, #a do
        cur[1] = i
        local row_best = i
        local ca = a:byte(i)
        for j = 1, #b do
            local v = math.min(prev[j + 1] + 1, cur[j] + 1,
                prev[j] + (ca == b:byte(j) and 0 or 1))
            cur[j + 1] = v
            if v < row_best then
                row_best = v
            end
        end
        if row_best > cutoff then
            return cutoff + 1
        end
        for j = 0, #b do
            prev[j + 1] = cur[j + 1]
        end
    end
    return prev[#b + 1]
end

-- Suggest near-miss globals for runtime errors. Lua annotates failures
-- like "attempt to call a nil value (global 'fooo')"; when the annotated
-- global does not exist, the closest _G names are what the user meant.
-- Returns "did you mean 'x'?" (two candidates when two are equally
-- close), or nil when there is nothing credible to offer.
function introspect.suggest(err)
    local kind, name = tostring(err):match("%((%a+) '([%w_]+)'%)")
    if kind ~= "global" or type(_G) ~= "table" then
        return nil
    end
    if rawget(_G, name) ~= nil then
        return nil -- exists but is not callable etc.: nothing to suggest
    end
    local cutoff = #name >= 4 and 2 or 1
    local best, second
    for cand in pairs(_G) do
        if type(cand) == "string" and cand ~= name then
            local d = edit_distance(name, cand, cutoff)
            if d <= cutoff then
                if not best or d < best.d or (d == best.d and cand < best.name) then
                    second = best
                    best = { name = cand, d = d }
                elseif not second or d < second.d
                    or (d == second.d and cand < second.name) then
                    second = { name = cand, d = d }
                end
            end
        end
    end
    if not best then
        return nil
    end
    if second and second.d == best.d then
        return string.format("did you mean '%s' or '%s'?", best.name, second.name)
    end
    return string.format("did you mean '%s'?", best.name)
end

-- seed docs: a handful of stdlib one-liners, extendable at runtime
local seed = {
    print = "print(...) — writes its arguments (tab-separated) to the output funnel",
    type = "type(v) — returns the type name of v as a string",
    pairs = "pairs(t) — iterator over all key/value pairs of t",
    ipairs = "ipairs(t) — iterator over the array part of t (1..n)",
    tostring = "tostring(v) — human-readable string for v",
    tonumber = "tonumber(v, [base]) — converts v to a number, or nil",
    ["string.format"] = "string.format(fmt, ...) — printf-style formatting",
    ["string.rep"] = "string.rep(s, n, [sep]) — repeats s n times",
    ["string.sub"] = "string.sub(s, i, [j]) — substring from i to j",
    ["table.insert"] = "table.insert(t, [pos], v) — insert v into t",
    ["table.concat"] = "table.concat(t, [sep]) — join array elements into a string",
    ["math.floor"] = "math.floor(x) — rounds towards -infinity",
    ["io.read"] = "io.read(fmt) — reads from stdin ('l' = one line)",
    ["io.write"] = "io.write(...) — writes strings to stdout",
    ["os.time"] = "os.time([tbl]) — current time as seconds since epoch",
}
for path, doc in pairs(seed) do
    introspect.docs[path] = doc
end

return introspect
