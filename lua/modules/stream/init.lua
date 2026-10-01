--- @module stream
-- stream: Node 流的 v1 收窄面, 纯 Lua, 地基是 events。
-- 构造: stream.readable / stream.writable / stream.duplex / stream.transform
-- (各自吃 opts 表), 适配器 stream.duplexFromSock / stream.readableFromChunks /
-- stream.pushReadable。语义按 Node v24 钉版 + 本模块的勘定(详见
-- docs/node-parity.md「stream」节):
--   Readable: opts._read(self, size) 是拉式源 (内部缓冲空且在流动时被调,
--     一次不交货即等下次 push); self:push(chunk) 灌数据 (返回 false = 已到
--     highWaterMark, 源头该收手), push(nil) 即 EOF;'data'/'end'/
--     'readable'/'pause'/'resume' 事件; pause/resume 切流动; 加 'data'
--     监听自动转流动。
--   Writable: opts._write(self, chunk, cb) 是汇, cb(err) 报完成; write 返回
--     false = 缓冲到 highWaterMark (此后队列排空发一次 'drain');
--     end(chunk, cb) 收尾 (end 是关键字, 拼作 s:end_(...) 或 s["end"]),
--     全部落盘后 'finish'。
--   Duplex = 两半共存; Transform 的 opts._transform(self, chunk, cb) 中间
--     变换 (cb(err) 结束本片, 可多次 self:push), 可选 opts._flush(self, cb),
--     end 后先 flush 再 'finish', 读侧随缓冲排空发 'end'。
--   背压: readable:pipe(writable) 里 writable:write 返回 false 即 pause,
--     等 'drain' 续推; unpipe 摘钩子。错误联动 (与 Node 裸 pipe 的差异,
--     见勘定): 源 'error' 会 destroy 目标并 unpipe, 目标 'error' 只 unpipe。
--   error → destroy → 'close': destroy(err) 先发 'error' 再发 'close',
--     幂等; 'end'/'finish' 后自动 destroy (autoDestroy 口径)。
--   记账: 字符串按字节, 其余值按 1 计 (objectMode 恒开的简化);
--     highWaterMark 缺省 16384, 只做阈值不做字节精确。

local events = require "events"
local Emitter = getmetatable(events.new()) -- events 的方法表, 流继承它

local DEFAULT_HWM = 16384

-- 尺寸记账: 字符串按 #, 其余 (数字/表等) 按 1
local function sizeof(chunk)
    if type(chunk) == "string" then
        return #chunk
    end
    return 1
end

local function check_opt_fn(what, f, required)
    if f == nil and not required then
        return
    end
    if type(f) ~= "function" then
        error(("stream: %s must be a function, got %s")
            :format(what, type(f)), 3)
    end
end

local function check_hwm(opts)
    local h = opts and opts.highWaterMark
    if h ~= nil and type(h) ~= "number" then
        error(("stream: highWaterMark must be a number, got %s")
            :format(type(h)), 3)
    end
    return h or DEFAULT_HWM
end

-- ---------- 公共底座: destroy / 事件继承 ----------

local Stream = {}
Stream.__index = Stream
setmetatable(Stream, { __index = Emitter })

-- destroy(err): 幂等收口。err 非 nil 先发 'error' (无监听则 raise,
-- events 语义), 恒发一次 'close'; opts._destroy(self, err) 先行摘外部
-- 资源 (如 sock)。缓冲与未决回调就地清账。
function Stream:destroy(err)
    if self._closed then
        return self
    end
    self._closed = true
    self._destroyed = true
    if self._opts._destroy then
        local ok, e = pcall(self._opts._destroy, self, err)
        if not ok then
            io.stderr:write(("stream: _destroy error: %s\n")
                :format(tostring(e)))
        end
    end
    -- 读侧清缓冲
    self._bufR, self._bufferedR = {}, 0
    -- 写侧: 未决回调以错误结账 (在途一笔 + 排队若干)
    local flight = self._wflight
    self._wflight = nil
    local pending = self._bufW
    self._bufW, self._bufferedW, self._wbusy = {}, 0, false
    if flight and flight.cb then
        flight.cb("stream destroyed")
    end
    if pending then
        for i = 1, #pending do
            local cb = pending[i].cb
            if cb then
                cb("stream destroyed")
            end
        end
    end
    if err ~= nil then
        self:emit("error", err)
    end
    self:emit("close")
    return self
end

function Stream:isPaused()
    return not self._flowing
end

-- autoDestroy 的收口判据: 纯流一张脸到位即关; duplex/transform 两张脸
-- ('end' 与 'finish') 都到齐才关 (Node 同款)。
function Stream:_autoclose()
    if self._closed then
        return
    end
    local rdone = not self._hasr or self._renddone
    local wdone = not self._hasw or self._wfinished
    if rdone and wdone then
        self:destroy()
    end
end

-- ---------- Readable ----------

local Readable = {}
Readable.__index = Readable
setmetatable(Readable, { __index = Stream })

-- newListener 钩在 events 里, 但它在监听器落位**前**发出 —— 在里面开流
-- 会把头几块数据丢给空气。所以改覆写 on/once: 'data' 监听真正挂上后才
-- 转流动 (Node 同款语义, Node 靠 resumeScheduled 延一拍, 我们靠挂后发车)。
local function readable_new(opts, mt)
    opts = opts or {}
    local self = events.new()
    setmetatable(self, { __index = mt or Readable })
    self._opts = opts
    self._hwmR = check_hwm(opts)
    self._bufR, self._bufferedR = {}, 0
    self._flowing = nil -- nil 初始 / true 流动 / false 暂停
    self._started = false -- 转流动过一次
    self._ended = false -- push(nil) 已到
    self._reading = false -- 在 _read 里 (push 只入账不投递)
    self._pushseq = 0 -- 每次 push 自增: _read 有无产出的判据
    self._pipes = {} -- { { dest = w, hook... } }
    self._hasr = true
    check_opt_fn("_read", opts._read)
    return self
end

-- 首个 'data' 监听挂上 → 转流动
function Readable:_start()
    self._started = true
    self._flowing = true
    self:emit("resume")
    self:_pump()
    self:_want_read()
end

local function arm_start(self, name, was)
    if name == "data" and was then
        self:_start()
    end
end

function Readable:on(name, fn)
    local was = self._flowing == nil and not self._started
    local r = Emitter.on(self, name, fn)
    arm_start(self, name, was)
    return r
end

Readable.addListener = Readable.on

function Readable:once(name, fn)
    local was = self._flowing == nil and not self._started
    local r = Emitter.once(self, name, fn)
    arm_start(self, name, was)
    return r
end

-- 把块灌进内部缓冲; nil 即 EOF。返回是否还有余量 (false = 到 hwm,
-- 源头应停手 —— 背压的另一半)。在 _read 里调用时只入账, 投递由
-- _want_read 的循环收口, 免得拉式源递归无底。
function Readable:push(chunk)
    if self._destroyed then
        return false
    end
    if chunk == nil then
        self._ended = true
        if self._flowing and not self._reading then
            self:_pump()
        end
        return false
    end
    if self._ended then
        self:emit("error", "push after EOF")
        return false
    end
    self._bufR[#self._bufR + 1] = chunk
    self._bufferedR = self._bufferedR + sizeof(chunk)
    self._pushseq = self._pushseq + 1
    if not self._flowing then
        self:emit("readable") -- 暂停态有数据到位, 通知持有人
    elseif not self._reading then
        self:_pump()
    end
    return self._bufferedR < self._hwmR
end

-- 投递缓冲为 'data'; refill = 投递到空后是否回头补货 (want_read 的
-- 循环自己补, 不再经 pump 触发)。监听器里 pause() 即刻生效。
function Readable:_pump(refill)
    while self._flowing and not self._destroyed and #self._bufR > 0 do
        local chunk = table.remove(self._bufR, 1)
        self._bufferedR = self._bufferedR - sizeof(chunk)
        self:emit("data", chunk)
    end
    if self._destroyed or #self._bufR > 0 then
        return
    end
    if self._ended then
        self:emit("end")
        self._renddone = true
        self:_autoclose()
    elseif self._flowing and refill ~= false then
        self:_want_read()
    end
end

-- 内部缓冲空且在流动时向源要数据: _read 一次不产出 (同步 push 没发生)
-- 就返回等下次 push; 产出了就投递再续要, 直到源收口或暂停。
function Readable:_want_read()
    while not self._destroyed and not self._ended
        and self._flowing and #self._bufR == 0 do
        local src = self._opts._read
        if not src or self._reading then
            return
        end
        local seq = self._pushseq
        self._reading = true
        local ok, err = pcall(src, self, self._hwmR)
        self._reading = false
        if not ok then
            self:destroy(err) -- 源炸 → error → close 联动
            return
        end
        if self._pushseq == seq and #self._bufR == 0
            and not self._ended then
            return -- 源暂无产出: 等下一次 push 触发
        end
        self:_pump(false)
    end
end

function Readable:pause()
    if self._flowing == false then
        return self
    end
    self._flowing = false
    self:emit("pause")
    return self
end

function Readable:resume()
    if self._flowing == true then
        return self
    end
    self._flowing = true
    self:emit("resume")
    self:_pump()
    return self
end

-- pipe(w, opts): 数据面 + 背压 + 错误联动; 返回 w。
--   w:write 返回 false → 源 pause, w 'drain' → 源 resume;
--   源 'end' → w:end_() (opts.end == false 可关);
--   源 'error' → unpipe 后 w:destroy(err) (勘定的联动, 裸 Node 不带);
--   w 'error' / w 'close' → 只 unpipe。
function Readable:pipe(w, opts)
    if (type(w) ~= "table" and type(w) ~= "userdata") then
        error(("stream.pipe: destination must be a table or userdata, "
            .. "got %s"):format(type(w)), 2)
    end
    check_opt_fn("pipe destination's write", w.write, true)
    local hook = { dest = w }
    hook.ondata = function(chunk)
        if w:write(chunk) == false then
            self:pause()
        end
    end
    hook.ondrain = function()
        if self._flowing == false then
            self:resume()
        end
    end
    hook.onend = function()
        if not (opts and opts["end"] == false) and not w._destroyed then
            w:end_()
        end
    end
    hook.onsrcerr = function(err)
        self:unpipe(w)
        w:destroy(err)
    end
    hook.ondsterr = function()
        self:unpipe(w)
    end
    hook.onwclose = function()
        self:unpipe(w)
    end
    self._pipes[#self._pipes + 1] = hook
    -- 'data' 最后挂: 挂上的一瞬间同步源就可能开流投递, 那时其余
    -- 钩子 (end/error/drain) 必须已经就位
    w:on("drain", hook.ondrain)
    self:on("end", hook.onend)
    self:on("error", hook.onsrcerr)
    w:on("error", hook.ondsterr)
    w:on("close", hook.onwclose)
    self:on("data", hook.ondata)
    if self._flowing == nil then
        self._flowing = true
        self:emit("resume")
    end
    self:_pump()
    self:_want_read()
    return w
end

function Readable:unpipe(w)
    local removed = false
    for i = #self._pipes, 1, -1 do
        local hook = self._pipes[i]
        if w == nil or hook.dest == w then
            self:off("data", hook.ondata)
            self:off("end", hook.onend)
            self:off("error", hook.onsrcerr)
            hook.dest:off("drain", hook.ondrain)
            hook.dest:off("error", hook.ondsterr)
            hook.dest:off("close", hook.onwclose)
            table.remove(self._pipes, i)
            removed = true
        end
    end
    if removed and #self._pipes == 0
        and #self:listeners("data") == 0 then
        self:pause() -- 没有消费者了, 停在原地
    end
    return self
end

-- ---------- Writable ----------

local Writable = {}
Writable.__index = Writable
setmetatable(Writable, { __index = Stream })

local function writable_new(opts)
    opts = opts or {}
    local self = events.new()
    setmetatable(self, { __index = Writable })
    self._opts = opts
    self._hwmW = check_hwm(opts)
    self._bufW, self._bufferedW = {}, 0
    self._wbusy = false
    self._wended = false -- end() 已调
    self._needdrain = false
    self._hasw = true
    check_opt_fn("_write", opts._write)
    check_opt_fn("_final", opts._final)
    return self
end

-- 队头出队送 _write; cb(err) 结账后看是否 'drain'/'finish'
function Writable:_process()
    if self._wbusy or self._destroyed then
        return
    end
    local front = self._bufW[1]
    if not front then
        if self._wended then
            self:_finish()
        elseif self._needdrain then
            self._needdrain = false
            self:emit("drain")
        end
        return
    end
    self._wbusy = true
    table.remove(self._bufW, 1)
    self._bufferedW = self._bufferedW - sizeof(front.chunk)
    self._wflight = front
    local sink = self._opts._write
    local done = false
    local function settle(e)
        if done then
            return
        end
        done = true
        self._wflight = nil
        self._wbusy = false
        if self._destroyed then
            return
        end
        if e then
            self:destroy(e)
            return
        end
        if front.cb then
            front.cb()
        end
        self:_process()
    end
    local ok, err = pcall(sink, self, front.chunk, settle)
    if not ok then
        settle(err)
    end
end

-- write(chunk, cb) → boolean: false = 已到 hwm, 队列排空会发 'drain'
function Writable:write(chunk, cb)
    if self._destroyed then
        self:emit("error", "write after destroy")
        return false
    end
    if self._wended then
        self:emit("error", "write after end")
        return false
    end
    check_opt_fn("write callback", cb)
    self._bufW[#self._bufW + 1] = { chunk = chunk, cb = cb }
    self._bufferedW = self._bufferedW + sizeof(chunk)
    self:_process()
    local ok = self._bufferedW < self._hwmW
    if not ok then
        self._needdrain = true
    end
    return ok
end

-- end(chunk?, cb?): 末块入队, 全部结账后 'finish' (transform 另有 flush)
function Writable:end_(chunk, cb)
    if self._destroyed then
        return self
    end
    if chunk ~= nil then
        self._bufW[#self._bufW + 1] = { chunk = chunk, cb = nil }
        self._bufferedW = self._bufferedW + sizeof(chunk)
    end
    if cb then
        self:on("finish", cb)
    end
    self._wended = true
    self:_process()
    return self
end

Writable["end"] = Writable.end_ -- end 是关键字; s["end"] 形状可用

function Writable:_finish()
    if self._wfinished then
        return
    end
    self._wfinished = true
    self:emit("finish")
    self:_autoclose()
end

-- ---------- Duplex: 两半各记账, 一套事件 ----------

local Duplex = {}
Duplex.__index = Duplex
setmetatable(Duplex, { __index = Readable })
-- 写半的方法混进来 (Lua 无多继承, 就地拷一份)
for k, v in pairs(Writable) do
    if Duplex[k] == nil and k ~= "__index" then
        Duplex[k] = v
    end
end

local function duplex_new(opts, mt)
    opts = opts or {}
    local self = readable_new(opts, mt or Duplex)
    self._hasw = true -- 写半也记账, 两脸到齐才 autoclose
    self._hwmW = check_hwm({ highWaterMark =
        opts.writableHighWaterMark or opts.highWaterMark })
    self._bufW, self._bufferedW = {}, 0
    self._wbusy = false
    self._wended = false
    self._needdrain = false
    check_opt_fn("_write", opts._write)
    check_opt_fn("_final", opts._final)
    return self
end

-- ---------- Transform: _transform 中继, end 先 flush 再 finish ----------

local Transform = {}
Transform.__index = Transform
setmetatable(Transform, { __index = Duplex })

local function transform_new(opts)
    opts = opts or {}
    check_opt_fn("_transform", opts._transform, true)
    check_opt_fn("_flush", opts._flush)
    local self = duplex_new(opts, Transform)
    self._tpending = 0 -- 在途 _transform 计数
    return self
end

-- 复用 Writable 的 write; _process 换成 _transform 中继
function Transform:_process()
    if self._wbusy or self._destroyed then
        return
    end
    local front = self._bufW[1]
    if not front then
        if self._wended then
            if self._tpending == 0 then
                self:_flush_and_finish()
            end
        elseif self._needdrain then
            self._needdrain = false
            self:emit("drain")
        end
        return
    end
    self._wbusy = true
    table.remove(self._bufW, 1)
    self._bufferedW = self._bufferedW - sizeof(front.chunk)
    self._wflight = front
    self._tpending = self._tpending + 1
    local fn = self._opts._transform
    local done = false
    local function settle(e)
        if done then
            return
        end
        done = true
        self._tpending = self._tpending - 1
        self._wflight = nil
        self._wbusy = false
        if self._destroyed then
            return
        end
        if e then
            self:destroy(e)
            return
        end
        if front.cb then
            front.cb()
        end
        self:_process()
    end
    local ok, err = pcall(fn, self, front.chunk, settle)
    if not ok then
        settle(err)
    end
end

function Transform:_flush_and_finish()
    if self._wfinished then
        return
    end
    local flush = self._opts._flush
    if not flush then
        self:_finish() -- 'finish' 先行, 读侧 'end' 随缓冲排空跟上
        self:_end_readable()
        return
    end
    self._wbusy = true
    local done = false
    local function settle(e)
        if done then
            return
        end
        done = true
        self._wbusy = false
        if self._destroyed then
            return
        end
        if e then
            self:destroy(e)
            return
        end
        self:_finish()
        self:_end_readable()
    end
    local ok, err = pcall(flush, self, settle)
    if not ok then
        settle(err)
    end
end

-- 读侧收口: 'end' 由 pump 在缓冲排空后发
function Transform:_end_readable()
    self._ended = true
    if self._flowing then
        self:_pump()
    end
end

-- ---------- 适配器 ----------

-- sock (loop.net) → Duplex: 常驻读回调转 push, write 转发,
-- destroy 时关 sock, 读错误/EOF 走 error→destroy / push(nil) 联动
local function stream_duplexFromSock(sock)
    if type(sock) ~= "table" and type(sock) ~= "userdata" then
        error(("stream.duplexFromSock: expects a sock, got %s")
            :format(type(sock)), 2)
    end
    check_opt_fn("sock.write", sock.write, true)
    check_opt_fn("sock.read", sock.read, true)
    local d = duplex_new({
        _write = function(self, chunk, cb)
            sock:write(chunk, function(e)
                if e then
                    cb(e)
                else
                    cb()
                end
            end)
        end,
        _destroy = function(self)
            sock:close()
        end,
    })
    sock:read(function(e, chunk)
        if d._destroyed then
            return
        end
        if e then
            d:destroy(e)
        elseif chunk == nil then
            d:push(nil)
        else
            d:push(chunk)
        end
    end)
    return d
end

-- 内存块 → Readable: chunks 是字符串或块表, 一次一块按需推
-- (fs.readFileSync 的结果一行就是一个内存块)
local function stream_readableFromChunks(chunks)
    if type(chunks) == "string" then
        chunks = { chunks }
    end
    if type(chunks) ~= "table" then
        error(("stream.readableFromChunks: expects a string or a table, "
            .. "got %s"):format(type(chunks)), 2)
    end
    local i, n = 0, #chunks
    return readable_new({
        _read = function(self)
            i = i + 1
            if i <= n then
                self:push(chunks[i])
            else
                self:push(nil)
            end
        end,
    })
end

-- 推式 Readable (loop.http 的 onData 模式): 生产者直接 :push(chunk),
-- 返回 false 即收手; 缓冲排空时 _read 被调, 生产者可在此续灌自己的
-- 队列 —— "视余量"就是这个返回值加这个回灌点。
local function stream_pushReadable(opts)
    return readable_new(opts) -- _read 缺省即"无拉面", push 全公开
end

-- ---------- 模块表 ----------

local stream = {
    DEFAULT_HWM = DEFAULT_HWM,
    readable = readable_new,
    writable = writable_new,
    duplex = duplex_new,
    transform = transform_new,
    duplexFromSock = stream_duplexFromSock,
    readableFromChunks = stream_readableFromChunks,
    pushReadable = stream_pushReadable,
    -- Node 形状别名: new stream.Readable(opts) → stream.Readable(opts)
    Readable = readable_new,
    Writable = writable_new,
    Duplex = duplex_new,
    Transform = transform_new,
}

return stream
