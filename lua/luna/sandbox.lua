-- luna sandbox: process-level restriction for untrusted code.
--
--   luna --sandbox script.lua [args]     (also -e and the console)
--
-- One launch mode, two worlds. Trusted mode is today's runtime,
-- unchanged. Sandbox mode installs four denial layers before any user
-- code runs, then takes its own knobs away:
--
--   1. module allowlist — require goes through a guard; anything
--      outside the default set (plus LUNA_SANDBOX_MODULES) and the
--      luna.* policy layer is refused;
--   2. no native loading — package.cpath emptied and the package
--      table frozen (assigning it back is the trivial bypass);
--   3. permissions — subprocess (os.execute, io.popen) and filesystem
--      mutation (os.remove/rename/tmpname, io.open write modes,
--      io.output(file)) raise; the environment turns invisible
--      (os.getenv returns nil) rather than raising;
--   4. resource caps — kernel.fuel (count-hook strikes ≈ 100k
--      instructions each) and kernel.memcap (allocator bytes), both
--      driven by LUNA_SANDBOX_FUEL / LUNA_SANDBOX_MEM, both removed
--      from the kernel table once set.
--
-- Introspection that would unwind the guards goes too: the bytecode
-- loader (load forced to text mode) and debug's upvalue/registry
-- probes (a getupvalue on the require guard would hand back the
-- unguarded require).
--
-- Boundary, stated plainly: this hardens the same Lua state — there is
-- no second VM. Against untrusted code written IN Lua it holds (the
-- language has no unsafe primitives left reachable); it is not a
-- defense against a compromised process. Plugins are skipped: they
-- inject package.preload, an allowlist bypass.
local M = {}

-- pure-Lua modules a sandboxed script may require without asking
local DEFAULT_MODULES = {
    "json", "path", "util", "events", "stream",
    "csv", "ini", "toml", "yaml", "xml", "zlib",
    "lpeg", "argparse",
}

function M.install()
    local kernel = require("kernel")
    local os_env = os.getenv

    -- limits first: the values come from the environment while the
    -- script can still not see it, then the knobs come off the table
    local fuel = tonumber(os_env("LUNA_SANDBOX_FUEL"))      -- millions of instructions
    if fuel and fuel > 0 then
        kernel.fuel(math.floor(fuel * 10 + 0.5))            -- 1 strike ≈ 100k
    end
    local mem = tonumber(os_env("LUNA_SANDBOX_MEM"))        -- MiB
    if mem and mem > 0 then
        kernel.memcap(math.floor(mem * 1024 * 1024))
    end
    kernel.fuel = nil
    kernel.memcap = nil

    -- allowlist: defaults + LUNA_SANDBOX_MODULES (comma-separated) +
    -- everything already loaded (the runtime's own infrastructure:
    -- kernel, linedit, the luna.* policy layer)
    local allowed = {}
    for _, name in ipairs(DEFAULT_MODULES) do
        allowed[name] = true
    end
    for name in (os_env("LUNA_SANDBOX_MODULES") or ""):gmatch("[^,%s]+") do
        allowed[name] = true
    end
    local baseline = {}
    for name in pairs(package.loaded) do
        baseline[name] = true
    end
    local realrequire = require
    local function guard(name)
        if type(name) ~= "string" or
           not (baseline[name] or allowed[name] or name:match("^luna%.")) then
            error("sandbox: module '" .. tostring(name) ..
                "' is not in the allowlist", 2)
        end
        local m = realrequire(name)
        if name == "fs" then
            M._harden_fs(m)
        end
        return m
    end
    _G.require = guard

    -- no native loading, and no writing the loaders back. A bare
    -- __newindex only guards NEW keys — cpath exists, so an assignment
    -- would raw-set right past it. Instead: nil the loadlib, snapshot
    -- what's left, drain the table, and serve reads through __index;
    -- every write now lands on __newindex. __metatable keeps the proxy
    -- itself out of reach (getmetatable returns the lock string).
    package.loadlib = nil
    package.cpath = ""
    local frozen = {}
    for k, v in pairs(package) do
        frozen[k] = v
    end
    for k in pairs(package) do
        rawset(package, k, nil)
    end
    setmetatable(package, {
        __index = frozen,
        __metatable = "sandbox: package is read-only",
        __newindex = function()
            error("sandbox: package is read-only", 2)
        end,
    })

    local function denied(what)
        return function()
            error("sandbox: " .. what .. " is denied", 2)
        end
    end

    -- subprocess and filesystem mutation raise. Reads follow a
    -- different rule: they hand back nothing rather than explode —
    -- the environment is invisible (os.getenv → nil, tolerated by
    -- the runtime's own nil-checking callers: REPL history, cwd).
    os.execute = denied("os.execute")
    os.getenv = function()
        return nil
    end
    os.remove = denied("os.remove")
    os.rename = denied("os.rename")
    os.tmpname = denied("os.tmpname")

    -- io: write-shaped entry points only; reads stay open
    io.popen = denied("io.popen")
    local realopen = io.open
    io.open = function(path, mode)
        mode = mode or "l"
        if mode:find("[wa+]") then
            error("sandbox: io.open write modes are denied", 2)
        end
        return realopen(path, mode)
    end
    local realoutput = io.output
    io.output = function(f)
        if type(f) == "string" then
            error("sandbox: io.output(file) is denied", 2)
        end
        return realoutput(f)
    end

    -- bytecode loader off: load/text only (string.dump + load with
    -- binary chunks is the standard escape from a text-only world)
    local realload = load
    _G.load = function(chunk, name, mode, env)
        if mode == nil or mode:find("b", 1, true) then
            mode = mode and (mode:gsub("b", "")) or "t"
            if mode == "" then
                mode = "t"
            end
        end
        return realload(chunk, name, mode, env)
    end
    if _G.loadstring then -- 5.4-era alias, if present
        _G.loadstring = function(s, name)
            return realload(s, name, "t")
        end
    end

    -- debug: keep what reads function objects (getinfo drives the
    -- introspection magics), drop what unwinds closures and frames —
    -- the guards hold the real require/io as upvalues (getupvalue is
    -- the door) and locals are probeable frame state (getlocal).
    debug.getupvalue = denied("debug.getupvalue")
    debug.setupvalue = denied("debug.setupvalue")
    debug.upvalueid = denied("debug.upvalueid")
    debug.upvaluejoin = denied("debug.upvaluejoin")
    debug.getlocal = denied("debug.getlocal")
    debug.setlocal = denied("debug.setlocal")
    debug.getregistry = denied("debug.getregistry")
    debug.sethook = denied("debug.sethook")
    debug.setmetatable = denied("debug.setmetatable")

    io.stderr:write("sandbox: allowlist + LUNA_SANDBOX_MODULES, " ..
        "no native loads, no subprocess/file-writes/env" ..
        ((fuel and fuel > 0) and (", fuel " .. fuel .. "M instr") or "") ..
        ((mem and mem > 0) and (", mem " .. mem .. "MiB") or "") ..
        "\n")
    io.stderr:flush()
end

-- fs is not in the default allowlist; when it is granted explicitly
-- (LUNA_SANDBOX_MODULES=fs), its write face gets the same treatment as
-- os.remove — reads stay open.
function M._harden_fs(fs)
    local denied = function(what)
        return function()
            error("sandbox: fs." .. what .. " is denied", 2)
        end
    end
    for _, name in ipairs({ "writeFileSync", "appendFileSync", "mkdirSync" }) do
        if fs[name] then
            fs[name] = denied(name)
        end
    end
    for _, name in ipairs({ "mkdir", "rmdir", "chdir", "touch", "lock", "unlock" }) do
        if fs[name] then
            fs[name] = denied(name)
        end
    end
end

return M
