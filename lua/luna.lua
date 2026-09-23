-- luna entry: CLI definition (argparse) and launch-mode dispatch.
--
-- Modes, following python/node conventions:
--   luna script.lua args   run a script and exit (args become its `...`)
--   luna -i script.lua     run the script, then drop into the console
--   luna                   start the interactive console (like node)
--   luna -e 'expr'         evaluate code and exit (REPL-style echo)
--
-- The REPL stays the entry point: node ships a REPL, so does luna.
local argparse = require "argparse"
local kernel = require "kernel"

-- embedded policy modules (see cmake/luna_lua.h.in); preloaded so the
-- REPL and plugins can require them by name
package.preload["luna.introspect"] = assert(load(__LUNA_INTROSPECT_SRC, "=(luna/introspect)"))
package.preload["luna.complete"] = assert(load(__LUNA_COMPLETE_SRC, "=(luna/complete)"))
local repl = assert(load(__LUNA_REPL_SRC, "=(luna/repl)"))()

local parser = argparse("luna", kernel.version() .. " — " ..
    _VERSION .. " interactive console and scripting runtime")
parser:epilog([[
examples:
  luna                          interactive console (like `node`)
  luna script.lua a b           run script.lua with args a b (like `lua`)
  luna -i script.lua            run script.lua, then drop into the console
  luna -e 'print(("x"):rep(3))' evaluate code and exit (like `node -e`)]])

parser:flag("-i --interactive",
    "after the script (or instead of it), start the interactive console")
parser:option("-e --eval",
    "evaluate code and exit; expression results echo Out[n]-style")
parser:flag("--no-color", "disable ANSI colors in output")
parser:argument("script", "a .lua script to run first"):args("?")
parser:argument("largs", "arguments passed to the script"):args("*")

local opts = parser:parse(arg)

-- Run a script file: args become the chunk's `...` (standalone lua
-- convention), arg[] is rebuilt for the duration of the run.
local function run_script(path, script_args)
    local old_arg = _G.arg
    _G.arg = { [0] = path, table.unpack(script_args, 1, #script_args) }

    local fh, read_err = io.open(path, "r")
    if not fh then
        io.stderr:write("luna: cannot open " .. path .. ": " .. tostring(read_err) .. "\n")
        _G.arg = old_arg
        return 1
    end
    local src = fh:read("a")
    fh:close()

    local ok, err = kernel.exec(src, "@" .. path, table.unpack(script_args, 1, #script_args))
    _G.arg = old_arg
    if not ok then
        io.stderr:write(tostring(err) .. "\n")
        if tostring(err):find("interrupted", 1, true) then
            return 130 -- SIGINT convention: 128 + SIGINT
        end
        return 1
    end
    return 0
end

-- Evaluate -e code through a session so expressions echo Out[n]-style.
-- Unlike interactive input, trailing incomplete chunks are an error:
-- there is no next line to continue with.
local function run_eval(code)
    local session = repl.new({ chunk_name = "eval" })
    local status = session:feed(code)
    if status == "continue" then
        io.stderr:write("luna: -e code is incomplete\n")
        return 1
    end
    if status == "error" then
        return 1
    end
    return 0
end

-- Interactive console. Works on a TTY (line-based until the line editor
-- lands) and degrades gracefully to piped stdin.
local function run_console()
    return repl.run({ color = not opts.no_color })
end

if opts.eval then
    os.exit(run_eval(opts.eval))
elseif opts.script then
    local code = run_script(opts.script, opts.largs)
    if code ~= 0 then
        os.exit(code)
    end
    if opts.interactive then
        os.exit(run_console())
    end
    os.exit(0)
else
    os.exit(run_console())
end
