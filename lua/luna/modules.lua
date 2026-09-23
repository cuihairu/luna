-- luna.modules: Node-style require resolution layered onto
-- package.searchers.
--
-- Installed as the second searcher (right after package.preload,
-- before the native package.path searcher) so manifests take effect.
-- It only handles the two forms the native machinery cannot:
--
--   1. relative paths:  require "./lib/util" — resolved against the
--      directory of the file that called require;
--   2. bare module names: require "dep" — walks up the parent
--      directories looking for a luna_modules/dep/ directory (the
--      node_modules convention). Inside a package directory the
--      package.json manifest picks the entry: {"main": "lib/entry.lua"};
--      without a manifest the convention is dep/init.lua, and a flat
--      luna_modules/dep.lua is also accepted.
--
-- Results are cached by the native package.loaded machinery, exactly
-- like any other module. Forms these rules do not resolve (or rooted
-- lookups that find nothing) fall through to the native searchers, so
-- package.path modules (argparse, dkjson, lexer, ...) keep working.
local modules = {}

-- Manifests parsed during resolution, name -> table (name, version,
-- main, description, ...). Plugins (batch 9) introspect this.
local manifests = {}

local ok_json, json = pcall(require, "dkjson")

local function readfile(path)
    local fh = io.open(path, "r")
    if not fh then
        return nil
    end
    local text = fh:read("a")
    fh:close()
    return text
end

-- Directory of the Lua chunk that (transitively) called require.
-- Walks up the stack past require itself (a C function) to the first
-- chunk whose source is a real file ("@path"); REPL/eval chunks have
-- synthetic sources, so file-less callers resolve from the process
-- working directory. Relative sources (scripts launched as
-- `luna app/main.lua`) are made absolute before returning, so the
-- upward walk always has a root.
local function caller_dir()
    local depth = 3 -- modules.searcher <- require(C) <- caller
    local info = debug.getinfo(depth, "S")
    while info and depth < 16 do
        local src = info.source or ""
        if src:sub(1, 1) == "@" then
            local dir = src:sub(2):match("^(.*)/[^/]+") or "."
            if dir:sub(1, 1) ~= "/" then
                local okl, lfs = pcall(require, "lfs")
                if okl and lfs.currentdir then
                    dir = lfs.currentdir() .. "/" .. dir
                end
            end
            return dir
        end
        depth = depth + 1
        info = debug.getinfo(depth, "S")
    end
    return nil
end

local function parent(dir)
    local up = dir:match("^(.*)/[^/]+")
    if up == nil or up == dir then
        return nil
    end
    return up
end

-- Absolute starting directory for upward walks. Falls back to the
-- working directory when lfs is unavailable (then walks are limited:
-- relative "../.." chains have no root).
local function start_dir()
    local dir = caller_dir()
    if dir and dir ~= "" then
        return dir
    end
    local okl, lfs = pcall(require, "lfs")
    if okl and lfs.currentdir then
        return lfs.currentdir()
    end
    return "."
end

-- Entry file of a package directory: manifest "main" first, then the
-- init.lua convention.
local function package_entry(pkg_dir)
    local text = readfile(pkg_dir .. "/package.json")
    if text and ok_json then
        local data = json.decode(text)
        if type(data) == "table" then
            manifests[pkg_dir] = data
            local main = data.main
            if type(main) == "string" and main ~= "" then
                local entry = pkg_dir .. "/" .. main
                if entry:match("%.lua$") then
                    return entry
                end
                if readfile(entry .. ".lua") then
                    return entry .. ".lua"
                end
                return entry .. "/init.lua"
            end
            -- manifest present but no usable main: fall through to
            -- the init.lua convention
        end
    end
    if readfile(pkg_dir .. "/init.lua") then
        return pkg_dir .. "/init.lua"
    end
    return nil
end

local function load_entry(path)
    local loader, err = loadfile(path)
    if loader then
        return loader, path
    end
    return nil
end

-- A bare name: walk up looking for luna_modules/<name>/{package.json,
-- init.lua} or a flat luna_modules/<name>.lua.
function modules.resolve_bare(modname, from)
    local dir = from or start_dir()
    while dir do
        local pkg = dir .. "/luna_modules/" .. modname
        local entry = package_entry(pkg)
        if entry then
            return load_entry(entry)
        end
        local flat = dir .. "/luna_modules/" .. modname .. ".lua"
        if readfile(flat) then
            return load_entry(flat)
        end
        dir = parent(dir)
    end
    return nil
end

-- A relative name ("./x", "../x/y"): against the requiring file's
-- directory (or the working directory for file-less callers).
function modules.resolve_relative(modname, from)
    local dir = from or caller_dir() or "."
    local base = dir .. "/" .. modname
    local direct = base .. ".lua"
    if readfile(direct) then
        return load_entry(direct)
    end
    local init = base .. "/init.lua"
    if readfile(init) then
        return load_entry(init)
    end
    return nil
end

-- The package.searchers entry. Returns loader, path on success; nil
-- otherwise (the searcher chain continues with package.path).
function modules.searcher(modname)
    if type(modname) ~= "string" or modname == "" then
        return nil
    end
    if modname:sub(1, 1) == "." then
        return modules.resolve_relative(modname)
    end
    return modules.resolve_bare(modname)
end

-- Install as searcher #2: after preload (so runtime-injected modules
-- win), before the native package.path searcher (so manifest "main"
-- entries take effect). Idempotent.
function modules.install()
    for _, s in ipairs(package.searchers) do
        if s == modules.searcher then
            return false
        end
    end
    table.insert(package.searchers, 2, modules.searcher)
    return true
end

-- Manifest of a package directory, if one was parsed.
function modules.manifest(pkg_dir)
    return manifests[pkg_dir]
end

return modules
