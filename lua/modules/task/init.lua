-- task: structured async tasks — coroutines suspended at await, timers
-- from loop.setTimeout. P2 async model: no new loop.xxx, no C changes —
-- the whole model sits on setTimeout/clearTimeout and script-tail drain.
--
-- require "task" pulls loop in (opt-in face, same as require "loop").
-- The script tail auto-drains, so sleeping tasks keep the process alive;
-- the REPL never drains, so awaited flows belong to script / -e usage —
-- inside the console only pure-compute tasks run to completion.
--
-- Yield protocol: a body runs in a coroutine. task.await yields the
-- AWAIT sentinel plus the promise; the scheduler resumes with
-- (true, ...) on resolve or (false, reason) on reject/cancel. A raw
-- coroutine.yield that does not hand back the sentinel ends the task
-- as an error — there is no second yield channel to keep straight.
local loop = require "loop"

local task = {}

local AWAIT = {} -- yield marker: coroutine.yield(AWAIT, promise)
local CANCELLED = {} -- error object raised inside a cancelled task
setmetatable(CANCELLED, {
    __tostring = function() return "task cancelled" end,
})

local PROMISE = {}
PROMISE.__index = PROMISE

local next_id = 0
local live = {} -- id -> handle (tasks not yet terminal)
local by_co = {} -- coroutine -> record
-- handle -> record, weak on the handle: the record lives exactly as
-- long as its handle is referenced (live tasks are referenced by
-- live[]), and the handle itself stays a clean data table — no
-- internals leaking into Out[n] reprs.
local rec_of = setmetatable({}, { __mode = "k" })
local counters = { started = 0, done = 0, error = 0, cancelled = 0 }

-- kick: resume a task coroutine and classify the outcome. Called on
-- spawn, on promise settle, and on cancel. Never lets a body error
-- escape into the loop callback that resumed it.
local kick

local function terminal(rec, status, err, results)
    rec.status = status
    rec.terminal = true
    rec.handle.status = status
    rec.handle.awaiting = nil
    rec.waiting_on = nil
    by_co[rec.co] = nil
    live[rec.id] = nil
    counters[status] = counters[status] + 1
    if status == "done" then
        rec.handle.results = results
    elseif status == "error" then
        rec.handle.err = err
        io.stderr:write(string.format("task #%d failed (%s): %s\n",
            rec.id, rec.src, tostring(err)))
    end
end

kick = function(rec, ...)
    local r = table.pack(coroutine.resume(rec.co, ...))
    if not r[1] then
        if r[2] == CANCELLED then
            terminal(rec, "cancelled")
        else
            terminal(rec, "error", r[2])
        end
        return
    end
    if coroutine.status(rec.co) == "dead" then
        terminal(rec, "done", nil, table.pack(table.unpack(r, 2, r.n)))
        return
    end
    if r[2] ~= AWAIT then
        terminal(rec, "error", string.format(
            "raw coroutine.yield in a task (first value %s) — await through task.await/promise",
            type(r[2])))
        return
    end
    local p = r[3]
    if type(p) ~= "table" or getmetatable(p) ~= PROMISE then
        terminal(rec, "error", "await handed back a non-promise")
        return
    end
    -- await already registered the waiter and the awaiting tag; the
    -- promise settles later (timer, resolve elsewhere) and kicks us.
    rec.waiting_on = p
    rec.handle.awaiting = p._kind
end

-- promise: one-shot settle/reject. Settling twice raises in the
-- setter's frame; waiters all resume (usually one).
local function promise_settle(p, ok, ...)
    if p._settled then
        error("task.promise: settled twice", 2)
    end
    p._settled = true
    p._ok = ok
    p._vals = table.pack(...)
    local waiters = p._waiters
    p._waiters = {}
    for i = 1, #waiters do
        local rec = waiters[i]
        rec.waiting_on = nil
        rec.handle.awaiting = nil
        kick(rec, ok, table.unpack(p._vals, 1, p._vals.n))
    end
end

function PROMISE:resolve(...)
    promise_settle(self, true, ...)
end

function PROMISE:reject(err)
    promise_settle(self, false, err)
end

function task.promise()
    return setmetatable({ _settled = false, _waiters = {} }, PROMISE)
end

-- await: suspend the current task until the promise settles. Resolved
-- values come back as returns; a rejection raises the reason (or the
-- CANCELLED sentinel after handle:cancel).
function task.await(p)
    if type(p) ~= "table" or getmetatable(p) ~= PROMISE then
        error("task.await expects a task.promise()", 2)
    end
    local rec = by_co[coroutine.running()]
    if not rec then
        error("task.await outside a task — start one with task.run", 2)
    end
    if not p._settled then
        p._waiters[#p._waiters + 1] = rec
        rec.waiting_on = p
        rec.handle.awaiting = p._kind
        local r = table.pack(coroutine.yield(AWAIT, p))
        if not r[1] then
            error(r[2], 0)
        end
        return table.unpack(r, 2, r.n)
    end
    if p._ok then
        return table.unpack(p._vals, 1, p._vals.n)
    end
    error(p._vals[1], 0)
end

-- sleep: one timer + await. The record keeps the timer handle so
-- cancel can clearTimeout it before the callback ever fires.
function task.sleep(ms)
    if type(ms) ~= "number" or ms < 0 then
        error("task.sleep expects a non-negative delay in ms", 2)
    end
    local rec = by_co[coroutine.running()]
    if not rec then
        error("task.sleep outside a task — start one with task.run", 2)
    end
    local p = task.promise()
    p._kind = "sleep"
    rec.timer = loop.setTimeout(function()
        rec.timer = nil
        if not p._settled then
            p:resolve()
        end
    end, ms)
    return task.await(p)
end

-- run: start fn in a coroutine immediately (it runs until its first
-- await or completion before run returns) and hand back the handle.
local HANDLE = {}
HANDLE.__index = HANDLE

-- cancel: cooperative. Only a task suspended in await can be cancelled;
-- the CANCELLED error lands at its await point, so the body can catch
-- it and clean up — swallowing it keeps the task alive (honest status).
function HANDLE:cancel()
    local rec = rec_of[self]
    if not rec or rec.terminal then
        return false
    end
    if coroutine.status(rec.co) ~= "suspended" or not rec.waiting_on then
        return false
    end
    local p = rec.waiting_on
    local waiters = p._waiters
    for i = #waiters, 1, -1 do
        if waiters[i] == rec then
            table.remove(waiters, i)
            break
        end
    end
    rec.waiting_on = nil
    self.awaiting = nil
    if rec.timer then
        loop.clearTimeout(rec.timer)
        rec.timer = nil
    end
    kick(rec, false, CANCELLED)
    return true
end

function task.run(fn, ...)
    if type(fn) ~= "function" then
        error("task.run expects a function", 2)
    end
    local args = table.pack(...)
    next_id = next_id + 1
    local info = debug.getinfo(fn, "S")
    local rec = {
        id = next_id,
        co = coroutine.create(fn),
        status = "running",
        terminal = false,
        src = info and (info.short_src .. ":" .. info.linedefined) or "?",
    }
    local handle = setmetatable({
        id = rec.id,
        status = "running",
        src = rec.src,
        awaiting = nil,
        results = nil,
        err = nil,
    }, HANDLE)
    rec_of[handle] = rec
    rec.handle = handle
    by_co[rec.co] = rec
    live[rec.id] = handle
    counters.started = counters.started + 1
    kick(rec, table.unpack(args, 1, args.n))
    return handle
end

-- promisify: err-first callback style fn -> wrapper returning a
-- promise. First settle wins; a synchronous throw rejects unless the
-- callback already settled (then the throw is reported, not swallowed).
function task.promisify(fn)
    if type(fn) ~= "function" then
        error("task.promisify expects a function", 2)
    end
    return function(...)
        local args = table.pack(...)
        local p = task.promise()
        local settled = false
        local ok, err = pcall(fn, table.unpack(args, 1, args.n), function(e, ...)
            if settled then
                return
            end
            settled = true
            if e ~= nil then
                p:reject(e)
            else
                p:resolve(...)
            end
        end)
        if not ok and not settled then
            settled = true
            p:reject(err)
        elseif not ok then
            io.stderr:write("task: promisified function raised after settling: "
                .. tostring(err) .. "\n")
        end
        return p
    end
end

-- list/stats: the %tasks data source. list holds live handles only
-- (terminal tasks drop out immediately — errors already went to stderr
-- once and stay readable on the handle the caller kept).
function task.list()
    local out = {}
    for _, h in pairs(live) do
        out[#out + 1] = h
    end
    table.sort(out, function(a, b)
        return a.id < b.id
    end)
    return out
end

function task.stats()
    local s = {
        started = counters.started,
        done = counters.done,
        error = counters.error,
        cancelled = counters.cancelled,
        live = 0,
        awaiting = 0,
        running = 0,
    }
    for _, h in pairs(live) do
        s.live = s.live + 1
        if h.awaiting then
            s.awaiting = s.awaiting + 1
        else
            s.running = s.running + 1
        end
    end
    return s
end

return task
