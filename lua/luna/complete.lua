-- luna.complete: the Tab completion engine.
--
-- complete.line(line) returns the insert-strings that can follow the
-- last partial identifier on the line (readline convention: candidates
-- are the text to insert at the cursor, not full rewritten words).
--
-- Builtin candidates come from three places: global names, Lua
-- keywords, and — for dotted chains like "io.w" — the fields of the
-- resolved container, metatable-aware via a plain-table __index.
-- Resolution runs under pcall so a hostile __index cannot break the
-- prompt.
--
-- Extension point: sources. Plugins append function(line) -> list
-- here; results are merged after the builtin candidates and a raising
-- source is skipped (failure isolation), never fatal.
local complete = {}

complete.sources = {}

local KEYWORDS = {
    "and", "break", "do", "else", "elseif", "end", "false", "for",
    "function", "goto", "if", "in", "local", "nil", "not", "or",
    "repeat", "return", "then", "true", "until", "while",
}

-- The trailing identifier chain of the line: `io.w`, `obj:`, `x`. An
-- empty match (line ends with a space, an operator or an open paren)
-- means there is nothing word-like to complete.
local function last_chain(line)
    return line:match("([%w_%.:]*)$")
end

-- "io.w" -> "io", "w"   "a.b.c" -> "a.b", "c"   "obj:" -> "obj", ""
-- A bare word has no separator and comes back as the field itself.
function complete.split_chain(chain)
    local base, field = chain:match("^(.-)[%.:]([^%.:]*)$")
    if base then
        return base, field
    end
    return nil, chain
end

-- Walk a dotted/colon chain from _G. Protected: indexing may run
-- metamethods, and completion must never crash the session.
function complete.resolve(base)
    local value = _G
    local ok, err = pcall(function()
        for part in base:gmatch("[^%.:]+") do
            local t = type(value)
            if t ~= "table" and t ~= "userdata" then
                error("not a container", 0)
            end
            value = value[part]
            if value == nil then
                error("no such field", 0)
            end
        end
    end)
    if ok then
        return value
    end
    return nil, err
end

-- String keys of a table (plus its plain-table __index) that start
-- with the given prefix, sorted.
local function fields(value, prefix)
    local out, seen = {}, {}
    local function collect(t)
        for k in pairs(t) do
            if type(k) == "string" and k:sub(1, #prefix) == prefix and not seen[k] then
                seen[k] = true
                out[#out + 1] = k
            end
        end
    end
    local ok = pcall(collect, value) -- pairs() can be guarded too
    if ok then
        local mt = getmetatable(value)
        if mt and type(mt.__index) == "table" then
            pcall(collect, mt.__index)
        end
    end
    table.sort(out)
    return out
end

-- A unique callable completes as a call site: "print" -> "print(",
-- IPython-style. Everything else stays a bare word.
local function decorate(cands, container)
    if #cands == 1 and container ~= nil then
        local ok, v = pcall(function()
            return container[cands[1]]
        end)
        if ok and type(v) == "function" then
            return { cands[1] .. "(" }
        end
    end
    return cands
end

local function builtin(line)
    local chain = last_chain(line)
    if chain == nil or chain == "" then
        return {}
    end

    local base, field = complete.split_chain(chain)
    if base == nil then
        -- bare word: matching globals, then matching keywords
        local names = {}
        for name in pairs(_G) do
            if type(name) == "string" and name:sub(1, #field) == field then
                names[#names + 1] = name
            end
        end
        for _, kw in ipairs(KEYWORDS) do
            if kw:sub(1, #field) == field then
                names[#names + 1] = kw
            end
        end
        table.sort(names)
        return decorate(names, _G)
    end

    local container = complete.resolve(base)
    if container == nil then
        return {}
    end
    return decorate(fields(container, field), container)
end

-- The engine entry point: builtin candidates merged with every
-- registered plugin source. Sources see the whole line (they may
-- implement e.g. path completion after "dofile('") and a raising
-- source is skipped, not fatal.
function complete.line(line)
    local cands = builtin(line)
    for _, source in ipairs(complete.sources) do
        local ok, more = pcall(source, line)
        if ok and type(more) == "table" then
            for _, c in ipairs(more) do
                cands[#cands + 1] = c
            end
        end
    end
    return cands
end

function complete.add_source(source)
    if type(source) ~= "function" then
        error("completion source must be a function", 2)
    end
    complete.sources[#complete.sources + 1] = source
end

return complete
