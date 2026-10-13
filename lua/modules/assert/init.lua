-- assert: Node assert 面 (v24.21.0 形状对拍, 纯 Lua)。
--   assert/ok/equal/notEqual/strictEqual/notStrictEqual/deepEqual/
--   notDeepEqual/deepStrictEqual/notDeepStrictEqual/fail/ifError/
--   throws/doesNotThrow。
--   单元格式化走 util.format, 单元渲染走 util.inspect。
--   已知偏差 (账记):
--   * 真值集按 Node 收口 (falsy = nil/false/0/''/NaN——Lua 本只
--     nil/false 为假, 语言边界);
--   * deepEqual 的宽松比较走 Lua == (含类型强转 1=='1'), 与 Node
--     同形; deepStrictEqual 的 -0≠0 靠 string.format("%.17g") 辨号
--     (Lua == 不区分正负零);
--   * 错误对象是表 (code/actual/expected/operator/message 字段),
--     非 Lua error 串——Node 的 AssertionError 实例在 Lua 无对应物,
--     字段口径一致;
--   * throws 的 error 匹配支持串/表(message|code|name)/函数(构造器),
--     不支持正则 (Lua 无内建正则, 用模式匹配语义不同);
--   * 不立项: async 面 (rejects/doesNotReject)——luna 的 promise 契约
--     在 task 模块, 异步断言随 task 面另立。

local M = {}

local util = require("util")
local format = util.format
local inspect = util.inspect

-- ------------------------------------------------------------------
-- 错误对象 (Node AssertionError 形状)
-- ------------------------------------------------------------------

local function make_error(actual, expected, operator, message)
    return setmetatable({
        code = "ERR_ASSERTION",
        actual = actual,
        expected = expected,
        operator = operator,
        message = message,
    }, {
        __tostring = function(self)
            return self.message
        end,
    })
end

-- ------------------------------------------------------------------
-- 真值集 (Node falsy: nil/false/0/''/NaN)
-- ------------------------------------------------------------------

local function js_falsy(v)
    if v == nil or v == false then return true end
    if v == 0 or v == "" then return true end
    return type(v) == "number" and v ~= v -- NaN
end

-- JS Number(s): 空串与纯空白串 → 0, 其余走 tonumber, 失败 → nil (即 NaN 语义)
local function js_tonumber(s)
    if s == "" then return 0 end
    return tonumber(s)
end

-- 宽松相等 (Node ==): 布尔转数字, 串转数字, 无类型强转的走 Lua ==。
local function loose_eq(a, b)
    if type(a) == type(b) then return a == b end
    if type(a) == "boolean" and type(b) == "number" then
        return (a and 1 or 0) == b
    end
    if type(a) == "number" and type(b) == "boolean" then
        return a == (b and 1 or 0)
    end
    if type(a) == "number" and type(b) == "string" then
        return a == js_tonumber(b)
    end
    if type(a) == "string" and type(b) == "number" then
        return js_tonumber(a) == b
    end
    if type(a) == "boolean" and type(b) == "string" then
        return (a and 1 or 0) == js_tonumber(b)
    end
    if type(a) == "string" and type(b) == "boolean" then
        return js_tonumber(a) == (b and 1 or 0)
    end
    return false
end

-- ------------------------------------------------------------------
-- assert / ok
-- ------------------------------------------------------------------

function M.assert(value, message)
    if not js_falsy(value) then return value end
    local msg
    if message ~= nil then
        msg = tostring(message)
    else
        msg = "The expression evaluated to a falsy value:\n\n"
            .. "  assert(" .. inspect(value) .. ")\n"
    end
    error(make_error(value, nil, "==", msg), 0)
end

M.ok = M.assert

-- ------------------------------------------------------------------
-- 标量比较 (equal/strictEqual 及 not 系)
-- ------------------------------------------------------------------

-- 严格相等: NaN 相等, -0≠0, 无类型强转 (Object.is 语义)
local function strict_eq(a, b)
    if type(a) ~= type(b) then return false end
    if type(a) == "number" then
        if a ~= a and b ~= b then return true end -- NaN
        if a == 0 and b == 0 then
            -- 辨正负零: %.17g 出 "-0" vs "0"
            return string.format("%.17g", a) == string.format("%.17g", b)
        end
        return a == b
    end
    return a == b
end

function M.equal(actual, expected, message)
    if loose_eq(actual, expected) then return end
    error(make_error(actual, expected, "==",
        message or format("%s == %s", inspect(actual), inspect(expected))), 0)
end

function M.notEqual(actual, expected, message)
    if not loose_eq(actual, expected) then return end
    error(make_error(actual, expected, "!=",
        message or format("%s != %s", inspect(actual), inspect(expected))), 0)
end

function M.strictEqual(actual, expected, message)
    if strict_eq(actual, expected) then return end
    error(make_error(actual, expected, "strictEqual",
        message or format("%s === %s", inspect(actual), inspect(expected))), 0)
end

function M.notStrictEqual(actual, expected, message)
    if not strict_eq(actual, expected) then return end
    error(make_error(actual, expected, "!==",
        message or format("%s !== %s", inspect(actual), inspect(expected))), 0)
end

-- ------------------------------------------------------------------
-- 深比较 (deepEqual 宽松 / deepStrictEqual 严格)
-- ------------------------------------------------------------------

-- 宽松深比较: 标量走宽松 == (含强转), 表递归比键 (缺键即不等)
local function deep_eq_loose(a, b)
    if type(a) ~= type(b) then
        -- Node deepEqual 允许跨类型 == (1=='1'), 但表 vs 非表不等
        if type(a) == "table" or type(b) == "table" then return false end
        return loose_eq(a, b)
    end
    if type(a) ~= "table" then
        if a ~= a and b ~= b then return true end -- NaN
        return loose_eq(a, b)
    end
    -- 表: 键集须一致 (Node 要求同键集)
    local ka, kb = {}, {}
    for k in pairs(a) do ka[k] = true end
    for k in pairs(b) do kb[k] = true end
    for k in pairs(ka) do
        if kb[k] == nil then return false end
    end
    for k in pairs(kb) do
        if ka[k] == nil then return false end
    end
    for k, va in pairs(a) do
        if not deep_eq_loose(va, b[k]) then return false end
    end
    return true
end

-- 严格深比较: 标量走 strict_eq, 表递归比键
local function deep_eq_strict(a, b)
    if type(a) ~= type(b) then return false end
    if type(a) ~= "table" then return strict_eq(a, b) end
    local ka, kb = {}, {}
    for k in pairs(a) do ka[k] = true end
    for k in pairs(b) do kb[k] = true end
    for k in pairs(ka) do
        if kb[k] == nil then return false end
    end
    for k in pairs(kb) do
        if ka[k] == nil then return false end
    end
    for k, va in pairs(a) do
        if not deep_eq_strict(va, b[k]) then return false end
    end
    return true
end

function M.deepEqual(actual, expected, message)
    if deep_eq_loose(actual, expected) then return end
    error(make_error(actual, expected, "deepEqual",
        message or "Expected values to be loosely deep-equal:"), 0)
end

function M.notDeepEqual(actual, expected, message)
    if not deep_eq_loose(actual, expected) then return end
    error(make_error(actual, expected, "notDeepEqual",
        message or "Expected values not to be loosely deep-equal:"), 0)
end

function M.deepStrictEqual(actual, expected, message)
    if deep_eq_strict(actual, expected) then return end
    error(make_error(actual, expected, "deepStrictEqual",
        message or "Expected values to be strictly deep-equal:"), 0)
end

function M.notDeepStrictEqual(actual, expected, message)
    if not deep_eq_strict(actual, expected) then return end
    error(make_error(actual, expected, "notDeepStrictEqual",
        message or "Expected values not to be strictly deep-equal:"), 0)
end

-- ------------------------------------------------------------------
-- fail / ifError
-- ------------------------------------------------------------------

function M.fail(message)
    error(make_error(nil, nil, "fail",
        message ~= nil and tostring(message) or "Failed"), 0)
end

function M.ifError(err)
    if err == nil then return end
    local msg = "ifError got unwanted exception: "
        .. (type(err) == "string" and err or tostring(err))
    error(make_error(err, nil, "ifError", msg), 0)
end

-- ------------------------------------------------------------------
-- throws / doesNotThrow
-- ------------------------------------------------------------------

-- 剥掉 Lua error 的位置前缀 ("chunk:line: "), 取纯消息
local function strip_pos(s)
    if type(s) ~= "string" then return s end
    return s:match("^.-:%d+: (.*)$") or s
end

-- error 匹配: 串 (比 message 子串) / 表 (message|code|name 字段) /
-- 函数 (构造器, 比 type)。不支持正则 (账记)。
-- got 为表时按字段比 (Node 的 validation object 语义); got 为串时比 message。
local function match_error(got, want)
    if want == nil then return true end
    local t = type(want)
    if t == "string" then
        return type(got) == "string" and strip_pos(got):find(want, 1, true) ~= nil
    end
    if t == "table" then
        local gmsg = type(got) == "table" and got.message or strip_pos(got)
        local gcode = type(got) == "table" and got.code or nil
        local gname = type(got) == "table" and got.name or nil
        if want.message ~= nil and gmsg ~= want.message then return false end
        if want.code ~= nil and gcode ~= want.code then return false end
        if want.name ~= nil and gname ~= want.name then return false end
        return true
    end
    if t == "function" then
        return type(got) == t
    end
    return false
end

function M.throws(fn, error_, message)
    -- Node: 串作第二参是断言 message, 不是 error 匹配器
    if type(error_) == "string" then
        message = error_
        error_ = nil
    end
    local ok, err = pcall(fn)
    if ok then
        error(make_error(nil, nil, "throws",
            message or "Missing expected exception."), 0)
    end
    if not match_error(err, error_) then
        error(make_error(err, nil, "throws",
            message or format("Expected exception matching %s", inspect(error_))), 0)
    end
end

function M.doesNotThrow(fn, error_, message)
    local ok, err = pcall(fn)
    if not ok then
        error(make_error(err, nil, "doesNotThrow",
            message or "Got unwanted exception."), 0)
    end
end

return M
