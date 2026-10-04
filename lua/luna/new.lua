-- luna new: scaffold a plugin, package, or script from a built-in
-- template.
--
-- Templates are string generators, not on-disk files: they ride the
-- same embedding path as every other policy module, so the binary can
-- scaffold anywhere without a sidecar. Dispatch happens before
-- argparse (luna.lua), alongside the rocks/serve/ps interceptions.
local M = {}

-- Names become paths and JSON fields; anything outside this set (and
-- any ".." run) is rejected before a byte is written.
local NAME_PATTERN = "^[%w][%w%._-]*$"

local templates = {}

templates.plugin = {
    desc = "directory plugin: plugin.json + init.lua (./plugins/ discovery)",
    make = function(name)
        local cmd = (name:gsub("[^%w_]", "_"))
        return {
            { path = name .. "/plugin.json", content = ([[
{
  "name": "%s",
  "version": "0.1.0",
  "description": "%s — a luna plugin",
  "main": "init.lua"
}
]]):format(name, name) },
            { path = name .. "/init.lua", content = ([[
-- %s — luna plugin entry.
-- Discovery: put this directory in ./plugins/, ~/.luna/plugins, or a
-- directory named by $LUNA_PLUGIN_PATH; luna reads plugin.json and
-- runs this entry on startup. Guide: docs/guide/plugins.md.
local magic = require("luna.magic")

magic.register("%s", function(session, arg)
    print("hello from the %s plugin")
end, "%%%s — say hi from the %s plugin")

return {}
]]):format(name, cmd, name, cmd, name) },
        }
    end,
}

templates.package = {
    desc = "luna package: package.json + init.lua (luna_modules/ resolution)",
    make = function(name)
        return {
            { path = name .. "/package.json", content = ([[
{
  "name": "%s",
  "version": "0.1.0",
  "main": "init.lua"
}
]]):format(name) },
            { path = name .. "/init.lua", content = ([[
-- %s — luna package entry (manifest main = "init.lua").
-- Consume: place this directory at <project>/luna_modules/%s/, then
-- require("%s") resolves it from anywhere in the project.
local M = {}

function M.greet(who)
    return "hello, " .. tostring(who or "world")
end

return M
]]):format(name, name, name) },
        }
    end,
}

templates.script = {
    desc = "single-file CLI script: <name>.lua (args as varargs, arg[0] = path)",
    make = function(name)
        return {
            { path = name .. ".lua", content = ([[
-- %s — luna script. Run: luna %s.lua [args...]
-- Chunk varargs are the script's arguments; arg[0] is this path.
local args = { ... }

if #args == 0 or args[1] == "-h" or args[1] == "--help" then
    io.stderr:write("usage: luna %s.lua [args...]\n")
    os.exit(#args == 0 and 1 or 0)
end

print(("hello, %%s!"):format(args[1]))
print(("got %%d arg(s)"):format(#args))
]]):format(name, name, name) },
        }
    end,
}

local function list(out)
    out:write("templates:\n")
    local names = {}
    for kind in pairs(templates) do
        names[#names + 1] = kind
    end
    table.sort(names)
    for _, kind in ipairs(names) do
        out:write(string.format("  %-8s %s\n", kind, templates[kind].desc))
    end
end

local function usage(out)
    out:write("usage: luna new <template> <name>   (see templates below)\n")
    list(out)
end

function M.run(argv)
    local fs = require("fs")

    if argv[1] == "--list" then
        list(io.stdout)
        return 0
    end

    local kind, name = argv[1], argv[2]
    if not kind or not name then
        usage(io.stderr)
        return 1
    end
    local tpl = templates[kind]
    if not tpl then
        io.stderr:write("luna new: unknown template '" .. kind .. "'\n")
        usage(io.stderr)
        return 1
    end
    if not name:match(NAME_PATTERN) or name:find("..", 1, true) then
        io.stderr:write("luna new: bad name '" .. name ..
            "' (letters, digits, '.', '_', '-'; no '..')\n")
        return 1
    end

    local files = tpl.make(name)
    for _, f in ipairs(files) do
        if fs.exists(f.path) then
            io.stderr:write("luna new: refusing to overwrite " .. f.path .. "\n")
            return 1
        end
    end

    if kind ~= "script" and not fs.exists(name) then
        fs.mkdirSync(name)
    end
    for _, f in ipairs(files) do
        fs.writeFileSync(f.path, f.content)
    end

    io.write("created " .. kind .. " '" .. name .. "':\n")
    for _, f in ipairs(files) do
        io.write("  " .. f.path .. "\n")
    end
    if kind == "plugin" then
        io.write("next: move " .. name .. "/ into ./plugins/ (or ~/.luna/plugins), then run `luna` and try %" ..
            (name:gsub("[^%w_]", "_")) .. "\n")
    elseif kind == "package" then
        io.write("next: move " .. name .. "/ into <project>/luna_modules/, then require(\"" .. name .. "\")\n")
    else
        io.write("next: luna " .. name .. ".lua world\n")
    end
    return 0
end

return M
