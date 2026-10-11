-- console: Node console 面 (v24.21.0 形状对拍, 纯 Lua)。
--   log/info/debug/warn/error/assert/trace/dir/table/time/timeLog/
--   timeEnd/count/countReset/group/groupCollapsed/groupEnd/clear/
--   timeStamp/profile/profileEnd + Console 构造器 (自备流)。
--   单元格式化走 util.format, 单元渲染走 util.inspect; stdout 走
--   process.stdout (kernel.write 漏斗), stderr 走 process.stderr
--   (直写即刷)。
--   已知偏差 (账记):
--   * assert 的真值集按 Node 收口 (falsy = nil/false/0/''/NaN——
--     Lua 本只 nil/false 为假, 语言边界);
--   * time/timeLog/timeEnd 分辨率到毫秒 (kernel.millis), Node 纳秒
--     出三位小数——luna 整毫秒出 "5ms" 形 (Node 整毫秒同形 "5");
--   * table 单元与 dir 走 util.inspect (Lua 源形态, 键序确定性)——
--     Node 是 JS inspect (单引号 'two'、{ n: 1 } 形态); 列宽按 UTF-8
--     码点计 (Node 按显示宽, CJK/emoji 单元会窄);
--   * Lua 无 Set/Map, "(iteration index)" 列不会出现;
--   * 行形二分: #t > 0 按数组行 (下标 0 起, 两栖表只看数组部),
--     否则按对象行 (键字节序); function 行是空键对象行 (Node
--     typeof 'function' 无键同款); 含 nil 洞的数组按 #v 截断;
--   * 列并全序: 编号列数值序 → 字符串键列字节序, Values 殿后
--     (Node 是 Object.keys 插入序, 整键先升序);
--   * properties 的列即属性名单 (给定序, 可重复, 选取只对表行
--     生效; 数组行的命名属性 Lua 取不到, 串行的下标选取不生效);
--   * trace 的栈是 debug.traceback 形态 (Node 是 V8 栈);
--   * clear 探不到 stdout TTY (kernel.tty 只探 fd0), 恒 no-op
--     (Node 非 TTY 同款行为, 真终端不抹屏);
--   * 告警无 "(node:NNNN)" 进程前缀与 trace-warnings 提示;
--   * console.table 非 table 实参走 log 口径 (format, 非 inspect),
--     properties 非表 raise (Node 是 ERR_INVALID_ARG_TYPE, 文本不同);
--   * 不立项: Inspector/color 面 (profile/profileEnd/timeStamp 恒
--     no-op, Node 无 Inspector 时同形)。

local M = {}

local kernel = require("kernel")
local process = require("process")
local util = require("util")
local format = util.format
local inspect = util.inspect

-- ------------------------------------------------------------------
-- Console 类: 构造器收两个带 write(s) 的流表 (Node Console(stream
-- [, stderr]) 同形); 单例挂 process.stdout/stderr。
-- ------------------------------------------------------------------

local ConsoleC = {}
ConsoleC.__index = ConsoleC

function M.Console(stdout, stderr)
    if type(stdout) ~= "table" or type(stdout.write) ~= "function" then
        error("console.Console: stdout must have a write function", 0)
    end
    if stderr ~= nil
        and (type(stderr) ~= "table" or type(stderr.write) ~= "function") then
        error("console.Console: stderr must have a write function", 0)
    end
    return setmetatable({
        _out = stdout,
        _err = stderr or stdout,
        _indent = "",
        _times = {},
        _counts = {},
    }, ConsoleC)
end

local function emit(self, stream, s)
    stream.write(self._indent .. s .. "\n")
end

local function warn_line(self, s)
    emit(self, self._err, s)
end

-- ------------------------------------------------------------------
-- log 族: 格式化走 util.format (Node format 语义, 既有对拍)
-- ------------------------------------------------------------------

function ConsoleC:log(...)
    emit(self, self._out, format(...))
end
ConsoleC.info = ConsoleC.log
ConsoleC.debug = ConsoleC.log

function ConsoleC:warn(...)
    warn_line(self, format(...))
end
ConsoleC.error = ConsoleC.warn

-- assert: 假值告警不抛 (Node v10+ 同款)。真值走 Node falsy 集
-- (nil/false/0/''/NaN 触发——Lua 只 nil/false 为假, 语言边界在此
-- 按 Node 面收口)。无参 "Assertion failed"; 首参串
-- "Assertion failed: <format(args)>"; 首参非串
-- "Assertion failed <format(args)>" (探针口径)
local function js_falsy(v)
    if v == nil or v == false then return true end
    if v == 0 or v == "" then return true end
    return type(v) == "number" and v ~= v -- NaN
end

function ConsoleC:assert(value, ...)
    if not js_falsy(value) then return end
    local msg
    if select("#", ...) == 0 then
        msg = "Assertion failed"
    elseif type((...)) == "string" then
        msg = "Assertion failed: " .. format(...)
    else
        msg = "Assertion failed " .. format(...)
    end
    warn_line(self, msg)
end

-- trace: "Trace"[/": <format>"] + Lua 栈 (形态账记)
function ConsoleC:trace(...)
    local head = format(...)
    local msg = head == "" and "Trace" or ("Trace: " .. head)
    emit(self, self._err, msg .. "\n" .. debug.traceback("", 2))
end

-- dir: inspect 透传 (opts 契约同 util.inspect)
function ConsoleC:dir(obj, opts)
    if opts ~= nil and type(opts) ~= "table" then
        error("console.dir: opts must be a table, got " .. type(opts), 0)
    end
    emit(self, self._out, inspect(obj, opts))
end

-- ------------------------------------------------------------------
-- time / count 计数族 (kernel.millis, 毫秒分辨率账记)
-- ------------------------------------------------------------------

local function norm_label(label)
    return label == nil and "default" or tostring(label)
end

function ConsoleC:time(label)
    label = norm_label(label)
    if self._times[label] then
        warn_line(self, ("Warning: Label '%s' already exists for console.time()")
            :format(label))
        return
    end
    self._times[label] = kernel.millis()
end

function ConsoleC:timeLog(label)
    label = norm_label(label)
    local t0 = self._times[label]
    if not t0 then
        warn_line(self, ("Warning: No such label '%s' for console.timeLog()")
            :format(label))
        return
    end
    emit(self, self._out,
        ("%s: %sms"):format(label, tostring(kernel.millis() - t0)))
end

function ConsoleC:timeEnd(label)
    label = norm_label(label)
    local t0 = self._times[label]
    if not t0 then
        warn_line(self, ("Warning: No such label '%s' for console.timeEnd()")
            :format(label))
        return
    end
    self._times[label] = nil
    emit(self, self._out,
        ("%s: %sms"):format(label, tostring(kernel.millis() - t0)))
end

function ConsoleC:count(label)
    label = norm_label(label)
    local n = (self._counts[label] or 0) + 1
    self._counts[label] = n
    emit(self, self._out, ("%s: %d"):format(label, n))
end

function ConsoleC:countReset(label)
    label = norm_label(label)
    if self._counts[label] == nil then
        warn_line(self, ("Warning: Count for '%s' does not exist"):format(label))
        return
    end
    self._counts[label] = 0
end

-- ------------------------------------------------------------------
-- group 族: 两空格一级缩进 (探针口径); collapsed 无 TTY 不折叠
-- ------------------------------------------------------------------

function ConsoleC:group(...)
    emit(self, self._out, format(...))
    self._indent = self._indent .. "  "
end
ConsoleC.groupCollapsed = ConsoleC.group

function ConsoleC:groupEnd()
    self._indent = self._indent:sub(1, -3)
end

-- ------------------------------------------------------------------
-- no-op 面: clear (TTY 探不到, Node 非 TTY 同款) 与 Inspector 系
-- ------------------------------------------------------------------

function ConsoleC:clear() end
function ConsoleC:timeStamp() end
function ConsoleC:profile() end
function ConsoleC:profileEnd() end

-- ------------------------------------------------------------------
-- table: ASCII 网格 (探针口径——单空格边距, 全左对齐)
--   列合成: 各行的编号键 (数组行 0 起) 与对象键取并, Values 列殿后
--   (有标量行才出现); 缺格出空。
-- ------------------------------------------------------------------

-- UTF-8 码点计数 (显示宽账记: CJK/emoji 记 1)
local function disp_width(s)
    local _, n = s:gsub("[^\128-\191]", "")
    return n
end

-- 列名归一: 整值数字键出 tostring (Node 整键同形)
local function key_name(k)
    if type(k) == "number" then
        return k // 1 == k and string.format("%d", k) or tostring(k)
    end
    return tostring(k)
end

local function key_lt(a, b)
    local ta, tb = type(a), type(b)
    if ta ~= tb then return ta < tb end -- 数字键先于字符串键
    if ta == "number" or ta == "string" then return a < b end
    return tostring(a) < tostring(b)
end

-- 一行拆格: cells = 列名 → 已渲染串; scalar 非空则进 Values 列
local function row_cells(v)
    local t = type(v)
    if t ~= "table" then
        if t == "function" then
            return {}, nil -- 空键对象行 (Node function 无键)
        end
        return {}, inspect(v) -- 标量 → Values
    end
    local cells = {}
    if #v > 0 then
        for i = 1, #v do
            cells[tostring(i - 1)] = inspect(v[i])
        end
    else
        local keys = {}
        for k in pairs(v) do keys[#keys + 1] = k end
        table.sort(keys, key_lt)
        for _, k in ipairs(keys) do
            cells[key_name(k)] = inspect(v[k])
        end
    end
    return cells, nil
end

-- properties 选取: 对象行走键, 数组行走编号 (Node element[prop]
-- 同效; 数组行的命名属性 Lua 表取不到, 账记)
local function pick_prop(v, p)
    if #v > 0 then
        local n = tonumber(p)
        return n and v[n + 1] or nil
    end
    return v[p]
end

function ConsoleC:table(data, props)
    if type(data) ~= "table" then
        -- Node 非 object 实参直接 log 口径 (format, 非 inspect)
        emit(self, self._out, format(data))
        return
    end
    if props ~= nil and type(props) ~= "table" then
        error("console.table: properties must be an array, got "
            .. type(props), 0)
    end

    -- 行序: #data > 0 按数组行 (下标 0 起), 否则对象行 (字节序)
    local index_labels, row_list = {}, {}
    if #data > 0 then
        for i = 1, #data do
            index_labels[#index_labels + 1] = tostring(i - 1)
            row_list[#row_list + 1] = data[i]
        end
    else
        local keys = {}
        for k in pairs(data) do keys[#keys + 1] = k end
        table.sort(keys, key_lt)
        for _, k in ipairs(keys) do
            index_labels[#index_labels + 1] = key_name(k)
            row_list[#row_list + 1] = data[k]
        end
    end

    -- 每行拆格: properties 给定时列即属性名单 (给定序, Node
    -- properties.map 同款——列不依赖行内容, 选取只对表行生效);
    -- 否则逐行拆格取键并
    local cells_list, scalars = {}, {}
    local has_values = false
    for r, v in ipairs(row_list) do
        if props ~= nil then
            local cells = {}
            if type(v) == "table" then
                for _, p in ipairs(props) do
                    local raw = pick_prop(v, p)
                    if raw ~= nil then cells[key_name(p)] = inspect(raw) end
                end
            end
            cells_list[r] = cells
        else
            local cells, scalar = row_cells(v)
            cells_list[r] = cells
            scalars[r] = scalar
            if scalar ~= nil then has_values = true end
        end
    end

    -- 列序: properties 给定 → 名单原序 (可重复, Node map 同款);
    -- 否则键并确定性全序 (编号/数字列数值序 → 字符串列字节序),
    -- Values 殿后 (有标量行才出现)
    local columns = {}
    if props ~= nil then
        for _, p in ipairs(props) do columns[#columns + 1] = key_name(p) end
    else
        local seen = {}
        for r = 1, #cells_list do
            for name in pairs(cells_list[r]) do
                if not seen[name] then
                    seen[name] = true
                    columns[#columns + 1] = name
                end
            end
        end
        table.sort(columns, function(a, b)
            local na, nb = tonumber(a), tonumber(b)
            if (na ~= nil) ~= (nb ~= nil) then return na ~= nil end
            if na ~= nil then return na < nb end
            return a < b
        end)
        if has_values then columns[#columns + 1] = "Values" end
    end

    -- 网格: 表头 "(index)" + 列; 全左对齐, 宽 = 码点最大值
    local show = { "(index)" }
    for _, c in ipairs(columns) do show[#show + 1] = c end
    local widths = {}
    for i, h in ipairs(show) do widths[i] = disp_width(h) end
    local grid = {}
    for r = 1, #cells_list do
        local cells = cells_list[r]
        local line_cells = { index_labels[r] }
        for ci, c in ipairs(columns) do
            line_cells[ci + 1] = (c == "Values" and scalars[r] or cells[c]) or ""
        end
        grid[r] = line_cells
        for i, s in ipairs(line_cells) do
            local w = disp_width(s)
            if w > widths[i] then widths[i] = w end
        end
    end

    local function frame(l, m, r)
        local parts = {}
        for i = 1, #show do
            parts[i] = string.rep("─", widths[i] + 2)
        end
        return l .. table.concat(parts, m) .. r
    end

    local function line(vals)
        local parts = {}
        for i = 1, #show do
            local s = vals[i] or ""
            parts[i] = " " .. s .. string.rep(" ", widths[i] - disp_width(s)) .. " "
        end
        return "│" .. table.concat(parts, "│") .. "│"
    end

    local rows = {}
    for r = 1, #grid do rows[r] = line(grid[r]) end
    emit(self, self._out,
        frame("┌", "┬", "┐") .. "\n"
            .. line(show) .. "\n"
            .. frame("├", "┼", "┤") .. "\n"
            .. table.concat(rows, "\n") .. "\n"
            .. frame("└", "┴", "┘"))
end

-- ------------------------------------------------------------------
-- 单例 (Node console 同形) + 方法挂出
-- ------------------------------------------------------------------

local default = M.Console(process.stdout, process.stderr)

for name, fn in pairs(ConsoleC) do
    if type(fn) == "function" then
        M[name] = function(...) return fn(default, ...) end
    end
end

return M
