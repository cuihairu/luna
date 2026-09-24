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

-- Evaluate one command line in the live state; returns the framed
-- response. The caller suspends and reinstalls any count hook around
-- step(): kernel.exec installs its own SIGINT hook while it runs.
local function dispatch(line)
    local res = table.pack(kernel.exec("return " .. line, "=attach"))
    if not res[1] then
        res = table.pack(kernel.exec(line, "=attach"))
    end
    if not res[1] then
        return "ERR\n" .. tostring(res[2]) .. "\n\30\n"
    end
    local parts = {}
    for i = 2, res.n do
        parts[#parts + 1] = intro.repr(res[i])
    end
    if #parts == 0 then
        parts[1] = "nil"
    end
    return "OK\n" .. table.concat(parts, "\n") .. "\n\30\n"
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
        debug.sethook() -- let exec own the hooks for a moment
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

return serve
