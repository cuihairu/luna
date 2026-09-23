-- luna.repl: the interactive session core.
--
-- A Session turns submitted lines into executed chunks. Multi-line
-- input is handled by accumulating lines until kernel.check() stops
-- reporting "incomplete"; expression lines are re-loaded as `return
-- <chunk>` so their values echo back like IPython's Out[n].
--
-- All output goes through kernel.write() (funnel: sink function or
-- stdout), so sessions run headless in tests.
local kernel = require "kernel"
local complete = require "luna.complete"

local repl = {}

local function emit(text)
    kernel.write(text)
end

-- Plain tostring-based display for now; luna.help.repr takes over
-- once introspection lands.
local function repr(v)
    return tostring(v)
end

local Session = {}
Session.__index = Session

function repl.new(opts)
    opts = opts or {}
    local self = setmetatable({}, Session)
    self.pending = nil       -- accumulated lines of an incomplete chunk
    self.in_n = 0            -- inputs executed
    self.out_n = 0           -- echoed results
    self.out = {}            -- Out[n] -> first value, IPython-style
    self.last = nil          -- _
    self.chunk_name = opts.chunk_name or "repl"
    return self
end

function Session:build(line)
    if not self.pending then
        return line
    end
    return self.pending .. "\n" .. line
end

-- feed(line) -> "ok" | "continue" | "error" | "done"
-- nil line means EOF.
function Session:feed(line)
    if line == nil then
        return "done"
    end
    if line == "" and not self.pending then
        return "ok" -- blank input: nothing to do, not numbered
    end

    local chunk = self:build(line)
    local status, err = kernel.check(chunk, self.chunk_name)
    -- Chunks are statement lists, so a bare expression like "_", "1+2"
    -- or "f()" does not compile on its own (often with an <eof> error
    -- that also looks like truncation). `return <chunk>` is the probe:
    -- if it compiles, the input is a complete expression, not an
    -- unfinished statement.
    local wrapped_ok = kernel.check("return " .. chunk, self.chunk_name) == "ok"

    if status == "incomplete" and not wrapped_ok then
        self.pending = chunk
        return "continue"
    end
    self.pending = nil
    self.in_n = self.in_n + 1

    if status == "error" and not wrapped_ok then
        emit(err .. "\n")
        return "error"
    end

    local src = wrapped_ok and ("return " .. chunk) or chunk

    local res = table.pack(kernel.exec(src, self.chunk_name .. "[" .. self.in_n .. "]"))
    if not res[1] then
        emit(tostring(res[2]) .. "\n")
        return "error"
    end

    local has_value = false
    for i = 2, res.n do
        if res[i] ~= nil then
            has_value = true
            break
        end
    end
    if has_value then
        self.out_n = self.out_n + 1
        local n = self.out_n
        self.out[n] = res[2]
        _G.Out = _G.Out or {}
        _G.Out[n] = res[2]
        _G.__ = _G._  -- second-to-last, IPython-style
        _G._ = res[2] -- last result
        local parts = {}
        for i = 2, res.n do
            parts[#parts + 1] = repr(res[i])
        end
        emit("Out[" .. n .. "]: " .. table.concat(parts, "  ") .. "\n")
    end
    return "ok"
end

function Session:prompt()
    if self.pending then
        return "... "
    end
    return "In [" .. (self.in_n + 1) .. "]: "
end

-- Tab completion hook for the line editor (replxx callbacks call this
-- through the kernel). Candidates are insert-strings.
function Session:completions(line)
    return complete.line(line)
end

-- Interactive driver. Editing and history come with the line-editor
-- integration; for now the terminal's canonical mode reads lines.
function repl.run(argt)
    argt = argt or {}
    local session = repl.new()
    local tty = kernel.tty()

    if tty then
        emit(kernel.version() .. " — " .. _VERSION .. " interactive console\n")
        emit("Type ^D to exit.\n")
    end

    while true do
        kernel.clear_interrupt()
        if tty then
            io.write(session:prompt())
            io.stdout:flush()
        end
        local line = io.read("l")
        local status = session:feed(line)
        if status == "done" then
            if tty then
                emit("\n")
            end
            return 0
        end
    end
end

repl.Session = Session
repl.emit = emit
repl.repr = repr

return repl
