# FAQ

## 为什么叫 luna?

月亮(luna)反射太阳光,不发光也不发热——只是把 Lua 生态里成熟的工具(交互、模块、标准库、插件、事件循环)集成在一起,不造轮子。

## 与官方 Lua / LuaJIT / luau 的关系

- **运行时是 Lua 5.5**(嵌入式,二进制自带,不依赖系统 Lua)
- 不是 LuaJIT,没有 JIT 编译;也不是 luau(无类型系统、无编译时优化)
- 语法与标准库按 Lua 5.5 参考手册;额外内置 LPeg、utf8 等常用库

## 与 Node.js / Bun / Deno 的关系

- **不是 JS 运行时**,不跑 JavaScript/TypeScript
- 模块解析、标准库 API、事件循环**刻意对齐 Node v24 语义**(见[Node 方向选型](/node-parity))
- `http.serve`/`stream`/`events`/`path`/`util` 等按 Node 实证钉版,但底层是纯 Lua + 成熟 C 库(luasocket、libuv 等),不是 V8

## 为什么不用 LuaRocks?

- luna 追求**单二进制分发**:所有依赖编译进二进制或随 `luna_modules/` 分发,运行零外部依赖
- `luna_modules/` 上溯机制与 `node_modules` 一致,心智负担为零
- 包管理(`rocks` 子命令)是**给作者发包用的**,不是运行时必须;最终用户只需 `git clone` 或解压 zip 即可跑

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

然后重新 `cmake` 与构建。其他模块(zlib、fs、net、http、csv、ini、toml、yaml、xml、path、util、events、stream)不受影响。

## `luna serve` 与 `http.serve()` 的区别

- `luna serve [dir] [port]`:CLI 子命令,零脚本起静态文件服务,阻塞到 `^C`,退出码 130
- `http.serve(dir|handler [, port])`:Lua API,同一入口做**静态目录**与**可编程 handler**双模式,返回服务对象(`srv:close()`、`srv.port`、`srv.url`),适合测试与嵌入

两者底层同一套实现;CLI 只是把静态模式封成一条命令。

## `^C` 不停服务/脚本怎么办?

- **`luna serve` / `luna -e` 起的服务**:内部有 100ms 心跳定时器把事件循环从 epoll 里唤醒,`^C` 能及时落到 prepare 钩子,退出码 130
- **裸脚本跑死循环**:C 内核的计数钩子每 10 万条指令轮询一次中断标志,`^C` 能打断
- **若仍不停**:检查是否用了阻塞式 C 扩展绕过计数钩子;或改用 `loop` 事件循环(prepare 钩子每轮轮询)

## 为什么 `stream` 里的 `end` 事件要写成 `s:end_(...)` 或 `s["end"](...)`?

`end` 是 Lua 关键字,不能作方法名直接调用。`stream`、socket、loop 的 socket 对象统一约定:关键字冲突的方法拼下划线(`end_`、`close_` 等)或用下标调用。这是 Lua 语言层面的限制,不是 bug。

## `csv.lines` 迭代器遇到坏输入直接 raise,怎么安全用?

`csv.lines` 是逐记录迭代器,坏输入(如未闭合引号)只能 raise。喂不可信数据时:

```lua
local csv = require("csv")
for ok, row in pcall(csv.lines, data) do
    if not ok then break end
    -- process row
end
```

或自行 `pcall` 包装。格式模块的 `decode`/`encode` 才是 `nil, err` 口径。

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

- `util.inspect`:Lua 语法形态输出(字符串加引号、转义、循环引用标记 `[Circular *N]`,截断 `depth`),键序按 `pairs` 确定性排序
- `util.format("%j", v)`:等价 `json.encode(v)` 但非法值(函数/循环引用)抛错而非返回 `null`——与 Node v24 一致

## 如何把 luna 嵌入到自己的 C/C++ 项目?

luna 设计为**单二进制 CLI**,不是库。嵌入场景建议:

1. 直接调用 `luna` 二进制作为子进程(启动快,依赖少)
2. 用 `luna --attach <pid>` 接管运行中的实例
3. 如需深度嵌入,直接用 Lua 5.5 + 需要的 C 库(luasocket/lfs/openssl 等),参考 `src/luna_main.c` 的初始化流程

## Windows 支持现状

- 构建:MSVC / MinGW 均可(`cmake --build build --config Debug`)
- 测试:ctest 全组通过
- 路径模块:仅实现 `path.posix`(Node v24 语义),win32 面不做
- 插件 socket:Unix 域 socket,Windows 上需 WSL 或改用命名管道(暂未实现)
- 推荐 Windows 用户跑 WSL2

## 如何贡献/报告问题

- GitHub Issues:Bug 报告、功能建议、文档纠错
- PR 欢迎:先开 Issue 对齐方向,再提 PR;测试全绿、commit 不提 AI 字样
- 文档站源码在 `docs/`,VitePress 本地预览 `cd docs && npm run docs:dev`