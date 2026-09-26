-- luna.plugins: directory-based plugins, Node-style.
--
-- A plugin is a directory holding a plugin.json manifest
-- ({name, version, main, description}) plus its entry script. Entry
-- scripts wire themselves into the runtime through the extension
-- points — nothing here is plugin-specific:
--
--   magic.register(name, run, help)   REPL commands
--   complete.add_source(fn)           Tab completion sources
--   highlight.set(fn)                 syntax highlighting rules
--   -- a returned "modules" table injects modules:
--   return { modules = { ["my.mod"] = value_or_loader } }
--
-- The kernel/plugin boundary lives in those APIs: a plugin never
-- reaches into kernel internals, and the loader only discovers,
-- orders, runs and reports.
--
-- Discovery, nearest first (like node_modules): ./plugins (project),
-- ~/.luna/plugins (user), then $LUNA_PLUGIN_PATH entries (colon
-- separated). The first plugin bearing a manifest name wins; later
-- same-name discoveries are recorded as overridden, so a project can
-- shadow a user-level default.
--
-- Failure isolation: each plugin runs under pcall. A plugin that
-- raises — or fails to parse — is reported and skipped; remaining
-- plugins still load, and luna keeps starting.
local plugins = {}

plugins.loaded = {}     -- name -> manifest table
plugins.failed = {}     -- name (or dir, if unnamed) -> error string
plugins.overridden = {} -- name -> dir that lost the name race

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

function plugins.locations()
    local dirs = {}
    -- nearest wins, so project plugins are collected first
    dirs[#dirs + 1] = "./plugins"
    local home = os.getenv("HOME")
    if home and home ~= "" then
        dirs[#dirs + 1] = home .. "/.luna/plugins"
    end
    local extra = os.getenv("LUNA_PLUGIN_PATH")
    if extra then
        for path in extra:gmatch("[^:]+") do
            dirs[#dirs + 1] = path
        end
    end
    return dirs
end

local function is_dir(path)
    local okl, lfs = pcall(require, "lfs")
    if not okl then
        return false
    end
    return lfs.attributes(path, "mode") == "directory"
end

-- Manifest of one plugin directory, or nil + reason.
local function manifest_of(dir)
    local text = readfile(dir .. "/plugin.json")
    if not text then
        return nil, "no plugin.json"
    end
    if not ok_json then
        return nil, "json support unavailable"
    end
    -- dkjson answers a syntax error with nil, pos, err instead of
    -- raising, so a malformed manifest is a *reason*, not a crash —
    -- and the parser's own words beat a generic "no usable name"
    local data, _, jerr = json.decode(text)
    if type(jerr) == "string" then
        return nil, "plugin.json does not parse: " .. jerr
    end
    if type(data) ~= "table" or type(data.name) ~= "string" or data.name == "" then
        return nil, "plugin.json has no usable name"
    end
    return data
end

-- All discoverable plugins, nearest-first, deduplicated by name.
-- Invalid candidates are reported via plugins.failed keyed by
-- directory (they have no manifest name to report under).
function plugins.discover()
    local out, by_name = {}, {}
    for _, root in ipairs(plugins.locations()) do
        local entries = {}
        if is_dir(root) then
            local okl, lfs = pcall(require, "lfs")
            if okl then
                for entry in lfs.dir(root) do
                    if entry:sub(1, 1) ~= "." then
                        entries[#entries + 1] = entry
                    end
                end
                table.sort(entries)
            end
        end
        for _, entry in ipairs(entries) do
            local dir = root .. "/" .. entry
            local manifest, why = manifest_of(dir)
            if not manifest then
                plugins.failed[dir] = why
            elseif not by_name[manifest.name] then
                by_name[manifest.name] = dir
                out[#out + 1] = { dir = dir, manifest = manifest }
            else
                plugins.overridden[manifest.name] = dir
            end
        end
    end
    return out
end

-- Entry file of a plugin directory: manifest "main" (file or package
-- layout), then the init.lua convention.
local function entry_of(dir, manifest)
    local main = manifest.main
    if type(main) == "string" and main ~= "" then
        if main:match("%.lua$") then
            return dir .. "/" .. main
        end
        return dir .. "/" .. main .. ".lua"
    end
    return dir .. "/init.lua"
end

-- Load one plugin directory: run its entry, register injected
-- modules. Returns ok, err.
function plugins.load(dir, manifest)
    local entry_path = entry_of(dir, manifest)
    local chunk, err = loadfile(entry_path)
    if not chunk then
        return nil, (err or "cannot load " .. entry_path)
    end
    local ok, result = pcall(chunk)
    if not ok then
        return nil, tostring(result)
    end
    if type(result) == "table" and type(result.modules) == "table" then
        for name, value in pairs(result.modules) do
            if type(name) == "string" then
                -- package.preload holds loader functions; wrap plain
                -- values so require resolves them to the value itself
                if type(value) == "function" then
                    package.preload[name] = value
                else
                    package.preload[name] = function() return value end
                end
            end
        end
    end
    return true
end

-- Discover and load everything. Returns the report tables.
function plugins.load_all()
    for _, found in ipairs(plugins.discover()) do
        local name = found.manifest.name
        local ok, err = plugins.load(found.dir, found.manifest)
        if ok then
            plugins.loaded[name] = found.manifest
        else
            plugins.failed[name] = err
        end
    end
    return plugins.loaded, plugins.failed, plugins.overridden
end

return plugins
