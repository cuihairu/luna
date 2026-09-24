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
│   (replxx,附 attach 唤醒线程)+ C 模块注册    │
│   (lfs/socket/zlib/…)                       │
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

## 事件循环:第一批(显式选择,脚本 opt-in)

Node 之"Node",一半在事件循环。luna 现在有了第一块:**`require "loop"`**——deps/libuv(1.44.2)接线的 Node 命名 API(setTimeout/setInterval/setImmediate/clear*/run/stop),静态编进 luna 单一二进制。同步标准库不受影响,REPL 不变;谁 require 谁进异步世界。曾经推迟的理由仍在,也决定了这一批的边界:

1. **语义冲突未决**:同步 stdlib 与回调式异步在同一会话里混用,yield 点(哪个调用让出)仍是语言设计问题——所以循环只驱动用户注册的回调,标准库一个字节不改;
2. **REPL 不需要**:交互主循环天然同步;循环是脚本模式的显式选择,没有全局 setTimeout 去污染同步代码;
3. **两份契约搭一趟车**:循环唯一的自有句柄是一个 prepare 钩子(有活句柄时启动,最后一个关闭时停),它每轮做两件事——把 uv_run 阻塞期间落下的 `^C` 转成 `interrupted` 错误(退出码 130 约定不变),和轮询 attach 套接字。事件循环里的进程因此照常可被 `--attach` 观测:轮询点模型原样成立,不因 libuv 的存在而多出第二个并发来源。

keep-alive 语义与 libuv 对齐:每个回调句柄被 registry 持有直到 `uv_close` 完成回调落地(`uv_close` 异步,句柄内存必须活过它);最后一个句柄关闭,钩子停,空转的 `run("default")` 返回——和 Node 的"事件空则退出"一致。

先把模块、插件、高亮、内省做扎实——运行时的"骨头"长好了,事件循环才能长在正确的位置。

## attach:轮询点,而非事件循环

`luna --attach` 让一个进程观测另一个进程的实时状态,要在目标忙于任意 Lua 代码(甚至死循环)或阻塞在行编辑器时仍能应答——这本质是个并发问题,而它的实现刻意**不引入**事件循环:

- **两个轮询点**。目标进程在 `lua/luna/serve.lua` 暴露一个非阻塞的 `serve.step()`(accept → 读一行 → pcall 求值 → 封帧应答)。调用它的只有两处:REPL 主循环的空闲间隙,和脚本执行期间 C 计数钩子里每约 20 万条指令的一次顺带调用(钩子在轮询期间挂起、之后复原,嵌套 `pcall` 保证轮询失败不伤用户代码)。没有新线程进 Lua,没有 epoll,语义仍是纯同步的;
- **信号即门铃**。空闲目标阻塞在 replxx 的行编辑上——replxx 内部吞掉 EINTR,同线程的 notify 又被刻意忽略,所以 `luna_line.c` 里有一个专用助手线程:SIGUSR1 处理器只往管道写一个字节(async-signal-safe),线程醒来对 replxx 合成一个 `KEY_ENTER`(必须是带 control 修饰的键码,裸 `\r` 只会响铃),被阻塞的 `input()` 返回空行,主循环回接到轮询点;
- **协议一行可读**:请求就是一行文本,应答是 `"OK\n"/"ERR\n"/"EXIT\n" + 正文 + "\30\n"` 的文本帧(`EXIT` 只脱离客户端、不杀目标),`\30` 后的换行让行读取器不悬空;命令执行期间内核输出被捕获进帧并同时镜像到目标终端。`%` 魔法、`?expr` 帮助糖、以及 `\1complete` 补全元行全部复用目标端自己的模块(`luna.magic`、`luna.introspect`、`luna.complete`)——serve 只是桥,不是第二套 REPL 实现;客户端错误走 stderr,stdout 保持结果流干净;
- **可观测性的边界**:attach 只在**轮询点**到达,所以它看到的是目标"呼吸的间隙"里的状态,而不是任意指令边界的状态——这正好把它和调试器、和未来的事件循环区分开。

这是"成熟件优先"的又一次体现:计数钩子(SIGINT 已有)、replxx 的模拟按键 API、luasocket 的 unix 传输,全部是既有积木,新写的只有几百行 Lua 桥接。配套的进程策略:`SIGPIPE` 被显式忽略(对端消失的写操作浮出为 EPIPE 错误,`pcall` 接得住;信号本身是 `pcall` 接不住的),attach 套接字经 umask 包裹 bind + chmod 双保险做到仅创建者可读写。
