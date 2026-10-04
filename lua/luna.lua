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
-- REPL and plugins can require them by name.
-- Chunkname: coverage builds set __LUNA_COV_ROOT so names point at the
-- real files (luacov only traces '@'-prefixed sources); otherwise the
-- plain '=(luna/x)' marker, as always.
local function luna_chunkname(name)
  local root = __LUNA_COV_ROOT
  if root then
    return "@" .. root .. "/lua/luna/" .. name .. ".lua"
  end
  return "=(luna/" .. name .. ")"
end
package.preload["luna.introspect"] = assert(load(__LUNA_INTROSPECT_SRC, luna_chunkname("introspect")))
package.preload["luna.complete"] = assert(load(__LUNA_COMPLETE_SRC, luna_chunkname("complete")))
package.preload["luna.highlight"] = assert(load(__LUNA_HIGHLIGHT_SRC, luna_chunkname("highlight")))
package.preload["luna.magic"] = assert(load(__LUNA_MAGIC_SRC, luna_chunkname("magic")))
package.preload["luna.modules"] = assert(load(__LUNA_MODULES_SRC, luna_chunkname("modules")))
package.preload["luna.plugins"] = assert(load(__LUNA_PLUGINS_SRC, luna_chunkname("plugins")))
package.preload["luna.rocks"] = assert(load(__LUNA_ROCKS_SRC, luna_chunkname("rocks")))
package.preload["luna.serve"] = assert(load(__LUNA_SERVE_SRC, luna_chunkname("serve")))
package.preload["luna.new"] = assert(load(__LUNA_NEW_SRC, luna_chunkname("new")))
package.preload["luna.sandbox"] = assert(load(__LUNA_SANDBOX_SRC, luna_chunkname("sandbox")))

-- Node-style resolution for project packages: relative requires and
-- bare names walking up luna_modules/ directories, manifests honored.
require("luna.modules").install()

-- Any .luna/rocks tree up the parent chain joins package.path/cpath so
-- scripts and the REPL can require installed rocks.
require("luna.rocks").inject_paths()

local repl = assert(load(__LUNA_REPL_SRC, luna_chunkname("repl")))()

local parser = argparse("luna", kernel.version() .. " — " ..
    _VERSION .. " interactive console and scripting runtime")
parser:epilog([[
examples:
  luna                          interactive console (like `node`)
  luna script.lua a b           run script.lua with args a b (like `lua`)
  luna -i script.lua            run script.lua, then drop into the console
  luna -e 'print(("x"):rep(3))' evaluate code and exit (like `node -e`)
  luna serve [dir] [port]       serve a directory over http (like
                                `python -m http.server`)
  luna ps                       list attachable luna processes
  luna --attach <pid>           open a console on a running luna's
                                live state (see also `luna ps`)
  luna new <template> <name>    scaffold a plugin, package, or script
                                (luna new --list)]])

parser:flag("-i --interactive",
    "after the script (or instead of it), start the interactive console")
parser:flag("-v --version", "print the version and exit")
parser:option("-e --eval",
    "evaluate code and exit; expression results echo Out[n]-style")
parser:flag("--no-color", "disable ANSI colors in output")
parser:flag("--no-plugins", "skip plugin discovery and loading")
parser:flag("--no-serve",
    "do not open the unix attach socket for `luna --attach`")
parser:flag("-S --sandbox",
    "restrict the session: module allowlist, no native loads, no " ..
    "subprocess/file-writes/env; LUNA_SANDBOX_FUEL / LUNA_SANDBOX_MEM " ..
    "cap compute and memory")
parser:option("--attach",
    "attach to a running luna's live state (its pid); interactive")
parser:argument("script", "a .lua script to run first"):args("?")
parser:argument("largs", "arguments passed to the script"):args("*")

-- Package management: `luna install|search|list|update ...` wraps the
-- vendored LuaRocks. Intercepted before argparse — their flags are
-- luarocks' own (--from-lock, --tree, ...), not this parser's. The
-- wrapper runs luarocks in-process and exits with its status; the
-- attach socket is not open yet at this point, so there is nothing to
-- clean up on the way out.
local ROCKS_CMDS = { install = true, search = true, list = true, update = true }
if arg[1] and ROCKS_CMDS[arg[1]] then
    os.exit(require("luna.rocks").dispatch(arg))
end

-- One-line static file server: `luna serve [dir] [port]` rides the
-- http module's serveCli (its flags are that usage's own, not this
-- parser's), intercepted before argparse like the rocks commands. The
-- attach socket never opens here — a file server has no live state to
-- attach to.
if arg[1] == "serve" then
    os.exit(require("http").serveCli({ table.unpack(arg, 2) }))
end

-- Process table: `luna ps` lists attachable luna processes (the attach
-- channel's directory view: pid, live/stale, command). Intercepted
-- before argparse like the rocks and serve commands; read-only — no
-- socket opens here.
if arg[1] == "ps" then
    os.exit(require("luna.serve").psCli())
end

-- Scaffolding: `luna new <template> <name>` writes a plugin, package,
-- or script starter from a built-in template. Intercepted before
-- argparse like the subcommands above; read/write only inside the
-- target paths (refuses to overwrite), the attach socket never opens.
if arg[1] == "new" then
    os.exit(require("luna.new").run({ table.unpack(arg, 2) }))
end

local opts = parser:parse(arg)

-- `luna --version` is the installers' post-install check: answer and
-- leave before anything starts (no plugins, no attach socket).
if opts.version then
    io.write(kernel.version() .. "\n")
    return 0
end

-- Sandbox mode installs before anything user-facing runs; plugins are
-- skipped outright (they inject package.preload entries, an allowlist
-- bypass). The runtime's own lazily-loaded pieces (the line editor the
-- console needs) are pulled in while the real require still exists.
if opts.sandbox then
    pcall(require, "linedit") -- the console's line editor, pre-guard
    pcall(require, "luna.serve") -- the attach channel: its sock-dir
        -- snapshot and socket.unix both load while require/env/cpath
        -- are still real (the channel itself stays open as ops path)
    require("luna.sandbox").install()
end

-- Directory plugins load for every launch mode (they may inject
-- modules a script needs); --no-plugins skips them.
if not opts.no_plugins and not opts.sandbox then
    require("luna.plugins").load_all()
end

-- The attach socket opens for every mode except the attach client
-- itself (--no-serve opts out). Failures are quiet: attach is a
-- convenience, not a prerequisite.
if not opts.no_serve and not opts.attach then
    require("luna.serve").start()
end

-- One SIGINT convention across the one-shot launch modes: 128 + SIGINT,
-- whatever was running when ^C landed. The message has already gone to
-- the caller's own sink (stderr for scripts, the session emitter for
-- -e); this only picks the status.
local function exit_code_for(err)
    if tostring(err):find("interrupted", 1, true) then
        return 130
    end
    return 1
end

-- Script-tail auto-drain (batch 8): after a script's (or -e's) main
-- chunk ends normally, run the event loop until it empties — Node's
-- "the event loop runs until nothing is left" contract, so a plain
-- loop.setTimeout without an explicit loop.run() still fires. No-op
-- for scripts that never touched the loop; all-unref'd handles exit
-- at once. The REPL deliberately does NOT drain (its three sync
-- contracts — line-edit blocking read, ^C/130, attach polling —
-- stand). A ^C landing mid-drain gets the same 130 as the script body.
local function maybe_drain()
    local ok, err = pcall(function()
        local loop = package.loaded.loop
        if loop and loop.maybeDrain then
            loop.maybeDrain()
        end
    end)
    if not ok then
        io.stderr:write(tostring(err) .. "\n")
        return exit_code_for(err)
    end
    return 0
end

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

    -- While the script runs, the kernel's count hook polls the attach
    -- socket (installed by serve.start), so an attach client reaches a
    -- busy — even looping — script.

    local ok, err = kernel.exec(src, "@" .. path, table.unpack(script_args, 1, #script_args))
    _G.arg = old_arg
    if not ok then
        io.stderr:write(tostring(err) .. "\n")
        return exit_code_for(err)
    end
    return maybe_drain()
end

-- Evaluate -e code through a session so expressions echo Out[n]-style.
-- Unlike interactive input, trailing incomplete chunks are an error:
-- there is no next line to continue with.
local function run_eval(code)
    local session = repl.new({ chunk_name = "eval",
        color = kernel.colors() and not opts.no_color })
    local status, err = session:feed(code)
    if status == "continue" then
        io.stderr:write("luna: -e code is incomplete\n")
        return 1
    end
    if status == "error" then
        -- the session already reported the message; a ^C that lands
        -- mid-chunk gets the same 130 a script run would get
        return exit_code_for(err)
    end
    return maybe_drain()
end

-- Attach console: line-edit locally, execute remotely in the target's
-- live state. After each send, SIGUSR1 interrupts the target's blocked
-- line editor so its poll picks the command up. Framed replies end at
-- the \30 byte; ^D or %detach leaves.
local function run_attach(pid)
    local oks, socket_unix = pcall(require, "socket.unix")
    if not oks then
        io.stderr:write("luna: attach needs socket.unix (bundled)\n")
        return 1
    end
    local serve = require("luna.serve")
    local client = socket_unix()
    local okc, cerr = client:connect(serve.path_for(pid))
    if not okc then
        io.stderr:write("luna: cannot attach to " .. tostring(pid) .. ": " ..
            tostring(cerr) .. "\n")
        return 1
    end
    client:settimeout(30)
    local okl, linedit = pcall(require, "linedit")
    local editor = kernel.tty() and okl
    if editor then
        -- Tab completion runs remotely: send the meta line, wake the
        -- target's editor, and turn the framed candidate list back
        -- into the shape linedit expects.
        linedit.set_completion(function(input)
            local ok_s = pcall(function()
                return client:send("\1complete " .. input .. "\n")
            end)
            if not ok_s then
                return {}
            end
            pcall(kernel.wake, tonumber(pid))
            local acc = {}
            while true do
                local ok_r, repart = pcall(function()
                    return client:receive("*l")
                end)
                if not ok_r or repart == nil or repart == "\30" then
                    break
                end
                acc[#acc + 1] = repart
            end
            if acc[1] ~= "OK" then
                return {}
            end
            -- the completion bridge wants a table, not varargs
            return { table.unpack(acc, 2) }
        end)
    end
    io.write("attached to " .. pid .. " — %detach or ^D to leave\n")
    while true do
        local line
        if editor then
            line = linedit.read("attach> ")
        else
            io.write("attach> ")
            io.stdout:flush()
            line = io.read("l")
        end
        if not line then
            break -- ^D
        end
        line = line:match("^%s*(.-)%s*$")
        if line == "%detach" then
            break
        end
        if line ~= "" then
            local ok_s = pcall(function() return client:send(line .. "\n") end)
            if not ok_s then
                io.stderr:write("attach: target went away\n")
                return 1
            end
            pcall(kernel.wake, tonumber(pid))
            local acc = {}
            while true do
                local ok_r, repart, rerr = pcall(function()
                    return client:receive("*l")
                end)
                if not ok_r or repart == nil then
                    io.stderr:write("attach: no reply (" ..
                        tostring(ok_r and rerr or repart) .. ")\n")
                    return 1
                end
                if repart == "\30" then
                    break
                end
                acc[#acc + 1] = repart
            end
            -- first line is the status frame; the rest is the body —
            -- errors go to stderr so pipes see a clean result stream
            local status = acc[1]
            local body = table.concat(acc, "\n", 2)
            if status == "OK" then
                print(body)
            elseif status == "EXIT" then
                -- the target refused to die: %exit means detach here
                print(body)
                break
            else
                io.stderr:write(body .. "\n")
            end
        end
    end
    return 0
end

-- Interactive console. Works on a TTY (line-based until the line editor
-- lands) and degrades gracefully to piped stdin. Colors require a TTY
-- (kernel.colors, which honors NO_COLOR/LUNA_COLOR) and stay off with
-- --no-color.
local function run_console()
    return repl.run({ color = kernel.colors() and not opts.no_color })
end

-- Single exit path: drop the attach socket file, then hand the status
-- back to main(). Returning (instead of os.exit) lets the state close
-- properly — GC finalizers run, open files get flushed by their own
-- __gc — and lets the caller's exit code path see the result.
local function finish(code)
    require("luna.serve").stop()
    return code
end

if opts.attach then
    return finish(run_attach(opts.attach))
elseif opts.eval then
    return finish(run_eval(opts.eval))
elseif opts.script then
    local code = run_script(opts.script, opts.largs)
    if code ~= 0 then
        return finish(code)
    end
    if opts.interactive then
        return finish(run_console())
    end
    return finish(0)
else
    return finish(run_console())
end
