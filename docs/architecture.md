# 架构设计

本文回答四个问题:**代码怎么分层、第三方怎么选、插件边界在哪、事件循环为何推迟**。

## 分层

```
┌────────────────────────────────────────────┐
│ 插件(目录,plugin.json + 入口脚本)            │  ← 发现/排序/隔离由 plugins.lua 执行
├────────────────────────────────────────────┤
│ 扩展点:magic.register · complete.add_source │
│         highlight.set · modules 注入        │
├────────────────────────────────────────────┤
│ 策略层(纯 Lua,编译期嵌入二进制):             │
│   repl · complete · highlight · introspect  │
│   magic · modules · plugins                 │
├────────────────────────────────────────────┤
│ C 内核 luna_kernel:exec/write/millis/tty    │
│   colors/sink/clear_interrupt + 行编辑桥     │
│   (replxx)+ C 模块注册(lfs/socket/zlib/…)   │
├────────────────────────────────────────────┤
│ Lua 5.4 官方虚拟机 + LPeg + deps 第三方库     │
└────────────────────────────────────────────┘
```

**分层理由:**

- **C 内核尽量薄**:只做 C 擅长且必须进 C 的事——信号(SIGINT)、毫秒时钟、isatty、输出汇聚点(sink)、replxx 桥。REPL 的行为逻辑全是 Lua,因为逻辑变化频率高,放 Lua 层改起来不需要懂 C,插件也能读懂并替换;
- **策略层随二进制分发**:repl/highlight/… 在编译期由 CMake 嵌成 C raw string(`cmake/luna_lua.h.in` 生成头),用户拿到的单文件二进制行为完整;同时源文件即文档,`lua/luna/` 可直接阅读;
- **扩展点即边界**:四组注册 API(`magic.register`、`complete.add_source`、`highlight.set`、返回 `modules` 表)是插件唯一入口。策略层内部结构对插件不可见,策略层重构不破坏插件;
- **官方 Lua 是内核**:luna 不含自制解释器;所有"语言层"能力来自官方 Lua 5.4 + LPeg,升级 Lua 只动 `deps/`。

## 与 Node.js 的对照

| 概念 | Node.js | luna |
| --- | --- | --- |
| 入口 | `node` / `node script.js` / `node -e` | `luna` / `luna script.lua` / `luna -e` |
| 依赖目录 | `node_modules/` 逐级上溯 | `luna_modules/` 逐级上溯 |
| 相对 require | 相对当前文件 | 相对当前 chunk(`@` 源锚定) |
| 包清单 | `package.json` 的 `main` | 同名清单的 `main`,缺省 `init.lua` |
| 插件 | (n/a,靠包) | `plugins/` 目录 + `plugin.json`,就近遮蔽 |
| 全局注入 | `process`/`Buffer` | `kernel`/`Out`/`In` |
| 标准库 | 内建 | `json`/`fs`/`net`/`http`/`zlib`/`crypto` 随二进制 |

差异是刻意的:Lua 的 require 缓存(`package.loaded`)、chunk 与 `...`、官方搜索器语义全部保持官方行为,luna 只在**搜索器序列中插入一环**,不做替换。

## 选型对比

口径:候选、入选理由(维护活跃度/API 面/绑定质量)、谁被淘汰与原因。

### 行编辑与历史 —— replxx

| 候选 | 结论 |
| --- | --- |
| **replxx**(`undoing/replxx`) | **入选**:活跃维护、Unicode/中文宽字符正确、彩色提示、hints、history 持久化与增量检索、简洁 C API(项目提供 `luna_line.c` 桥) |
| linenoise(antirez) | 原始计划人选,batch 7 升级为 replxx:单文件虽美,但宽字符与多字节编辑弱、无彩色提示,社区修复长期悬置 |
| readline | GPL/LGPL 授权与全局状态(new readline 每进程一态)都不适合嵌入宿主 |
| lua-linenoise | 又绑一层 linenoise,继承其短板 |

### 词法高亮 —— scintillua + LPeg

| 候选 | 结论 |
| --- | --- |
| **scintillua**(Mitchell / orbitalquark,Textadept 生态) | **入选**:几十种语言的生产级 lexer 集,纯 Lua + LPeg,可独立嵌入(`lexers/lua.lua` 直接可用);换语言=换 lexer 文件,天然可插件化 |
| 自写正则高亮 | 淘汰:正则无法正确处理嵌套长字符串 `[[ ]]`、字符串内转义、`--` 与 `---@` 之类歧义;前车之鉴是各家编辑器的高亮 bug 清单 |
| 动手移植 TextMate 语法 | 淘汰:oniguruma 依赖 + JSON→Lua 转换层,复杂度无收益 |

### 命令行解析 —— argparse(mpeterv)

| 候选 | 结论 |
| --- | --- |
| **argparse**(`mpeterv/argparse`) | **入选**:受 Python argparse 启发,声明式定义、自动 usage/help、子命令支持、纯 Lua、测试完备 |
| docopt.lua | 淘汰:docopt 的"文档即解析器"风格在 help 文案与解析冲突时难控制,实现成熟度参差 |
| 手写 `arg[i]` 分支 | 淘汰:`-i`/`--interactive`、可选位置参数、`--no-*` 开关的组合手写必错 |

### JSON —— dkjson

| 候选 | 结论 |
| --- | --- |
| **dkjson**(David Kolf,dkolf.de;LuaRocks 账号 dhkolf) | **入选**:纯 Lua 单文件零依赖、严格模式(拒绝非法输入)、大整数处理正确、LuaRocks 长期稳定分发(无上游 git 仓库,以 src.rock 形式发布,vendor 时记录 provenance) |
| lua-cjson | 淘汰:C 绑定快 3–5 倍,但 REPL 场景吞吐无关紧要,且发行版/OpenResty fork 版本林立、API 细节漂移 |
| rxi/json.lua | 淘汰:实现清爽但无严格模式,循环引用行为与 dkjson 不一致 |

### zlib —— lua-zlib(brimworks)

| 候选 | 结论 |
| --- | --- |
| **lua-zlib**(`brimworks/lua-zlib`) | **入选**:维护最久、API 为流式 closure(`zlib.deflate(level)(data, "finish")`),可做增量;薄层补一次性 `compress`/`decompress` |
| lua-lzlib | 淘汰:API 更贴近 zlib 原型但久未跟进新版 Lua |
| 自写 FFI 绑定 | 淘汰:依赖 LuaJIT FFI,luna 用官方 PUC Lua |

### crypto/TLS —— luaossl(wahern)

| 候选 | 结论 |
| --- | --- |
| **luaossl**(`wahern/luaossl`) | **入选**:覆盖面最完整(pkey/x509 全家族/ssl/store/rand/…,26 个子模块)、紧贴 OpenSSL 1.1–3.x、按子模块注册(`_openssl.<sub>`)、文档详尽 |
| lua-openssl(zhaozg,OpenResty 生态常见) | 淘汰:API 偏 OpenSSL 原型直译,**注意归属——它不是 OpenResty 官方出品**,只是 OpenResty 发行版常带;维护节奏依赖单作者 |
| LuaSec | 淘汰:定位是 socket TLS 封装,不做 x509/pkey 级操作 |

### 其余

- **Lua 5.4**:官方源码,不用 LuaJIT(5.4 语义 + 维护优先);
- **LPeg**:scintillua 的依赖,亦是将来 lexer 插件的运行时;
- **luafilesystem**(keplerproject)、**luasocket**(Diego Nehab):各自领域的事实标准,不重新选;
- **libuv**:`deps/` 已入库、尚未接线——见下节。

## 内核/插件边界与加载顺序

**边界**:加载器(`luna.plugins`)只负责发现、按就近顺序排序、`pcall` 执行入口、注入 `modules`、产出三份报告(`loaded`/`failed`/`overridden`);插件只通过扩展点 API 接线。互相不透视内部。

**加载顺序**(每种启动模式都加载,`--no-plugins` 跳过):

1. `./plugins`(项目)→ 2. `~/.luna/plugins`(用户)→ 3. `$LUNA_PLUGIN_PATH` 逐项;
2. 同目录内按字典序;manifest 名**先到先得**,后到者记 overridden(项目遮蔽用户);
3. 入口在 `pcall` 下执行——**失败隔离**:一个插件抛错或清单损坏,只记入 `plugins.failed`,兄弟插件与 luna 本体继续启动;补全源与高亮函数同样受 pcall 保护。

**为何"名字先到先得 + 失败不回退"**:遮蔽语义要可预测——用户看到 `%plugins` 里项目版已占名,比"悄悄用了用户版"更好排查;回退会把失败藏进成功里。

## 事件循环:刻意推迟

Node 之"Node",一半在事件循环。luna 目前**没有**:标准库是同步的(`fs.readFileSync`、阻塞 socket),REPL 是单线程会话模型。理由:

1. **语义冲突未决**:同步 stdlib 与回调式异步在同一会话里混用,需要先定死 yield 点(哪个调用让出、哪个永不阻塞),这不是实现问题而是语言设计问题;
2. **REPL 不需要**:交互会话的主循环是"读一行 → 求值 → 打印",天然同步;协程+`kernel.exec` 已覆盖中断(`^C`)需求;
3. **deps/libuv 已入库**:当异步 batch 启动时,基础设施在,不需要新依赖;届时按"脚本模式可选异步、REPL 逐步暴露"的顺序推进,而不是一步到位。

先把模块、插件、高亮、内省做扎实——运行时的"骨头"长好了,事件循环才能长在正确的位置。
