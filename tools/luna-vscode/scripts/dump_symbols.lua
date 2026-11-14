-- dump_symbols.lua — regenerate the extension's symbol snapshot from
-- the real runtime's own introspection. The runtime is the source of
-- truth: module names come from its staged load path (package.path's
-- luna_modules entries, listed with fs.readdirSync), signatures from
-- luna.introspect.signature, docstrings from luna.introspect.docs,
-- definition sites from debug.getinfo. Never hand-write entries: a
-- hand-maintained table fossilizes the moment the runtime moves.
--
-- Usage: luna scripts/dump_symbols.lua <out.json>
-- Summary goes to stderr; stdout stays clean.

local json = require "dkjson"
local intro = require "luna.introspect"
local fs = require "fs"

local MAX_DEPTH = 3

-- Re-entrancy guard: running this script puts its own dir on
-- package.path, so the module sweep below can find "dump_symbols" and
-- require it — which would re-execute the whole dump, recursively.
local THIS_MODULE = "dump_symbols"
package.loaded[THIS_MODULE] = true

-- "modules/fs/init.lua:65"-style definition site; nil for C builtins
local function defined_at(fn)
    local info = debug.getinfo(fn, "S")
    if not info or info.what ~= "Lua" or info.source:sub(1, 1) ~= "@" then
        return nil
    end
    local src = info.source:sub(2)
    src = src:gsub("^.*/luna_modules/", "modules/")
    src = src:gsub("^.*/luna/", "luna/")
    return src .. ":" .. tostring(info.linedefined)
end

local function node_of(v, depth)
    local t = type(v)
    if t == "function" then
        return {
            kind = t,
            sig = intro.signature(v),
            at = defined_at(v),
        }
    elseif t == "table" then
        if depth <= 0 then
            return { kind = t }
        end
        -- some faces refuse enumeration (process.env raises; closed
        -- userdata can too) — degrade to a bare kind, honestly
        local okm, members = pcall(function()
            local m = {}
            for k, vv in pairs(v) do
                if type(k) == "string" and k:match("^%w[%w_]*$") then
                    m[k] = node_of(vv, depth - 1)
                end
            end
            return next(m) and m or nil
        end)
        if not okm then
            return { kind = t }
        end
        return { kind = t, members = members }
    end
    return { kind = t } -- string/number/boolean/... faces
end

-- Every module name the runtime can load from its staged dirs: the
-- luna_modules entries of package.path, listed with the runtime's own
-- fs.readdirSync (plain Lua has no readdir).
local function module_names()
    local seen, names = {}, {}
    for entry in package.path:gmatch("[^;]+") do
        local dir = entry:match("^(.*)/%?%.lua$") or entry:match("^(.*)/%?/init%.lua$")
        if dir then
            local ok, list = pcall(fs.readdirSync, dir)
            if ok then
                for _, name in ipairs(list) do
                    local mod = name:match("^(%w[%w_]*)%.lua$") or
                        (name:match("^%w[%w_]*$") and name)
                    if mod and not seen[mod] then
                        seen[mod] = true
                        names[#names + 1] = mod
                    end
                end
            end
        end
    end
    table.sort(names)
    return names
end

-- Attach docstrings onto the nodes they describe, keyed by dotted path
-- (the same keys intro.docs and intro.doc_for use).
local function attach_docs(node, path)
    if not node then
        return
    end
    local doc = intro.docs[path]
    if doc then
        node.doc = doc
    end
    if node.members then
        for k, child in pairs(node.members) do
            attach_docs(child, path .. "." .. k)
        end
    end
end

local out = {
    generated = os.date("!%Y-%m-%dT%H:%M:%SZ"),
    modules = {},
    globals = {},
    docs = intro.docs,
}

for _, name in ipairs(module_names()) do
    local ok, mod = pcall(require, name)
    -- true is package.loaded's executed-marker (a chunk, not a module
    -- face) — e.g. this very script's own name; not worth a dump entry
    if ok and mod ~= nil and mod ~= true then
        out.modules[name] = node_of(mod, MAX_DEPTH)
    else
        io.stderr:write(string.format(
            "skip %s: ok=%s type=%s\n", name, tostring(ok), type(mod)))
    end
end

-- luna-added globals: the same filter %reset uses (base set minus
-- __LUNA internals). kernel is captured in the base set, so it is
-- added explicitly — it is THE luna global (kernel.wake, kernel.pid).
local base = _G.__LUNA_BASE_GLOBALS or {}
for name, v in pairs(_G) do
    if type(name) == "string" and not base[name] and not name:match("^__LUNA") then
        out.globals[name] = node_of(v, MAX_DEPTH)
    end
end
out.globals.kernel = node_of(require "kernel", MAX_DEPTH)

for name, node in pairs(out.modules) do
    attach_docs(node, name)
end
for name, node in pairs(out.globals) do
    attach_docs(node, name)
end

local out_path = arg[1]
if not out_path then
    io.stderr:write("usage: luna scripts/dump_symbols.lua <out.json>\n")
    os.exit(1)
end

local text = assert(json.encode(out, { indent = true }))
local fh = assert(io.open(out_path, "w"))
fh:write(text, "\n")
fh:close()

-- summary on stderr (a regen is verified by the snapshot test)
local modules_n, funcs_n = 0, 0
local function count(node)
    if node.kind == "function" then
        funcs_n = funcs_n + 1
    end
    for _, child in pairs(node.members or {}) do
        count(child)
    end
end
for _, node in pairs(out.modules) do
    modules_n = modules_n + 1
    count(node)
end
for _, node in pairs(out.globals) do
    count(node)
end
io.stderr:write(string.format(
    "symbols: %d modules, %d functions -> %s (generated %s)\n",
    modules_n, funcs_n, out_path, out.generated))
