# 沙箱运行时

`luna --sandbox`(短写 `-S`)把一次会话放进受限世界:模块白名单、禁原生加载、权限剥离、资源限额。信任模式就是今天的运行时,一字未动;沙箱在**任何用户代码起跑之前**把四层限制装好,再把自己的旋钮从表上拿走。

四种启动模式都吃得下:`--sandbox script.lua args`、`--sandbox -e 'code'`、裸 `--sandbox` 进控制台、`--sandbox -i`。装好的第一件事是往 stderr 打一行横幅,把生效的层报出来:

```text
$ luna --sandbox -e 'print("ok")'
sandbox: allowlist + LUNA_SANDBOX_MODULES, no native loads, no subprocess/file-writes/env
ok
```

## 四层

| 层 | 拦什么 | 出口 |
| --- | --- | --- |
| 模块白名单 | `require` 走卫兵,表外名字拒绝 | `LUNA_SANDBOX_MODULES` 按名放行 |
| 禁原生加载 | `package.loadlib` 拿掉、`cpath` 清空、`package` 表冻结 | 无 |
| 权限 | 子进程、文件写、环境读取 | 无(读环境返回 `nil`,不炸) |
| 资源限额 | 指令预算、内存上限 | `LUNA_SANDBOX_FUEL` / `LUNA_SANDBOX_MEM` |

### 1. 模块白名单

默认放行纯 Lua 数据面:`json` `path` `util` `events` `stream` `csv` `ini` `toml` `yaml` `xml` `zlib` `lpeg` `argparse`,加上已经加载的一切(运行时自己的基础设施:`kernel`、行编辑器、`luna.*` 策略层)和 `luna.*` 名字本身。表外的名字(如 `fs`)要先在 `LUNA_SANDBOX_MODULES` 里点过头名才进得来:

```lua
-- sbx_demo.lua
local json = require "json"          -- 白名单内,放行
print(json.encode({ ok = true }))
local ok, err = pcall(require, "fs") -- 模块表外,拒绝
print(ok, err)
print(os.getenv("HOME"))             -- 环境不可见: nil
print(pcall(os.execute, "id"))       -- 子进程: 响亮拒绝
```

```text
$ luna --sandbox sbx_demo.lua
sandbox: allowlist + LUNA_SANDBOX_MODULES, no native loads, no subprocess/file-writes/env
{"ok":true}
false	sandbox: module 'fs' is not in the allowlist
nil
false	sandbox: os.execute is denied
```

授予是按名的,逗号分隔:`LUNA_SANDBOX_MODULES=fs,path`。放行不等于全权——`fs` 进来时写面单独收走(见第 3 层)。

### 2. 禁原生加载

`package.loadlib` 置空、`package.cpath` 清空;`package` 表整个冻结——新键和改值都拒绝。冻结用的是「掏空 + 冻结副本 + `__index` 透读」,而不是一枚 `__newindex` 元方法:元方法只拦**新**键,对已存在的 `cpath` 裸赋值会直接绕过去。元表本身用 `__metatable` 锁住,`getmetatable(package)` 只拿回锁串。

```text
$ luna --sandbox -e 'package.cpath = "./?.so"'
[string "eval[1]"]:1: sandbox: package is read-only
$ luna --sandbox -e 'print(package.loadlib)'
nil
```

### 3. 权限

**动作类响亮拒绝**——子进程(`os.execute`、`io.popen`)、文件系统变更(`os.remove`/`os.rename`/`os.tmpname`、`io.open` 的写模式、`io.output(file)`)、以及能拆卫兵的内省(`debug.getupvalue`/`setupvalue`/`upvalueid`/`upvaluejoin`/`getlocal`/`setlocal`/`getregistry`/`sethook`/`setmetatable`):

```text
$ luna --sandbox -e 'local f = io.open("/tmp/pwn", "w")'
[string "eval[1]"]:1: sandbox: io.open write modes are denied
```

**读取类静默返回空**——`os.getenv` 一律返回 `nil`,环境不可见但不炸:控制台自己的历史文件、`path.cwd` 的回退链都建立在「拿不到就跳过」之上。这也是 WASM 一系运行时的分法:读不到就当没有,动不了就明说。

字节码加载关闭:`load` 强制文本模式(`string.dump` + 二进制 `load` 是文本世界的标准逃逸),文本 chunk 之外一律

```text
attempt to load a binary chunk (mode is 't')
```

被点名放行的 `fs` 一样只有读面:`writeFileSync`/`appendFileSync`/`mkdirSync` 与 `mkdir`/`rmdir`/`chdir`/`touch`/`lock`/`unlock` 在装填时单独替换成拒绝桩。

### 4. 资源限额

两个旋钮,由环境变量驱动、设完即从 `kernel` 表上摘除(脚本拿不到、改不了):

| 变量 | 单位 | 去向 |
| --- | --- | --- |
| `LUNA_SANDBOX_FUEL` | 百万条指令 | `kernel.fuel`——计数钩子每次 strike 扣 1(一次 strike ≈ 10 万条指令),扣完即停 |
| `LUNA_SANDBOX_MEM` | MiB | `kernel.memcap`——分配器字节账,超限的分配返回失败 |

```text
$ LUNA_SANDBOX_FUEL=1 luna --sandbox -e 'local s=0; for i=1,100000000 do s=s+i end'
sandbox: instruction budget exhausted
$ LUNA_SANDBOX_MEM=16 luna --sandbox -e 'local t = ("x"):rep(64*1024*1024)'
not enough memory
```

两个变量都不设,限额层不生效(其余三层照旧);内存上限是**运行中**的值—— allocator 平时逐字节等价于默认行为,上限一设才开始记账。

## 运维面与插件

**attach 通道保留**。`luna ps` 照看、`luna --attach <pid>` 照连——sock 目录在沙箱安装前快照、陈旧 socket 的清理用的是安装前捕获的真身,`socket.unix` 也在卫兵之前加载。通道本身不是绕过面:attach 灌进去的每一行都在目标态里、同一套卫兵之下求值:

```text
$ luna --sandbox /tmp/sbx_busy.lua &        # 忙循环脚本
$ luna ps | grep sbx
897829   live   ./build/luna --sandbox /tmp/sbx_busy.lua
$ printf 'x = 41\nx + 1\nrequire("fs")\n' | luna --attach 897829
attach> nil
attach> 42
attach> attach:1: sandbox: module 'fs' is not in the allowlist
```

**插件跳过**。目录插件靠 `package.preload` 注入,对白名单是一枚绕过;沙箱模式下整个发现装载层不跑,`%plugins` 报空。

## 契约与边界

- **同一个 Lua 态,不是第二台 VM。** 沙箱加固的是进程内的态:对「用 Lua 写的不可信代码」成立——语言里已无可达的不安全原语(原生加载、子进程、文件写、上值/注册表读取全部断路);它不防「进程本身被攻破」的场景,那需要操作系统级别的隔离。
- **卫兵的钥匙都藏在上值里。** 真 `require`、真 `io.open` 是装填函数闭包的上值,装填帧返回后即死;上值读取(`debug.getupvalue`)与帧局部读取(`debug.getlocal`)都在拒绝清单里。`debug.getinfo` 保留——内省魔咒(`%whos`、`%modules`)靠它读**函数对象**的元信息,而拿不到上值的函数对象只是个调用目标。
- **读环境返回 `nil` 而非抛错。** 运行时自身的调用方(REPL 历史、`path.cwd`)都有 nil 回退;脚本看到的 WORLD 是「环境为空」,而不是一个到处炸的 WORLD。
- **限额语义。** `fuel` 粒度是 strike(≈10 万条指令),小预算会先于大循环触发;`memcap` 的字节账在收缩时回收(双向记账),进程启动期的固定开销不占预算。
- **信任模式零改动。** 不带 `--sandbox` 时,新 allocator 逐字节等价默认行为,插件、attach、全部模块照旧;两条世界线只在 `-S` 这一个开关上分叉。
