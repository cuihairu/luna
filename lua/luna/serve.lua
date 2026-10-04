-- luna.serve: the attach channel.
--
-- The target process listens on a unix domain socket
-- ($LUNA_SOCK_DIR/luna-<pid>.sock, default /tmp). An attach client --
-- `luna --attach <pid>` -- connects, sends one line of code, and
-- SIGUSR1s the target; the target's poll points (REPL idle gaps and a
-- count hook around scripts) step this module, which accepts the
-- connection, reads the line, evaluates it in the target's live Lua
-- state (same globals, same Out, same package.loaded) and writes the
-- result back as a framed response:
--
--   "OK\n"  followed by repr lines, then "\30\n"     (success)
--   "ERR\n" followed by the error text, then "\30\n"  (failure)
--
-- The trailing newline after \30 keeps line-based readers (`*l`) from
-- blocking on a terminator-less last line.
--
-- Everything runs under pcall: a broken client or a raising command
-- never takes the target down. v1 serves one client at a time.
local kernel = require "kernel"
local intro = require "luna.introspect"

local ok_unix, socket_unix = pcall(require, "socket.unix")

local serve = {}

serve.srv = nil       -- listening socket
serve.path = nil      -- socket file path
serve.client = nil    -- current client socket (nil until accepted)
serve.enabled = true  -- cleared by --no-serve

function serve.path_for(pid)
    local dir = os.getenv("LUNA_SOCK_DIR") or "/tmp"
    return dir .. "/luna-" .. tostring(pid) .. ".sock"
end

-- Bind and listen. Returns ok, err; failures are quiet (no socket
-- support, unwritable dir) — luna works fine without attach.
function serve.start()
    if serve.srv then
        return true
    end
    if not serve.enabled then
        return nil, "serve disabled"
    end
    if not ok_unix then
        return nil, "socket.unix unavailable"
    end
    local path = serve.path_for(kernel.pid())
    local srv = socket_unix()
    os.remove(path) -- stale socket from a dead predecessor
    -- create-owner-only: tighten the umask around bind so the socket
    -- file never exists wider than 0600 (0777 &~ 077); the chmod below
    -- stays as belt-and-suspenders for systems that ignore umask here
    local oldmask = kernel.umask(tonumber("077", 8))
    local okb, err = srv:bind(path)
    kernel.umask(oldmask)
    if not okb then
        return nil, err
    end
    pcall(kernel.chmod, path, "600") -- owner-only, like the session
    srv:listen(1)
    srv:settimeout(0) -- non-blocking accept; polls never stall
    serve.srv, serve.path = srv, path
    -- the C count hook polls this while user code runs (REPL idle
    -- polls call serve.step directly)
    _G.__LUNA_SERVE_STEP = serve.step
    return true
end

local function drop_client()
    pcall(function() serve.client:close() end)
    serve.client = nil
end

-- Thin attach session: just enough of the REPL session shape for the
-- magics to work remotely (%hist reads inputs, %reset clears the out
-- registers). __attach marks the mode for magics that branch on it
-- (%load runs the file in the target instead of buffering it).
local attach_session = { inputs = {}, out = {}, out_n = 0, __attach = true }

-- Cap on captured output per command: a run-away print loop inside
-- the target must not grow an unbounded frame.
local MAX_CAPTURE = 64 * 1024

-- Install a capture sink: kernel output lands in acc AND mirrors to
-- the target's own stdout, so both consoles see the same stream.
local function capture_start(acc)
    kernel.sink(function(s)
        if #acc < MAX_CAPTURE then
            acc[#acc + 1] = s
        end
        io.write(s)
    end)
end

-- Assemble a status frame; body lines always end before the \30 line
-- so line-based readers never see a glued terminator.
local function frame(status, body)
    if body and body ~= "" then
        if body:sub(-1) ~= "\n" then
            body = body .. "\n"
        end
        return status .. "\n" .. body .. "\30\n"
    end
    return status .. "\n\30\n"
end

-- One command line from the attach client. Lines starting with the
-- \1 control byte are meta requests (completion candidates); % lines
-- dispatch through the target's own magic table; "?expr" / "expr?"
-- describe a value; everything else evaluates as Lua. While a command
-- runs, kernel output is captured and echoed to the client — a remote
-- session reads like the target console would.
local function dispatch(line)
    if line:sub(1, 1) == "\1" then
        local input = line:match("^%s*\1complete%s+(.-)%s*$") or ""
        local okc, complete = pcall(require, "luna.complete")
        if not okc then
            return frame("ERR", "completion engine unavailable")
        end
        local parts = {}
        for _, cand in ipairs(complete.line(input) or {}) do
            parts[#parts + 1] = tostring(cand)
        end
        return frame("OK", #parts > 0 and table.concat(parts, "\n") or nil)
    end

    attach_session.inputs[#attach_session.inputs + 1] = line

    if line:sub(1, 1) == "%" then
        local name, arg = line:match("^%%(%S+)%s*(.-)%s*$")
        if not name then
            return frame("ERR", "empty magic")
        end
        if name == "exit" then
            -- the attach client must not kill the target process
            return frame("EXIT", "%exit inside attach detaches the client")
        end
        local okm, magic = pcall(require, "luna.magic")
        if not okm then
            return frame("ERR", "magics unavailable")
        end
        local acc = {}
        capture_start(acc)
        -- dispatch never raises for a failed magic: it comes back as
        -- (nil, message) -- surfacing that here is what makes a typo'd
        -- or misused magic visible to the attach client at all
        local okd, okm, merr = pcall(magic.dispatch, attach_session, name, arg)
        kernel.sink(nil)
        if not okd or not okm then
            return frame("ERR", table.concat(acc) .. tostring(merr or okm))
        end
        return frame("OK", table.concat(acc))
    end

    local sugar = line:match("^%s*%?(.+)$")
    if not sugar then
        sugar = line:match("^(.-)%s*%?$")
    end
    if sugar and sugar ~= "" then
        local acc = {}
        capture_start(acc)
        local res = table.pack(kernel.exec("return " .. sugar, "=attach[help]"))
        kernel.sink(nil)
        if not res[1] then
            return frame("ERR", table.concat(acc) .. tostring(res[2]))
        end
        return frame("OK", table.concat(acc) .. intro.help(res[2]))
    end

    local acc = {}
    capture_start(acc)
    local res = table.pack(kernel.exec("return " .. line, "=attach"))
    if not res[1] then
        res = table.pack(kernel.exec(line, "=attach"))
    end
    kernel.sink(nil)
    if not res[1] then
        return frame("ERR", table.concat(acc) .. tostring(res[2]))
    end
    local parts = {}
    for i = 2, res.n do
        parts[#parts + 1] = intro.repr(res[i])
    end
    if #parts == 0 then
        parts[1] = "nil"
    end
    return frame("OK", table.concat(acc) .. table.concat(parts, "\n"))
end

-- One poll: accept a pending connection if any, drain readable input,
-- answer each complete line. Never blocks; never raises (all pcall).
function serve.step()
    if not serve.srv then
        return
    end
    if not serve.client then
        local okc, client = pcall(function() return serve.srv:accept() end)
        if okc and client then
            pcall(function() client:settimeout(0) end)
            serve.client = client
        end
    end
    while serve.client do
        local okr, chunk, rerr = pcall(function()
            return serve.client:receive("*l")
        end)
        if not okr then
            drop_client() -- receive() itself raised: treat as closed
            break
        end
        if chunk == nil then
            if rerr == "closed" then
                drop_client()
            end
            break -- timeout: no complete line yet; done for this poll
        end
        -- no debug.sethook() here: kernel.exec owns the hook slot for
        -- the nested exec (save/restore around its own count hook), so
        -- clearing it would strand whatever wrapper the caller had
        -- installed -- the coverage build's line tracer, for one
        local okd, reply = pcall(dispatch, chunk)
        if not okd then
            reply = "ERR\n" .. tostring(reply) .. "\n\30\n"
        end
        local oks, sent = pcall(function() return serve.client:send(reply) end)
        if not oks or not sent then
            drop_client()
            break
        end
    end
end

-- Shutdown (tests, and anyone toggling serve at runtime).
function serve.stop()
    if serve.client then
        drop_client()
    end
    if serve.srv then
        pcall(function() serve.srv:close() end)
        if serve.path then
            os.remove(serve.path)
        end
    end
    serve.srv, serve.path, serve.client = nil, nil, nil
end

-- luna ps: the attach channel's directory view. Scan the sock dir for
-- luna-<pid>.sock files; a socket file can outlive its process (SIGKILL,
-- crash), so every candidate pid goes through the kernel's kill(pid, 0)
-- liveness probe and dead ones are reported stale — visible, not hidden.
-- Rows: { pid, path, alive, cmdline? }; cmdline comes from /proc
-- (Linux only — elsewhere nil, and the socket path is shown instead).
-- Returns rows sorted by pid; the printing lives in psCli.
function serve.ps()
    local dir = os.getenv("LUNA_SOCK_DIR") or "/tmp"
    local rows = {}
    local okl, lfs = pcall(require, "lfs")
    if not okl or type(lfs) ~= "table" or type(lfs.dir) ~= "function" then
        return rows
    end
    local okd, iter, dirobj = pcall(lfs.dir, dir)
    if not okd or type(iter) ~= "function" then
        return rows -- unreadable sock dir: report an empty table
    end
    -- lfs' iterator takes the dir handle as the generic-for STATE
    -- (pcall split the (iter, state) pair, so hand the state back
    -- explicitly; dropping it reads as "directory metatable expected")
    for entry in iter, dirobj do
        local pid = entry:match("^luna%-(%d+)%.sock$")
        if pid then
            pid = tonumber(pid)
            local path = dir .. "/" .. entry
            local cmdline
            local fh = io.open("/proc/" .. pid .. "/cmdline", "r")
            if fh then
                cmdline = fh:read("a"):gsub("%z", " "):gsub("%s+$", "")
                fh:close()
            end
            rows[#rows + 1] = {
                pid = pid,
                path = path,
                alive = kernel.alive(pid) == true,
                cmdline = (cmdline ~= "" and cmdline) or nil,
            }
        end
    end
    table.sort(rows, function(a, b)
        return a.pid < b.pid
    end)
    return rows
end

-- CLI surface behind `luna ps` (luna.lua intercepts it before argparse,
-- like the rocks and serve commands). Read-only: no socket opens here,
-- the run leaves with 0 whether or not anything was found.
function serve.psCli()
    local rows = serve.ps()
    if #rows == 0 then
        io.write(string.format("no luna processes with an attach socket under %s\n",
            os.getenv("LUNA_SOCK_DIR") or "/tmp"))
        return 0
    end
    io.write(string.format("%-8s %-6s %s\n", "PID", "STATE", "COMMAND"))
    for _, r in ipairs(rows) do
        local what = r.cmdline or ("(socket " .. r.path .. ")")
        io.write(string.format("%-8d %-6s %s\n", r.pid,
            r.alive and "live" or "stale", what))
    end
    return 0
end

return serve
