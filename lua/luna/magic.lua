-- luna.magic: % commands, IPython-style.
--
-- Registry first: plugins (and users at runtime) can add commands with
-- magic.register(name, run, help) where run(session, arg) may emit
-- through the session. Lines starting with % never parse as Lua, so
-- the REPL intercepts them before evaluation.
local kernel = require "kernel"
local intro = require "luna.introspect"

local magic = {}

magic.commands = {}

function magic.register(name, run, help)
    if type(name) ~= "string" or type(run) ~= "function" then
        error("magic.register expects (string, function, string?)", 2)
    end
    magic.commands[name] = { run = run, help = help }
    return true
end

-- dispatch(session, name, arg) -> ok | nil, err
function magic.dispatch(session, name, arg)
    local cmd = magic.commands[name]
    if not cmd then
        return nil, "unknown magic: %" .. name .. " (try %help)"
    end
    local ok, res = pcall(cmd.run, session, arg)
    if not ok then
        return nil, tostring(res)
    end
    return true
end

-- Echo a result list with Out bookkeeping (shared by %time/%timeit).
-- res is a table.pack result: [1]=ok, [2..n]=values.
function magic.echo_result(session, res)
    local has_value = false
    for i = 2, res.n do
        if res[i] ~= nil then
            has_value = true
            break
        end
    end
    if not has_value then
        return
    end
    session.out_n = session.out_n + 1
    local n = session.out_n
    session.out[n] = res[2]
    _G.Out = _G.Out or {}
    _G.Out[n] = res[2]
    _G.__ = _G._
    _G._ = res[2]
    local parts = {}
    for i = 2, res.n do
        parts[#parts + 1] = intro.repr(res[i])
    end
    kernel.write("Out[" .. n .. "]: " .. table.concat(parts, "  ") .. "\n")
end

-- "%time <code>": expression form gets the Out[n] echo; statement form
-- just runs. The wrapped probe mirrors what the session does with
-- input lines (expressions recompile as `return <code>`).
local function runnable(code)
    if kernel.check("return " .. code, "=magic") == "ok" then
        return "return " .. code
    end
    return code
end

magic.register("time", function(session, arg)
    if arg == "" then
        error("usage: %time <expression or code>")
    end
    local src = runnable(arg)
    local t0 = kernel.millis()
    local res = table.pack(kernel.exec(src, "=magic[time]"))
    local dt = kernel.millis() - t0
    if not res[1] then
        error(tostring(res[2]))
    end
    if src:sub(1, 7) == "return " then
        magic.echo_result(session, res)
    end
    kernel.write(string.format("Wall time: %.3f ms\n", dt))
end, "%time <expr> — run expr/code once and report wall time")

magic.register("timeit", function(session, arg)
    if arg == "" then
        error("usage: %timeit <expression>")
    end
    local src = runnable(arg)
    local reps, best = 0, math.huge
    local t0 = kernel.millis()
    while reps < 1000 and kernel.millis() - t0 < 100 do
        local r0 = kernel.millis()
        local res = table.pack(kernel.exec(src, "=magic[timeit]"))
        if not res[1] then
            error(tostring(res[2]))
        end
        best = math.min(best, kernel.millis() - r0)
        reps = reps + 1
    end
    kernel.write(string.format("%d loops, best of session: %.3f ms per loop\n", reps, best))
end, "%timeit <expr> — repeated runs, reports per-loop time")

magic.register("hist", function(session, arg)
    local lo, hi = 1, session.in_n
    if arg ~= "" then
        local a, b = arg:match("^(%d+)%s*-%s*(%d+)$")
        local one = arg:match("^(%d+)$")
        if a then
            lo, hi = tonumber(a), tonumber(b)
        elseif one then
            lo, hi = tonumber(one), tonumber(one)
        else
            error("usage: %hist [first-last | n]")
        end
    end
    for i = lo, hi do
        local line = session.inputs and session.inputs[i]
        if line then
            kernel.write(string.format("In [%d]: %s\n", i, line))
        end
    end
end, "%hist [range] — print session inputs (e.g. %hist 2-5)")

magic.register("whos", function(session)
    kernel.write(intro.whos())
end, "%whos — list current globals (like whos())")

magic.register("reset", function(session)
    -- keep the runtime environment captured at startup (stdlibs,
    -- kernel, lpeg, ...); clear only user state
    local base = _G.__LUNA_BASE_GLOBALS or {}
    for name, _ in pairs(_G) do
        if type(name) == "string" and not base[name]
            and not name:match("^__LUNA") then
            _G[name] = nil
        end
    end
    _G.Out = {}
    _G._ = nil
    _G.__ = nil
    -- In[n] is the input history (what %hist reads), not a result
    -- register — IPython's %reset keeps In too. The sweep above takes
    -- it like any post-startup global, so rebuild it from the session
    -- log, the same source %hist reads.
    _G.In = {}
    for i, line in pairs(session.inputs or {}) do
        _G.In[i] = line
    end
    session.out = {}
    session.out_n = 0
    session.pending = nil
    kernel.write("session state reset\n")
end, "%reset — clear user globals and Out registers")

magic.register("clear", function()
    if kernel.tty() then
        kernel.write("\27[2J\27[H")
    end
end, "%clear — clear the terminal screen")

magic.register("exit", function()
    os.exit(0)
end, "%exit — leave the console (same as ^D)")

magic.register("plugins", function()
    local okp, plugs = pcall(require, "luna.plugins")
    if not okp then
        kernel.write("plugins: loader unavailable\n")
        return
    end
    local names = {}
    for name in pairs(plugs.loaded) do
        names[#names + 1] = name
    end
    table.sort(names)
    if #names == 0 then
        kernel.write("no plugins loaded\n")
    else
        kernel.write("loaded plugins:\n")
        for _, name in ipairs(names) do
            local m = plugs.loaded[name]
            kernel.write(string.format("  %s %s  %s\n", name,
                m.version or "-", m.description or ""))
        end
    end
    local bad = 0
    for _ in pairs(plugs.failed) do
        bad = bad + 1
    end
    if bad > 0 then
        kernel.write(string.format("%d plugin(s) failed to load (see stderr)\n", bad))
    end
    local shadowed = {}
    for name, dir in pairs(plugs.overridden or {}) do
        shadowed[#shadowed + 1] = { name = name, dir = dir }
    end
    if #shadowed > 0 then
        table.sort(shadowed, function(a, b)
            return a.name < b.name
        end)
        kernel.write("overridden by an earlier same-name plugin:\n")
        for _, o in ipairs(shadowed) do
            kernel.write(string.format("  %s  %s\n", o.name, o.dir))
        end
    end
end, "%plugins — list loaded plugins, failures, and shadowed names")

magic.register("help", function()
    local names = {}
    for name in pairs(magic.commands) do
        names[#names + 1] = name
    end
    table.sort(names)
    kernel.write("magic commands:\n")
    for _, name in ipairs(names) do
        local cmd = magic.commands[name]
        kernel.write("  %" .. name .. "  " .. (cmd.help or "") .. "\n")
    end
end, "%help — list magic commands")

return magic
