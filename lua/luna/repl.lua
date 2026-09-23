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
local intro = require "luna.introspect"
local highlight = require "luna.highlight"

local repl = {}

-- ANSI accents for chrome (prompts, labels, errors); value rendering
-- goes through luna.highlight instead.
local RESET = "\27[0m"
local COLOR_IN = "\27[1;32m"  -- bold green: prompts, banner
local COLOR_OUT = "\27[1;35m" -- bold magenta: Out[n] labels
local COLOR_ERR = "\27[31m"   -- red: errors

local function emit(text)
    kernel.write(text)
end

local function paint(text, code, on)
    if not on then
        return text
    end
    return code .. text .. RESET
end

-- Display goes through luna.introspect: quoted strings, limited tables,
-- function signatures.
local function repr(v)
    return intro.repr(v)
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
    self.color = opts.color and true or false -- no-TTY degradation
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

    -- %magic commands: never valid Lua, intercept before evaluation.
    -- Lazy require keeps the session core runnable without the module.
    if line:sub(1, 1) == "%" then
        local mname, marg = line:match("^%%(%S+)%s*(.-)%s*$")
        if mname then
            self.in_n = self.in_n + 1
            self.inputs = self.inputs or {}
            self.inputs[self.in_n] = line
            local okm, magic = pcall(require, "luna.magic")
            if okm then
                local okd, err = magic.dispatch(self, mname, marg)
                if not okd then
                    emit(paint(tostring(err), COLOR_ERR, self.color) .. "\n")
                end
            end
            return "ok"
        end
    end

    -- IPython-style help sugar: "?expr" or "expr?". Neither form is
    -- valid Lua, so interception here is unambiguous.
    local help_target = line:match("^%s*%?+%s*(.-)%s*$")
    if help_target and help_target ~= "" then
        self.in_n = self.in_n + 1
        self:show_help(help_target)
        return "ok"
    end
    local trailing = line:match("^(.-)%s*%?+%s*$")
    if trailing and trailing ~= "" then
        self.in_n = self.in_n + 1
        self:show_help(trailing)
        return "ok"
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
    self.inputs = self.inputs or {}
    self.inputs[self.in_n] = line

    if status == "error" and not wrapped_ok then
        emit(paint(tostring(err), COLOR_ERR, self.color) .. "\n")
        return "error"
    end

    local src = wrapped_ok and ("return " .. chunk) or chunk

    local res = table.pack(kernel.exec(src, self.chunk_name .. "[" .. self.in_n .. "]"))
    if not res[1] then
        emit(paint(tostring(res[2]), COLOR_ERR, self.color) .. "\n")
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
        emit(paint("Out[" .. n .. "]: ", COLOR_OUT, self.color) ..
            highlight.render(table.concat(parts, "  "), self.color) .. "\n")
    end
    return "ok"
end

-- Evaluate expr and describe the resulting value (help sugar target).
function Session:show_help(expr)
    local res = table.pack(kernel.exec("return " .. expr, self.chunk_name .. "[help]"))
    if not res[1] then
        emit(paint(tostring(res[2]), COLOR_ERR, self.color) .. "\n")
        return
    end
    emit(intro.help(res[2]))
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
-- argt.color turns on ANSI chrome and value highlighting; it comes from
-- kernel.colors() && --no-color in the entry, so it is false without a
-- TTY unless LUNA_COLOR forces it.
function repl.run(argt)
    argt = argt or {}
    local session = repl.new({ color = argt.color })
    local tty = kernel.tty()

    -- Line editor (replxx bridge): editing, history recall, real-time
    -- highlighting and Tab completion. TTY only; piped stdin keeps the
    -- canonical io.read loop.
    local okl, linedit = pcall(require, "linedit")
    local editor = tty and okl
    local home = os.getenv("HOME")
    local hist_path = home and (home .. "/.luna_history") or nil
    if editor then
        linedit.set_completion(function(line)
            return session:completions(line)
        end)
        linedit.set_highlighter(function(line)
            return highlight.color_map(line)
        end)
        if hist_path then
            pcall(linedit.history_load, hist_path)
        end
    end

    if tty then
        emit(paint(kernel.version() .. " — " .. _VERSION ..
            " interactive console\n", COLOR_IN, session.color))
        emit("Type ^D to exit.\n")
    end

    -- interactive conveniences, IPython-style (not injected for plain
    -- script runs: the entry never calls run() there)
    _G.help = function(v)
        emit(intro.help(v))
    end
    _G.whos = function()
        emit(intro.whos())
    end

    while true do
        kernel.clear_interrupt()
        local line
        if editor then
            line = linedit.read(session:prompt())
        else
            io.write(paint(session:prompt(), COLOR_IN, session.color))
            io.stdout:flush()
            line = io.read("l")
        end
        if editor and line and line ~= "" then
            linedit.history_add(line)
        end
        local status = session:feed(line)
        if status == "done" then
            if editor and hist_path then
                pcall(linedit.history_save, hist_path)
            end
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
