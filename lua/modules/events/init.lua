--- @module events
-- events: Node EventEmitter 语义。events.new() 构造, 方法挂在实例上
-- (on/once/off/prepend*/listeners/listenerCount/setMaxListeners/emit/
-- removeAllListeners); events.defaultMaxListeners = 10 (可改, 影响此后
-- 所有添加判定)。语义按 Node v24 实证钉版, 详见 docs/node-parity.md:
--   emit 返回是否有监听; newListener 在添加前发出 (监听数不含新者),
--   removeListener 在移除后发出; removeAllListeners(ev) 逐个 (LIFO)
--   发 removeListener, 无参全清不发;
--   超限警告经 io.stderr, 每 (emitter, 事件名) 一次, 移除/重设后重置;
--   emit('error') 无监听 → raise (表负载原样, 标量包 "Unhandled error.")。

local Events = { defaultMaxListeners = 10 }

local Emitter = {}
Emitter.__index = Emitter

local function new()
    return setmetatable({ _l = {}, _max = nil, _warned = {} }, Emitter)
end

local function check_name(fn, name)
    if type(name) ~= "string" then
        error(("events.%s: event name must be a string, got %s")
            :format(fn, type(name)), 3)
    end
end

local function check_fn(fn, f)
    if type(f) ~= "function" then
        error(("events.%s: listener must be a function, got %s")
            :format(fn, type(f)), 3)
    end
end

-- 分发当前快照 (emit 期间的自移除/重挂都作用于活表, 本轮照常调用)
local function raw_fire(em, name, ...)
    local list = em._l[name]
    if not list or #list == 0 then
        return false
    end
    local snap = {}
    for i = 1, #list do
        snap[i] = list[i]
    end
    for i = 1, #snap do
        local e = snap[i]
        if e.once then
            for j = 1, #list do
                if list[j] == e then
                    table.remove(list, j)
                    break
                end
            end
            if #list == 0 then
                em._l[name] = nil
            end
        end
        e.fn(...)
    end
    return true
end

local function add(em, name, fn, once, prepend)
    check_name(once and "once" or "on", name)
    check_fn(once and "once" or "on", fn)
    -- newListener 在添加前发出 (监听数不含新者)
    raw_fire(em, "newListener", name, fn)
    local list = em._l[name] or {}
    em._l[name] = list
    local e = { fn = fn, once = once }
    if prepend then
        table.insert(list, 1, e)
    else
        list[#list + 1] = e
    end
    local max = em._max
    if max == nil then
        max = Events.defaultMaxListeners
    end
    if max > 0 and #list > max and not em._warned[name] then
        em._warned[name] = true
        io.stderr:write((
            "MaxListenersExceededWarning: Possible EventEmitter memory leak"
            .. " detected. %d %s listener%s added to [EventEmitter]."
            .. " MaxListeners is %d. Use emitter:setMaxListeners() to"
            .. " increase limit\n"):format(
            #list, name, #list == 1 and "" or "s", max))
    end
    return em
end

function Emitter:on(name, fn)
    return add(self, name, fn, false, false)
end

Emitter.addListener = Emitter.on

function Emitter:once(name, fn)
    return add(self, name, fn, true, false)
end

function Emitter:prependListener(name, fn)
    return add(self, name, fn, false, true)
end

function Emitter:prependOnceListener(name, fn)
    return add(self, name, fn, true, true)
end

function Emitter:removeListener(name, fn)
    check_name("removeListener", name)
    check_fn("removeListener", fn)
    local list = self._l[name]
    if list then
        for i = 1, #list do
            if list[i].fn == fn then
                table.remove(list, i)
                if #list == 0 then
                    self._l[name] = nil
                end
                self._warned[name] = nil -- 重臂警告 (Node 的写时复制数组同效)
                raw_fire(self, "removeListener", name, fn)
                break
            end
        end
    end
    return self
end

Emitter.off = Emitter.removeListener

function Emitter:removeAllListeners(name)
    if name == nil then
        self._l = {}
        self._warned = {}
        return self
    end
    check_name("removeAllListeners", name)
    local list = self._l[name]
    if list then
        -- LIFO 逐个移除并各发一次 removeListener (Node 同序)
        for i = #list, 1, -1 do
            local fn = list[i].fn
            table.remove(list, i)
            if #list == 0 then
                self._l[name] = nil
            end
            raw_fire(self, "removeListener", name, fn)
        end
        self._warned[name] = nil
    end
    return self
end

function Emitter:listeners(name)
    check_name("listeners", name)
    local list = self._l[name]
    local out = {}
    if list then
        for i = 1, #list do
            out[i] = list[i].fn
        end
    end
    return out
end

function Emitter:listenerCount(name)
    check_name("listenerCount", name)
    local list = self._l[name]
    return list and #list or 0
end

function Emitter:setMaxListeners(n)
    if type(n) ~= "number" then
        error(("events.setMaxListeners: expects a number, got %s")
            :format(type(n)), 2)
    end
    self._max = n -- 0/负数 = 不限 (Node 同口径)
    self._warned = {}
    return self
end

function Emitter:emit(name, ...)
    check_name("emit", name)
    local list = self._l[name]
    if not list or #list == 0 then
        if name == "error" then
            local v = select(1, ...)
            if type(v) == "table" then
                error(v)
            end
            if v == nil then
                error("Unhandled 'error' event", 0)
            end
            if type(v) == "string" then
                error(("Unhandled error. ('%s')"):format(v), 0)
            end
            if type(v) == "number" then
                local util = require "util"
                error("Unhandled error. (" .. util.inspect(v) .. ")", 0)
            end
            error(("Unhandled error. (%s)"):format(tostring(v)), 0)
        end
        return false
    end
    return raw_fire(self, name, ...)
end

Events.new = new
Events.EventEmitter = Events -- Node 的 events.EventEmitter 形状别名

return Events
