# FAQ

## 为什么叫 luna?

月亮(luna)反射太阳光,不发光也不发热——只是把 Lua 生态里成熟的工具(交互、模块、标准库、插件、事件循环)集成在一起,不造轮子。

## 与官方 Lua / LuaJIT / luau 的关系

- **运行时是 Lua 5.5**(嵌入式,二进制自带,不依赖系统 Lua)
- 不是 LuaJIT,没有 JIT 编译;也不是 luau(无类型系统、无编译时优化)
- 语法与标准库按 Lua 5.5 参考手册;额外内置 LPeg 等常用库

## 与 Node.js / Bun / Deno 的关系

- **不是 JS 运行时**,不跑 JavaScript/TypeScript
- 模块解析、标准库 API、事件循环**刻意对齐 Node v24 语义**(见[Node 方向选型](/node-parity))
- `http.serve`/`stream`/`events`/`path`/`util` 等按 Node 实证钉版,但底层是纯 Lua + 成熟 C 库(luasocket、libuv 等),不是 V8

## 为什么不用 LuaRocks?

- 追求**单二进制分发**:所有依赖编译进二进制或随 `luna_modules/` 分发,运行零外部依赖
- `luna_modules/` 上溯机制与 `node_modules` 一致,心智负担为零
- 包管理是 `luna install|search|list|update` 一组子命令(LuaRocks **内嵌在二进制里**,不需要系统 luarocks):装进项目 `.luna/rocks/` 并锁进 `luna.lock`,属开发期可选;运行时与最终用户一行都不用跑——`git clone` 或解压 zip 即可

## 怎么安装?产物从哪来?

一键安装(装最新每日构建进 PATH,装完即验 `luna --version`,重跑即升级):

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/cuihairu/luna/main/install.sh | sh
```

```powershell
# Windows (PowerShell)
irm https://raw.githubusercontent.com/cuihairu/luna/main/install.ps1 | iex
```

产物是**滚动 nightly Release**:固定 tag [`nightly`](https://github.com/cuihairu/luna/releases/tag/nightly),每次绿跑清旧传新重发,资产 `luna-nightly-<os>-<arch>.zip`(内含二进制与 `luna_modules/` 模块侧车)+ `.sha256` 侧车。公共仓库资产匿名直拉,**不需要任何凭据**;`--token`/`GITHUB_TOKEN` 与 `LUNA_INSTALL_MIRROR` / `LUNA_MIRROR` 保留作私有 fork 与直链兜底。Actions artifact 只是 run 的本地副本,交付通道是 Release。详见[快速上手](/guide/getting-started)。

## 为什么 `require("crypto")` 报错说没链接 OpenSSL?

crypto 模块绑定 luaossl,构建期**必须**链接系统 OpenSSL 开发头(`libssl-dev` / `openssl-devel`)。未安装时构建照常通过,但运行时给出指引性错误。解决:

```bash
# Debian/Ubuntu
apt-get install libssl-dev
# Fedora/RHEL
dnf install openssl-devel
# macOS
brew install openssl
```

然后重新 `cmake` 与构建。注意 `loop.net.connectTls`/`listenTls` 同样依赖 OpenSSL——没装时函数仍在,一调用就给出指引性错误;其余模块(zlib、fs、net、http、csv、ini、toml、yaml、xml、path、util、events、stream)不受影响。

## `luna serve` 与 `http.serve()` 的区别

- `luna serve [dir] [port]`:CLI 子命令,零脚本起静态文件服务,阻塞到 `^C`,退出码 130
- `http.serve(dir|handler [, port])`:Lua API,同一入口做**静态目录**与**可编程 handler**双模式,返回服务对象(`srv:close()`、`srv.port`、`srv.url`),适合测试与嵌入

两者底层同一套实现;CLI 只是把静态模式封成一条命令。

## `^C` 不停服务/脚本怎么办?

- **`luna serve` / `luna -e` 起的服务**:内部有 100ms 心跳定时器把事件循环从 epoll 里唤醒,`^C` 能及时落到 prepare 钩子,退出码 130
- **裸脚本跑死循环**:C 内核的计数钩子每 10 万条指令轮询一次中断标志,`^C` 能打断
- **若仍不停**:检查是否用了阻塞式 C 扩展绕过计数钩子;或改用 `loop` 事件循环(prepare 钩子每轮轮询)

## 为什么 `stream` 里的 `end` 事件要写成 `s:end_(...)` 或 `s["end"](...)`?

`end` 是 Lua 关键字,不能作方法名直接调用。`stream` 统一约定:关键字冲突的方法拼下划线(`end_`),`s["end"](...)` 同效——loop 的 sock 撞过同一条限制,Node 的 `end` 在那里直接改名 `shutdown`(见[事件循环](/guide/loop)的半关闭节)。这是 Lua 语言层面的限制,不是 bug。

## `csv.lines` 迭代器遇到坏输入直接 raise,怎么安全用?

`csv.lines` 是逐记录迭代器,坏输入(如未闭合引号)只能 raise。**别把它塞进 `for ... in pcall(...)`**——`pcall` 的返回值不是合法的迭代器三元组;把整个循环包进 `pcall`,坏数据来了已收下的行照常可用:

```lua
local csv = require("csv")
local data = 'a,b\n1,2\n3,"unclosed\nnext,row\n'
local rows = {}
local ok, err = pcall(function()
    for row in csv.lines(data) do rows[#rows + 1] = row end
end)
if not ok then print("stop:", err) end
print("rows:", #rows)
```

```text
stop: csv: unterminated quoted field at line 5, column 1
rows: 2
```

格式模块的 `decode`/`encode` 才是 `nil, err` 口径,坏数据不抛。

## yaml `decode` 返回的 nil 是真 nil 还是哨兵?

默认**真 nil**(YAML `null` → Lua `nil`)。要保留哨兵以便 round-trip:

```lua
local yaml = require("yaml")
local t = yaml.decode("x: null", { nullval = yaml.null })
-- t.x == yaml.null (哨兵表),再 encode 还原 null
```

`yaml.null` 是内部唯一哨兵表,不与任何用户数据相等。

## toml `decode` 报错只有行号没有列号正常吗?

正常。底层 tomlc17 只报行号,消息格式 `<原因> at line N`。这是 C 后端语义,已用例钉住。

## `xml.sax` 的回调名为什么是 `StartElement`/`EndElement`/`CharacterData`?

直接透传 lua-expat(lxp) 的 handler 模型,回调名与首参(parser 对象)均来自 lxp。用法:

```lua
local xml = require("xml")
local p = xml.sax{
    StartElement = function(parser, name, attrs) ... end,
    EndElement   = function(parser, name) ... end,
    CharacterData= function(parser, data) ... end,
}
p:parse(chunk1)
p:parse(chunk2)
p:parse()        -- 收尾,必须调用一次无参 parse
```

## `util.inspect` 与 `util.format` 的 `%j` 行为

- `util.inspect`:Lua 语法形态输出(字符串加引号与转义、循环引用标 `[Circular *N]`、`depth` 截断);**键排序保确定性**——序列段在前,其余数字升序、字符串按字节序,连跑几次结果一致
- `util.format("%j", v)`:JSON 序列化。dkjson 比浏览器 `JSON.stringify` 严(后者省略函数字段),函数/NaN/循环引用这类它拒绝的值,**回退 `util.inspect` 渲染而不抛**——`format("%j", {f = f})` 给 `{ f = <function ...> }`,环给 `{ self = [Circular *1] }`。这是勘定过的分叉,见 [Node 方向选型](/node-parity)

## 如何把 luna 嵌入到自己的 C/C++ 项目?

luna 设计为**单二进制 CLI**,不是库。嵌入场景建议:

1. 直接调用 `luna` 二进制作为子进程(启动快,依赖少)
2. 用 `luna --attach <pid>` 接管运行中的实例
3. 如需深度嵌入,直接用 Lua 5.5 + 需要的 C 库(luasocket/lfs/openssl 等),参考 `src/luna_main.c` 的初始化流程

## Windows 支持现状

- **试运行中,尚无产物**:nightly 矩阵挂着 windows 探针腿(experimental,红不拖垮其余平台)——vcpkg 静态 zlib 已通,`LUA_USE_POSIX` 已平台收窄,编译期阻碍清单(kernel `mode_t`、luasocket 平台源选型、unix 域 socket 目标、loop 网络层 POSIX 头、测试 `poll.h`)已登记,逐层消化中
- `install.ps1` 已就位,Windows 产物落地后即可一键安装
- `path` 模块只实现 `path.posix`(Node v24 语义),win32 面不做
- **当前推荐 WSL2 或原生 Linux/macOS**;`^C` 退出码 130、attach、`luna serve` 的 ^C 契约在 POSIX 下逐条走查过

## 如何贡献/报告问题

- GitHub Issues:Bug 报告、功能建议、文档纠错
- PR 欢迎:先开 Issue 对齐方向,再提 PR;测试全绿、commit 不提 AI 字样
- 文档站源码在 `docs/`,VitePress 本地预览 `cd docs && pnpm run docs:dev`