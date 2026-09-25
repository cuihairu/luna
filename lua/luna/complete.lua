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
-- registered source (the built-in require-target source below, plus
-- plugin additions). Sources see the whole line (they may implement
-- e.g. path completion after "dofile('") and a raising source is
-- skipped, not fatal. Later candidates never duplicate earlier ones.
function complete.line(line)
    local cands = builtin(line)
    local seen = {}
    for _, c in ipairs(cands) do
        seen[c] = true
    end
    for _, source in ipairs(complete.sources) do
        local ok, more = pcall(source, line)
        if ok and type(more) == "table" then
            for _, c in ipairs(more) do
                if not seen[c] then
                    seen[c] = true
                    cands[#cands + 1] = c
                end
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

-- --------------------------------------------------------------------- --
-- require-target completion: `require "pre` / `require("pre` proposes
-- loadable names — package.loaded and package.preload entries, plus
-- packages found on disk in every directory the package.path "?"/
-- "?/init.lua" elements point at (the bundled luna_modules tree, user
-- project trees). A dotted prefix continues into the container: its
-- loaded table's fields and its subpackage files on disk. Filesystem
-- scans need lfs; without it that part simply shrinks the candidate
-- set, and any unreadable directory is skipped.

-- Directories behind the two path-element shapes require actually uses.
local function path_dirs()
    local dirs, seen = {}, {}
    for elem in package.path:gmatch("[^;]+") do
        local dir = elem:match("^(.*)/%?%.lua$")
            or elem:match("^(.*)/%?/init%.lua$")
        if dir and dir ~= "" and not seen[dir] then
            seen[dir] = true
            dirs[#dirs + 1] = dir
        end
    end
    return dirs
end

-- Entry names of a directory, or nil when it cannot be listed.
local function scan_dir(path)
    local okl, lfs = pcall(require, "lfs")
    if not okl or type(lfs) ~= "table" or not lfs.dir then
        return nil
    end
    local ok, entries = pcall(function()
        local out = {}
        for entry in lfs.dir(path) do
            out[#out + 1] = entry
        end
        return out
    end)
    return ok and entries or nil
end

local function add_matches(names, seen, entries, prefix, base)
    for _, entry in ipairs(entries or {}) do
        if entry:sub(1, 1) ~= "." then
            local name = entry:gsub("%.lua$", "")
            if name:sub(1, #prefix) == prefix then
                local full = base and (base .. "." .. name) or name
                if not seen[full] then
                    seen[full] = true
                    names[#names + 1] = full
                end
            end
        end
    end
end

local function require_source(line)
    local prefix = line:match('require%s*%(%s*["\']([%w_.%-]*)$')
        or line:match('require%s*["\']([%w_.%-]*)$')
    if not prefix then
        return {}
    end

    local names, seen = {}, {}
    local function add(full)
        if full ~= "" and not seen[full] then
            seen[full] = true
            names[#names + 1] = full
        end
    end

    local base, tail = prefix:match("^(.*)%.([^%.]*)$")
    if base then
        -- dotted continuation: sub-names of the container package
        local loaded = package.loaded[base]
        if type(loaded) == "table" then
            for k in pairs(loaded) do
                if type(k) == "string" and k:sub(1, #tail) == tail then
                    add(base .. "." .. k)
                end
            end
        end
        local dirpath = base:gsub("%.", "/")
        for _, dir in ipairs(path_dirs()) do
            add_matches(names, seen, scan_dir(dir .. "/" .. dirpath), tail, base)
        end
    else
        for name in pairs(package.loaded) do
            if type(name) == "string" and name:sub(1, #prefix) == prefix then
                add(name)
            end
        end
        for name in pairs(package.preload) do
            if type(name) == "string" and name:sub(1, #prefix) == prefix then
                add(name)
            end
        end
        for _, dir in ipairs(path_dirs()) do
            add_matches(names, seen, scan_dir(dir), prefix, nil)
        end
    end
    table.sort(names)
    return names
end

complete.add_source(require_source)

return complete
