-- process: Node process 全局面 (v24.21.0, 纯 Lua, 不引 loop)。
--   聚合全局 arg/os + lfs + kernel 既有同步面 (pid/millis/umask/tty/
--   write),零新 C。字段缺省 nil (Node 的 undefined)。
--   已知偏差 (账记):
--   * hrtime/uptime 分辨率到毫秒 (kernel.millis), 非 Node 纳秒;
--   * memoryUsage 只报 heapUsed (Lua 堆, collectgarbage 计数),
--     非进程 RSS;
--   * nextTick 无 loop 时立即调用 (同步世界无 tick 队列); loop 已
--     装载时走 loop.setImmediate 排队;
--   * env 走 os.getenv 代理 + 本进程内叠写层: 枚举不支持 (Lua 无法
--     遍历 environ), 叠写对本进程 Lua 读可见、对 os.execute 子进程
--     不可见 (Node 同名写在子进程可见, 语言边界);
--   * platform/arch/type 是宿主探测 best-effort (win32 按
--     package.config 分隔符, linux 按 /proc, darwin 按 CoreServices
--     探测), 探不到即 nil; type 是扩展 (Node 无此字段, undefined);
--   * execArgv 恒空表 (解释器旗标在 arg 平移时已被消费);
--   * 不立项: abort/kill/信号处理 (loop.process 辖)、title/config/
--     dlopen/binding、cpuUsage/resourceUsage (loop.os 辖)。

local M = {}

local kernel = require("kernel")
local lfs = require("lfs")

-- ------------------------------------------------------------------
-- 宿主探测 (best-effort, 纯 Lua 无 uname)
-- ------------------------------------------------------------------

local function is_windows()
    return package.config:sub(1, 1) == "\\"
end

local function read_first_line(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local s = f:read("l")
    f:close()
    if s == "" then return nil end
    return s
end

local function detect_platform()
    if is_windows() then return "win32" end
    if read_first_line("/proc/version") then return "linux" end
    if read_first_line("/System/Library/CoreServices/SystemVersion.plist") then
        return "darwin"
    end
    return nil
end

-- Node arch 词表: x64/arm64/ia32/... (uname 的 amd64 拼法归一为 x64)
local ARCH_WORDS = { x86_64 = "x64", amd64 = "x64", aarch64 = "arm64",
                     arm64 = "arm64", i686 = "ia32", i386 = "ia32",
                     armv7l = "arm", armv6l = "arm" }

local function detect_arch()
    if is_windows() then return "x64" end -- 纯 Lua 无可靠探测, 假定
    local probe = read_first_line("/proc/version")
    if probe then
        -- 词内匹配: /proc/version 里 x86_64 藏在 x86_64-linux-gnu-gcc
        -- 一类词中, 按子串扫; 各词互斥, pairs 序无关
        for w in pairs(ARCH_WORDS) do
            if probe:find(w, 1, true) then return ARCH_WORDS[w] end
        end
    end
    return nil
end

local platform = detect_platform()

local TYPE_BY_PLATFORM = { linux = "Linux", darwin = "Darwin",
                           win32 = "Windows_NT" }

local function detect_release()
    return { name = "luna" } -- Node process.release 是发行对象
end

-- linux 的父进程号: /proc/self/stat 第 4 字段 (comm 可含空格括号,
-- 从最后一个 ')' 之后数: state 后第一个字段是 ppid)
local function detect_ppid()
    if platform ~= "linux" then return nil end
    local stat = read_first_line("/proc/self/stat")
    if not stat then return nil end
    local rest = stat:match(".*%)%s+(.*)")
    if not rest then return nil end
    local it = rest:gmatch("%S+")
    it() -- state
    return tonumber(it())
end

-- ------------------------------------------------------------------
-- 命令行 (Node: argv[0]=解释器, argv[1]=脚本, argv[2..]=参数)
-- ------------------------------------------------------------------

-- 可读且非 ELF/Lua 字节码即算脚本 (shebang 脚本也是脚本)
local function looks_like_script(p)
    if type(p) ~= "string" or p == "" then return false end
    if p:match("%.lua$") or p:match("%.luna$") then return true end
    local f = io.open(p, "rb")
    if not f then return false end
    local head = f:read(4) or ""
    f:close()
    return head ~= "\27Lua" and head ~= "\x7fELF"
end

-- 解释器路径: -e/REPL 模式 arg[0] 即二进制; 脚本模式 arg[0] 是
-- 脚本, 二进制不可复原, 取脚本目录下的 luna (同 luna_main 的
-- exe 回退口径: 路径样 argv0 覆盖 ./build/luna 一类调用)
local function detect_exec_path()
    local a0 = rawget(arg or {}, 0)
    if a0 == nil then return nil end
    if not looks_like_script(a0) then return a0 end
    local dir = a0:match("^(.*)/")
    if dir and dir ~= "" then return dir .. "/luna" end
    return "luna"
end

local arg0 = rawget(arg or {}, 0)
local script_mode = looks_like_script(arg0)
local exec_path = detect_exec_path()

local argv = { [0] = exec_path }
if script_mode then
    argv[1] = arg0
    local i = 1
    while true do
        local v = rawget(arg, i)
        if v == nil then break end
        argv[i + 1] = v
        i = i + 1
    end
else
    -- -e/REPL: arg[1..] 是 -e 与代码, Node 侧 argv[1]='[eval]';
    -- 嵌入宿主 (无 arg 全表) 同归 eval 语境
    argv[1] = "[eval]"
end

M.argv = argv
M.argv0 = exec_path -- Node argv0 = 原始 argv[0] = 解释器 (非脚本)
M.execPath = exec_path
M.execArgv = {} -- 解释器旗标已被消费, 恒空 (账记)

-- ------------------------------------------------------------------
-- env: getenv 代理 + 进程内叠写层
-- ------------------------------------------------------------------

local env_overlay = {}

M.env = setmetatable({}, {
    __index = function(_, k)
        local t = type(k)
        if t ~= "string" then return nil end
        local v = env_overlay[k]
        if v ~= nil then return v end
        return os.getenv(k)
    end,
    __newindex = function(_, k, v)
        if type(k) ~= "string" then
            error("process.env: keys must be strings, got " .. type(k), 0)
        end
        if v == nil then
            env_overlay[k] = nil -- 删除: 清叠写 (真实 environ 不可删)
        else
            env_overlay[k] = tostring(v)
        end
    end,
    __pairs = function()
        error("process.env: enumeration is not supported"
            .. " (Lua cannot walk environ; read keys via os.getenv)", 0)
    end,
})

-- ------------------------------------------------------------------
-- 基本字段
-- ------------------------------------------------------------------

M.pid = kernel.pid()
M.ppid = detect_ppid()
M.platform = platform
M.arch = detect_arch()
M.type = platform and TYPE_BY_PLATFORM[platform] or nil -- 扩展 (Node 无)
M.release = detect_release()
M.exitCode = nil -- Node 缺省 undefined, 非 0
M.version = kernel.version() -- "luna 0.1.0" (Node 是 "v24.21.0" 形状)
M.versions = { lua = (_VERSION:match("5%.%d+")), luna = "0.1.0" }

local t0 = kernel.millis()

function M.cwd()
    return assert(lfs.currentdir())
end

function M.chdir(dir)
    if type(dir) ~= "string" then
        error(("process.chdir: dir must be a string, got %s")
            :format(type(dir)), 0)
    end
    local ok, err = lfs.chdir(dir)
    if not ok then
        error("process.chdir: " .. tostring(err), 0)
    end
end

function M.exit(code)
    -- 无参用 exitCode (Node 同款; exitCode 缺省 nil 出 0);
    -- true/false 沿用 os.exit 语义
    os.exit(code ~= nil and code or (M.exitCode or 0), true)
end

function M.uptime()
    return (kernel.millis() - t0) / 1000
end

-- Node hrtime: [秒, 纳秒] 相对任意原点; hrtime(prev) 出差值。
-- 分辨率到毫秒 (kernel.millis), 非 Node 纳秒 (账记)。
-- 可调用表: 挂 .bigint 子面 (Node process.hrtime.bigint 同形)。
M.hrtime = setmetatable({}, {
    __call = function(_, prev)
        local ms = kernel.millis()
        local s = ms // 1000
        local ns = (ms % 1000) * 1000000
        if prev ~= nil then
            if type(prev) ~= "table" then
                error("process.hrtime: prev must be a table", 0)
            end
            s = s - math.floor(tonumber(prev[1]) or 0)
            ns = ns - math.floor(tonumber(prev[2]) or 0)
            if ns < 0 then
                s = s - 1
                ns = ns + 1000000000
            end
        end
        return { s, ns }
    end,
})

function M.hrtime.bigint()
    return kernel.millis() * 1000000 -- 整型纳秒 (毫秒分辨率)
end

function M.memoryUsage()
    return { heapUsed = math.floor(collectgarbage("count")) * 1024 }
end

-- 无参查询: kernel.umask 缺参会置 0077, 先置 0 取旧值再恢复
-- (Node 无参 umask 是纯查询)
function M.umask(mask)
    if mask == nil then
        local old = kernel.umask(0)
        kernel.umask(old)
        return old
    end
    return kernel.umask(mask)
end

-- ------------------------------------------------------------------
-- nextTick: loop 已装载走 setImmediate 排队, 否则立即调用
-- ------------------------------------------------------------------

function M.nextTick(fn, ...)
    if type(fn) ~= "function" then
        error(("process.nextTick: callback must be a function, got %s")
            :format(type(fn)), 0)
    end
    local loop = package.loaded["loop"]
    if loop ~= nil and type(loop.setImmediate) == "function" then
        loop.setImmediate(fn, ...)
    else
        fn(...)
    end
end

-- ------------------------------------------------------------------
-- stdio: stdout 走 kernel.write (输出漏斗, 捕获/插件可见); stderr
-- 直写 io.stderr 并即刷; stdin 薄包 io.stdin。isTTY 只有 stdin 面
-- (kernel.tty 探 fd0), stdout/stderr 记 nil (账记)。
-- ------------------------------------------------------------------

M.stdout = { isTTY = nil } -- isTTY: kernel.tty 只探 fd0 (账记)

function M.stdout.write(s)
    if type(s) ~= "string" then
        error(("process.stdout.write: chunk must be a string, got %s")
            :format(type(s)), 0)
    end
    kernel.write(s)
    return true
end

function M.stdout.end_() end

M.stderr = { isTTY = nil } -- isTTY: kernel.tty 只探 fd0 (账记)

function M.stderr.write(s)
    if type(s) ~= "string" then
        error(("process.stderr.write: chunk must be a string, got %s")
            :format(type(s)), 0)
    end
    io.stderr:write(s)
    io.stderr:flush()
    return true
end

function M.stderr.end_() end

M.stdin = { isTTY = kernel.tty() }

function M.stdin.read(...)
    return io.stdin:read(...)
end

return M
