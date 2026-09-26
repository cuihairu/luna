# todo

2026-09-25 通读 README / docs / 源码后的缺口盘点。README 宣称的能力大多已落地,本清单只记
**文档承诺了但实现缺位**、**明显值得补**与**顺手打磨**三类,按优先级排序。
约定:每完成一项必须带测试,`cmake --build` + `ctest` 全绿后才 commit/push;不打 tag、不发版。

**状态:2026-09-26 行编辑桥一轮、信号/可达性一轮全部完成**(逐组 commit 已推送,ctest
**15 组全绿**)。

## P1 会话与魔法命令(文档承诺 vs 实现缺口)

- [x] **`In[n]` 寄存器缺失**:补 `_G.In[n]`(repl `Session:record`,会话求值、魔法命令与
      `?` 糖同点维护,多行整块按整块登记);`%reset` 清 `Out`/`_`/`__`,`In` 按
      `session.inputs` **保留重建**(输入历史是会话的,不是用户状态的——与 IPython 的
      `%reset` 保留历史一致)。测试:repl 组 4 例。
- [x] **`%time`/`%timeit` 只吃表达式**:复用 REPL 的 wrapped 探针(`runnable`),表达式
      照旧回显 `Out[n]`,语句形式只报时间。测试:magic 组语句 `%time` 例。
- [x] **`%plugins` 不报遮蔽**:补 "overridden by an earlier same-name plugin" 段
      (名字 + 落选目录)。测试:magic 组遮蔽例。

## P2 错误提示与补全

- [x] **"did you mean" 建议**:Lua 5.5 错误标注 `(global 'x')` 且 `rawget(_G, x)` 为空时,
      `luna.introspect.suggest` 用有界编辑距离给出最近 1–2 个全局名(并列报两个,
      字典序定序),REPL 错误输出追加 `hint: did you mean 'x'?` 行。测试:repl 组 4 例。
- [x] **Tab 补全不认 require 目标**:内置 source——`require "pre` / `require("pre` 列
      `package.path` 各目录的包名(含子包,`base:gsub("%.","/")`)与 `package.loaded`
      已加载名 + preload 键;点号续配走已加载表字段;合并时全局去重
      (`complete.line` 的 `seen`)。测试:complete 组 6 例。
- [x] **luna_modules 包内子模块**:`resolve_bare` 加最长前缀回退(`package_subpath`):
      每层先整名(package_entry → flat → 无),再试 `<前缀>/余段.lua` 与
      `<前缀>/余段/init.lua`;子路径不走路由包 `main`,`return load_entry(sub)` 尾调用
      保持 loader+path 双值(Lua 5.5 的 require 新加载返回双值)。测试:modules 组 3 例。

## P3 标准库与打磨

- [x] **fs 便捷层补齐**:`appendFileSync`(创建即追加)/ `readdirSync`(排序、排除
      `.`/`..`;不可读目录由 lfs 自己 raise——vendored lfs 的 dir 工厂对 opendir 失败
      走 luaL_error,不返回 nil,故不加死守卫)/ `mkdirSync(recursive)`(mkdir -p 语义:
      递归建父、已存在幂等、非递归撞目录报 `already exists`)。测试:modules 组 2 例
      (roundtrip + 排序/错误路径)。
- [x] **`%hist` 打磨**:`%hist N` / `%hist N-M` 范围参数。测试:magic 组范围例。
- [x] **文档站同步**:cli-repl(In/Out 寄存器、打错字提示、require 补全、%time 语句、
      %hist 范围、%plugins 遮蔽段、%reset 语义)、modules(包内子路径规则、fs 新便捷层、
      Lua 5.5 require 双返回值注记)。
- [x] **覆盖率**:每个新特性/新分支都有对应用例(repl 21、magic 14、complete 21、
      modules 18);每组改动 `cmake --build` + `ctest` 12 组全绿后才提交。
      实测基线(2026-09-26,`build-cov` Profiling 插桩树,gcovr):C 侧 `src/` 行覆盖
      75.8%、函数 81.8%、分支 49%——缺口集中在 `luna_line.c`(replxx 行编辑,2%,
      只有真 TTY 路径能覆盖,测试仅经 serve 组的 pty 触达)与 `luna_loop.c` 的
      事件循环分支(79%);本轮新增特性全在 Lua 策略层(gcov 不可见),由上述四组
      逐分支用例覆盖。逐分支逼近 100% 需 pty 驱动的行编辑测试与 luacov 接线,记入
      下轮方向,不在本轮。

## 上轮遗留:replxx 行编辑桥(2026-09-26 第二轮)

上轮把"pty 驱动的行编辑测试"记为下轮方向,本轮接上:先重写 `test/luna_test_line.c`
(真二进制 + pty,17 例),在其中复现了三个真缺陷并修复——不是测试写错,是实现错了。

- [x] **`^C` 取消被当成 EOF**:`replxx_input` 对 ^C(abort_line)和 ^D(send_eof)
      都返回 NULL,原 `lline_read` 一律答 `nil,"eof"`,`repl.run` 见 nil 就结束会话
      ——编辑器里按一次 ^C 整个 REPL 退出。修复:abort_line 在返回前设置 **EAGAIN**
      (该路径此前不碰 errno),读前清零、读后即判——EAGAIN 走 `("", true)`,
      其余才是 EOF;`repl.run` 改收 `line, aborted`,取消时丢弃当前行与
      `session.pending`(不占 `In[n]`),续行块整体回退到同一编号提示符。
- [x] **`--no-color` / `NO_COLOR` 不降级输入行**:`replxx_set_no_color(g_rx, 0)`
      硬编码。新增 `linedit.set_no_color(on)`,由 `repl.run` 按 `session.color` 下发,
      输入行与回显同进同退。
- [x] **Tab 补全吞掉前文**:`string.su`+Tab 变成 `sub(`、`require "jso`+Tab 变成
      `require json`——候选是插入文本,replxx 却按自己的断词字符抹掉整段上下文。
      修复:`complete.line` 增加第二返回值 `replace_span`(Lua 侧算出该改写的尾段),
      桥把它写进 `*context_len`,仅当是整数且 `0 <= span <= strlen(input)` 才采纳,
      否则回落 replxx 自己推导的上下文。断词字符集本身不动
      (` \t\r\n,:()[]{}`:点号与引号本就不在其中,点号链因此不被拆;
      冒号保留,给不报 span 的源兜底出只含方法名的上下文)。
      【审核更正 2026-09-26:原记"把 `.`/`:` 移出断词字符集"与代码不符,
      该集合此次并未改动——起作用的是显式 span。】
- [x] **`lline_set_highlighter(nil)` 的 NULL 解引用**:清空高亮钩子在任何 read
      之前调用时 `g_rx` 还是 NULL,`replxx_set_highlighter_callback(g_rx, …)` 会砸;
      改为先 `ensure_rx(L)`。
- [x] **测试**:line 组(pty 驱动真二进制)17 例重写;新增 **linedit 组 29 例**
      (白盒:`#include "luna_line.c"` 直接驱动回调与唤醒线程,含 `^C`/键入/唤醒/
      EOF 四态由 pty 子进程复核)、complete 组 +4 例 span;`test/CMakeLists.txt`
      因 replxx 候选表是 C++ 侧不透明类型,加 `replxx_completions_shim.cxx` 视图,
      `project(... LANGUAGES C CXX)`。文档同步 `docs/guide/cli-repl.md`
      (输入模型:^C 语义、Tab 插入/替换段、NO_COLOR 输入行同退)。
- [x] **覆盖率**:`cmake --build` + `ctest` **14 组全绿**(build 与 build-cov 插桩树
      各跑一遍)。实测(2026-09-26,gcovr):C 侧 `src/` 行 **80.6%**、函数
      **86.8%**、分支 **55.2%**(上轮基线 75.8% / 81.8% / 49%);
      **`luna_line.c` 行 142/142、函数 14/14、分支 70/70,全部 100%**(上轮 2%)。
      桥的失败路径也各有归属:`RLIMIT_NOFILE` 收紧逼出 pipe() 失败、
      `RLIMIT_NPROC` 收紧逼出 pthread_create 失败、唤醒线程以管道 EOF 收尾。
      剩余缺口:`luna_loop.c` 分支 51%、`luna_kernel.c` 行 83%、`luna_main.c` 行 77%。

## 信号语义与可达性(2026-09-26 第三轮)

- [x] **一次性模式 `^C` 的退出码与脚本不一致**:`Session:feed` 两条错误路径补第二返回值
      `err`,`luna.exit_code_for(err)` 把 `interrupted` 映射为 **130**(128+SIGINT),
      `run_eval` 与 `run_script` 同口径;`finish(code)` 由 `os.exit` 改为**返回**码,
      `main` 尾部 `lua_close` 后 `return rc` 正常执行(状态真正关闭,被杀进程不写
      `.gcda` 的老毛病从根上少一半)。`docs/guide/cli-repl.md` 的 `^C` 条目补 130 约定。
- [x] **信号与挂载路径没有用例**:cli 组加 `run_luna_signalled` 辅助(fork+exec、stderr
      就绪标记、可选静默、超时收割)三例:`-e` 中断 130、脚本中断 130、`--no-serve` 下长转
      脚本中断 130(钉住计数钩子每两拍轮询的 else 侧);serve 组两例:SIGUSR1 重绘后 `^D`
      干净退出(进程与套接字都收干净)、`luna --attach` 挂上 `busy.lua` 并在其中求值
      (`attach> 42`)。两个夹具都**自行正常退出**收尾——否则被杀进程的 gcda 直接丢失。
- [x] **attach 失败面无人报错**:`pcall(kernel.wake, 2147483647)`、`kernel.chmod` 坏模式/
      不存在路径,报错都带调用名与 errno 原因(kernel 61/75/77 行);`badpoll.lua` 夹具装一个
      必然抛错的 `__LUNA_SERVE_STEP`,计数钩子的 pcall 必须吞掉(98 行),chunk 活到 `done`。
- [x] **错误对象与续行启发式**:repl 组 4 例——`error(setmetatable({}, {__tostring=…}))`
      直接以 `__tostring` 为消息、`error({})` 落到 `(error object is not a string)` + traceback
      (msghandler 248-250 行);`1 +   `(尾随空白)、`not`(裸关键字)、跨行短字符串(原始
      换行落在引号内,走 `unfinished string` 分支而非 `<eof>`,kernel 186/197/229 行)都续行。
      语义未改(见"明确不做":引号跨行仍无解),只是把现状钉住。
- [x] **死代码**:`dbg_msgh`;`luna_serve_flag` + `luna_kernel_request_serve()` +
      `k_serve_requested` 一整条无消费者的链;SIGUSR1 处理只留 `luna_line_notify_wake()`。
- [x] **入口失败无人测**:新增 **main 组**(白盒 `#include "luna_main.c"`,`main` 改名,与
      linedit 组同法):`run_chunk` 报 `luna: =(luna): <msg>` 并返回 1、`setup_module_paths`
      遇到没有 `package` 表的状态走守卫。CLI 任何模式都到不了这里(每个模式各自受保护),
      只能白盒驱动。
- [x] **插件清单的失败原因**:dkjson 的 `json.decode` 失败是**返回** `nil, pos, err`(不抛),
      `manifest_of` 接住它,坏 JSON 报 `plugin.json does not parse: <原因>`,清单真缺名字才报
      `has no usable name`;新增 `noname` 夹具,两例各钉一条,坏清单不中断启动。
- [x] **覆盖率**:`cmake --build` + `ctest` **15 组全绿**(build 与 build-cov 插桩树各跑一遍)。
      实测(2026-09-26,gcovr;统计口径不含同日并行接入的 luacov 插桩段):C 侧 `src/` 行
      **82%**、分支 **57%**(上轮 80.6% / 55.2%);**`luna_kernel.c` 行 98%**(189/192)、
      分支 80%;**`luna_main.c` 行 96%**(103/107,main 组补上最后可达的 4 行);
      `luna_loop.c` 行 79% / 分支 51%;**`luna_line.c` 仍 100%**。余量:kernel 的
      188/274/296(全空白却解析失败、load 报错无消息、msghandler 返回非串——按构造不可达),
      main 的 218-219/280/284(建态失败、入口失败时的 `rc=1`、入口返回非整数——防御分支),
      以及 luna_loop 的事件循环分支(下轮方向)。

## 覆盖率接线(2026-09-26 第四轮)

- [x] **luacov 接入(上一轮方向第 1 条)**:submodule `deps/luacov`(v0.17.0)。
      嵌入式源(嵌入式策略层)对 luacov 是无名 dostring,按 `codefromstrings=false`
      直接不可见——解法三层:(1) `luna_chunkname(name)`:覆盖率构建下以
      `'@<root>/lua/luna/<name>.lua'` 真实文件名加载,普通构建保持 `'=(luna/x)'`
      原样(报错文本不变);`run_chunk` 同步改 `luaL_loadbuffer`,入口
      `lua/luna.lua` 也进报告(此前整文件不可见,总数从虚高的 84.49% 落到真实的
      **84.14%**)。(2) 每个 Lua 态只有一个 debug 钩子槽:覆盖率接管后装**合并钩子**,
      行事件分发 `covhook(nil, line, 3)`(官方 level-3 扩展点,跳过钩子与包装层),
      count 事件转发 `kernel.count_hook()`——`^C` 中断与 attach 轮询节奏不变。
      (3) kernel 变成钩子无关:`__LUNA_SERVE_STEP` 轮询前保存/恢复
      `lua_gethook/mask/count`,`kernel.run` 仅在槽位空闲时武装计数钩子,收尾恢复
      而非清除。测试侧:`test/luna_cov.h`(`LUNA_COVERAGE=1` 环境门控,仅 Profiling
      树导出)+ 九个 harness 接线;`lua_coverage` / `gcovr_summary` 两个 target 出
      报告,汇总表打进构建输出;`docs/guide/getting-started.md` 补覆盖率一节。
- [x] **顺带真修两处**(覆盖率插件暴露的潜伏 bug,非测试迁就):
      `lua/luna/modules.lua` 的 `caller_dir` 以"第一个 `@` 源帧"定位调用方——模块
      自身以真实路径加载时(恰是覆盖率构建)会停在自己身上,改为跳过与自己
      `debug.getinfo(…, "S").source` 相同的帧;loop 组的 DNS 失败用例在本机
      capture-DNS(faux-IP)环境恒假绿(`.invalid` 也能解析),换成**解析器之下的**
      确定性注入:70 字符标签(查询构造期即败)、300 字符主机名(libuv 同步
      `UV_EINVAL`)、非数值地址反解,三例新增/替换后 loop 组 85 例。
- [x] **实测(2026-09-26,不虚报)**:build 与 build-cov 两树 `cmake --build` +
      ctest **15 组全绿**(覆盖率树串行 214s,统计文件合并假定无并发写)。
      Lua 侧(luacov):总 **84.14%**(1539/1829)——repl 98.16%、complete 97.93%、
      introspect 95.78%、highlight 93.88%、plugins 93.18%、magic 88.74%、
      modules 87.39%、luna.lua(入口)80.14%、http 82.67%、rocks 75.58%、
      **serve 39.84%**。C 侧(gcovr):行 **80.8%**(2236/2769)、函数 88.9%、
      分支 57.3%;kernel 行 98%、line 100%、loop 79%、main 63%(缺口主要是
      覆盖率接线自身与建态失败等防御分支,见第三轮)。
- 余量(如实):serve.lua 的 39.84% 是 attach 的捕获汇与错误分支——`--attach`
  的双进程对拍在串行 ctest 里能到,但快照断言对调度顺序敏感,已有 3 例钉住
  主路径;rocks 75.58% 的缺口在网络下载分支(离线环境不可注入)。剩余大头
  仍是 **`luna_loop.c` 分支**(见下轮方向)。

## luna_loop.c 失败注入(2026-09-26 第五轮)

- [x] **接管并行 worker 的内核修复**:上一轮 server 组测试暴露的缺陷被并行 worker
      会话定位并修复——`server_close`/`tserver_close` 在 `uv_close` 之前就 unref
      `closeref`,关闭回调被搁浅(注册了也永不交付)。修复:closeref 保持 pin 到句柄
      真正关闭,由 `on_server_closed`/`on_tserver_closed` 交付并释放(NOREF 有守卫,
      selfref 全程钉住 userdata)。两个新测试钉住契约:`server:close(cb)` 抛错被吃、
      TLS 服务器的 onConn/close-cb 抛错被吃。
- [x] **失败注入 27 例**(loop 组 85 → 112,普通树与覆盖率树同套用例):fs 全操作
      错误链(11 个操作的回调首参携带失败)、readFile 读目录(EISDIR,开成功读失败
      的中链)、fifo 的 stat.type='other'、回调抛错全家(定时器/fs/fs.watch/signal/
      sock/dns/process,字符串与表两种错误对象)、`loop.run('bogus')` 模式校验、
      句柄 tostring 全覆盖(timer/immediate/fswatch/sigwatch/udpsock/server/tsock)、
      非数值主机与端口越界的 listen/udp.bind 拒绝、IPv6 回环往返(family='inet6')、
      connect 后 run 前的 sock:read 报错、EOF 后再读(立即补发 EOF)、close 后的
      write(回调静默不交付)、`sock:shutdown` 半关、被占路径的 listenPipe、
      TLS 握手对纯 TCP 服务器失败、TLS 服务器丢弃纯客户端后继续服务正常 TLS 客户端、
      kill-after-exit 抛错。worker 另修 fs.watch 测试的固定 300ms 竞速(改为事件驱动
      判定 + 保底计时器),watch/signal 双事件断言改序无关。
- [x] **实测(2026-09-26,不虚报)**:两树 `cmake --build` + ctest 全绿(普通树 15 组
      ×8 轮;覆盖率树串行——见环境记录)。C 侧(gcovr):行 **85.0%**(2349/2765)、
      函数 **91.4%**(223/244)、分支 **63.6%**(743/1168;上轮 57.3%)。
      **`luna_loop.c` 行 84%**(1880/2220,上轮 79%),kernel 98%、line 100%。
      Lua 侧复测 **84.25%**(1541/1829;上轮 84.14%,本轮无 Lua 层改动,波动来自
      rocks 组本轮完整跑完)。
- 余量如实:loop 剩余缺口集中在分配失败守卫(OOM/ENOMEM)、TLS pend-flush 内部支路
  (Lua 面握手前拿不到 sock,pending 写不可达)、uv 同步失败支路(需内核级条件)——
  按构造不可注入,不虚补。
- 环境记录:机器负载 80+ 时(多会话并行编译),rocks 组的真实网络安装会撞测试内
  150s 超时(0x7c=124,`timeout` 杀安装子进程),与代码改动无关;负载回落后同树全绿。

## serve.lua attach 捕获路径(2026-09-26 第六轮)

- [x] **根因是一个真 bug,不是测试缺口**:serve.step 在嵌套 dispatch 前调
      `debug.sethook()` 清钩子,搁浅调用方装的任何包装器——覆盖率构建的
      luacov 行追踪器首当其冲,这正是 serve.lua 长期 39.84% 暗掉的原因(^C
      计数钩子同样被搁浅)。kernel.exec 早已自管钩子槽(save/restore 包住
      自己的计数钩子),该行陈旧,删除。
- [x] **第二个同路径真 bug**:`magic.dispatch` 对失败的 magic 从不 raise——
      以 `(nil, message)` 返回(未知 magic、`%time` 用法错等),而 serve 的
      `pcall` 只收首参,错误文本被丢在地上,远端只见裸 `OK`。改为接收
      `(okd, okm, merr)` 并在 `not okm` 时构 ERR 帧——typo 的 magic 现在
      真正可见。
- [x] **12 个新测试**(serve 组 14 → 26,全部单步确定、对调度顺序不敏感):
      start 幂等与 disabled;bind 失败(坑:常规文件会被 start 的陈旧清理
      `os.remove` 掉,得用非空目录占位);socket.unix 不可解析时新模块副本
      报 "socket.unix unavailable"(同一 Lua 态内清 `package.loaded`+`preload`
      再 require,靠 luna_chunkname 合并进同一覆盖块,无需新开 state);
      零候选补全的空体帧(坑:unparseable 前缀回退列出全部 globals,匹配
      不到的合法前缀才返回空);`%` 空魔法的 ERR;`%time` 用法错与未知
      magic 的 ERR(修 #2 后可钉);补全/魔法引擎不可达(清 loaded+preload
      注入 require 失败再还原);`?expr`/`expr?` 双糖与糖内语法错误;
      receive 抛毒即弃客户端(可注入的 client seam);EOF 弃客户端;发完行
      再关闭 → send 失败弃客户端;`__tostring` 恒抛的错误对象在 dispatch
      内炸开、逃到 step 外层 pcall、手工 ERR 构帧仍出货(钉住兜底行)。
- [x] **实测(2026-09-26,不虚报)**:两树 `cmake --build` + ctest **15 组全绿**
      (普通树 82s;覆盖率树串行 289s)。Lua 侧:总 **88.48%**(1620/211,
      上轮 84.25%),**serve.lua 100.00%**(130/130,0 未覆盖;39.84% 起步)。
- 余量如实:serve.lua 已清零,无"不可注入"残留。Lua 侧剩余大头:rocks
  75.58%(网络下载分支,离线不可注入)、luna.lua 入口 80.14%(交互面)、
  magic 88.74%。
- 环境记录(重演第五轮模式):负载 76 时覆盖率树 rocks 撞 420s 超时
  (0x7c=124),回落到 44 后同树全绿——负载 >40 不取 rocks 终绿的纪律继续有效。

## magic / 入口 / C 侧诚实账(2026-09-26 第七轮)

- [x] **magic.lua 88.74% → 100.00%**(151 行 0 缺):worker 递交的 9 个
      "coverage gap" 测试全绿采纳——echo_result 早退、%timeit 用法错、
      %time/%timeit 执行错、%clear 的 tty 分支(patch `k.tty` 后直调)、
      plugins 加载器不可达/加载失败/被遮蔽、%exit 的 `os.exit` mock
      (mock 让 `os.exit(0)` 那行真实执行而不杀进程——比"构造性不可达"
      的旧判定干净:覆盖账上这行本就可达,只是代价是进程生命)。
- [x] **luna.lua 80.14% → 95.27%**(29 暗行 → 7):pty 驱动真二进制的
      交互面——`%clear` 清屏序列(注意 raw-mode pty 下提交字节是 `\r`
      不是 `\n`,LF 会被原样塞进行编辑缓冲)、`%exit` 干净退出(os.exit
      绕过 serve.stop,测试须自清 socket 文件)、attach 补全桥
      (`zeb<Tab>` → `zebra_crossing`,跨进程 SIGUSR1 唤醒)、attach 目标
      死亡/连接失败/错误帧上 stderr。
- [x] **C 侧记账更正:main.c 真实 90.44%(136 行),此前报的 62% 是
      双重假象**。白盒 main 测试把 src/luna_main.c 编进自己,其 gcno
      在 gcda 缺席时会让 gcov 产全零 JSON(再拖一份暗行集进来),陈旧
      gcda 又会被 gcov 拿去配对——两路合谋把 136 行真集撑成 198 行
      并集。同时揭穿 luna_line.c 的假 100%:白盒 linedit 副本的计数
      灌水,真实 85%。修法:gcovr_summary 报告前清场(删 gcov-discarded/
      与 test 树全部 gcda/gcno),报告只信真实二进制与静态库自己的
      计数。坑:gcovr 的 --exclude/--gcov-exclude 都挡不住"零数据
      gcno"路径,清场是唯一可靠机制;cmake VERBATIM 直传 argv,find
      的模式不要带 shell 引号。
- [x] **worker 的 8 个红测试重写为真 API**(假设错但覆盖目标对):
      introspect——signature 非函数返 nil(77)、Lua 函数 help 带
      "defined at"(95)、doc_for 的种子路径与嵌套值反查(103/113)、
      suggest 用错误消息精确断言双候选形式(251);highlight——
      set_style 直写 styles(37)、lpeg 缺席时引擎加载失败(58)、
      lexer.load 败(62)、lex 抛错回退纯文本(78)、末标签后的尾段
      原样拷贝(97)。另有 magic 一处误导性注释修正、docs 首页 SVG
      图标与 docs/package-lock.json 采纳;根目录孤儿 package-lock.json
      (无 manifest 的空锁)判明为垃圾删除。
- [x] **上轮一处记账更正**:serve.lua 上轮报的 100.00% 有虚——exotic
      `__tostring` 测试实际走的是 kernel.exec 的 C 侧错误摊平(eval
      分支自己构 ERR),step() 的 not-okd 兜底行从未被钉(该测试注释
      描述的机制不成立,已修正)。新增
      test_a_raising_completion_candidate_still_frames:补全源返回
      `__tostring` 恒抛的候选,dispatch 的 tostring 循环(未包 pcall)
      炸开落进外层 pcall——兜底行现在被真实覆盖,serve.lua 100.00%
      (130/130)这次是真的。
- [x] **实测(2026-09-26,不虚报)**:两树 `cmake --build` + ctest
      **15 组全绿**(覆盖率树 291.7s;负载 ≤16,rocks 196s 一次过)。
      Lua 侧总 **91.33%**(1674 行 159 缺,上轮 88.48%):magic
      100.00%、highlight 100.00%、introspect 99.40%、complete
      100.00%、serve 100.00%、luna.lua 95.27%。C 侧(诚实账)总
      **86.1%**:kernel 98%(202/205)、main 90.44%(123/136)、line
      85%(121/142)、loop 84%(1881/2220)。
- 余量如实:introspect.lua:241(suggest 次优候选的条件行)留黑——
  按降序到达的 pairs 顺序永远走不到,行覆盖本身顺序依赖;改为断言
  其确定输出(best/second = 距离最小者按字母序取二),一个 flaky
  测试比一个暗行更糟。luna.lua 余 7 行:3 行构造性不可达(chunkname
  回退属普通构建;socket.unix 捆绑必在),4 行 attach 会话将死时的
  竞态恢复腿(对端关闭后 send 仍可能缓冲成功,钉住须假设 socket
  拆除顺序,断言从宽保确定性)。repl 98.16%、modules 87.39%、
  plugins 93.18%、rocks 75.58%(网络下载分支,离线不可注入)。

## loop 长尾 / modules·plugins 边角(2026-09-26 第八轮)

- [x] **luna_loop.c 84.48% → 91.16%**(gcov 口径;gcovr 诚实账 91%,
      2208 行 2014 执行):29 个新 C 测试——21 个常规面 + 8 个 TLS 面,
      全部真 socket/真 libuv 事件循环。常规面:loop.now 单调毫秒、
      signal start 失败清理后抛错、关闭后 watcher 的 tostring/二次
      close、unref/ref 往返(含活进程)、进程 tostring 状态、spawn
      缺失二进制抛错并排水、大 stdout 增长捕获、UDP 边角先于循环
      抛错/v6 往返/零长数据报/close 后送达静默/收发回调可抛、
      shutdown 只关写半、按主机名经解析器 connect、完成前关闭、
      无读者的 EOF 路径、server conn 回调可抛、fs.write ENOSPC 经
      回调上报、坏 attach step 不停时钟。TLS 面:监听坐标与关闭后
      操作抛错、坏 host/被占端口拒绝、坏 CA 文件、按主机名拨号
      (insecure——证书 SAN 是 IP 非 localhost DNS)、解析完成前关闭
      ("tls: closed before handshake" 投到 connectref)、读写回调
      中途换法/tostring、写回调抛错不炸循环、64MB 大写强制
      pend→flush(本机内核同步吞 32MB,64MB 才出真部分写;flush
      CPU-bound ~1.5s,bail 定时器 8s 且落 log[7],防中途改写诊断)。
- [x] **modules.lua 87.39% → 100.00%(111/111)、plugins.lua
      93.18% → 100.00%(88/88)**:9 个新测试。modules——install
      重复报 already-there、相对目录 init.lua 约定与 miss、不可加载
      入口与空名落穿(native searcher 会接住 require(''),须直调
      M.searcher 断言)、清单 main 的四种拼写(不带后缀→补 .lua、
      目录→sub/init.lua、缺失→nil、坏块→报错)、lfs 在场时相对
      caller 按源路径绝对化、lfs 缺席时 start-dir 回退照常找名。
      plugins——"main" 不带 .lua 可加载且编译失败的入口按插件目录
      入 failed、无 plugin.json 的目录是独立失败原因、lfs/json 双缺
      时 discover 降级(stub-reload 出新模块实例,"json support
      unavailable" 入 failed)。
- [x] **写测试逼出 5 处真产品 bug + 1 处死代码**(过程即审计,均在
      src/luna_loop.c):
      1. `net.end()` 二次调用失败会剥掉首次 shutdown 的待完成回调
         引用——endref 改为 shutdown 成功才提交;
      2. `net.connectTls` 返回的是回调不是 sock(tsock_new 的
         luaL_ref 弹出 userdata,栈顶只剩回调)——rawgeti 取回;
      3. TLS 部分写只持余量又按余量重试,双违 OpenSSL 契约:未完成
         的写必须同 (buf,len) 重试(违者 bad write retry),且按余量
         长度读会越过分配(堆越读)——改为整包持有、原长重试;
      4. 缺 SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER(缓冲自 Lua 串搬进
         malloc)与 SSL_MODE_AUTO_RETRY(TLS 1.3 会话票让无读回调的
         写永远 WANT_READ 卡死)——两个 mode 补齐;
      5. tls_pump 重入:读回调在投递栈内再 :read() 无限递归炸
         "C stack overflow"——pumping 守卫 + pump_body 拆分;
      6. tserver_deliver_conn 死代码删除(14 行零调用者)。
- [x] **attach_target.lua 孤儿保护**:60s os.clock() 死线 +
      os.exit(1)——上轮两个各烧 1h14m 的挂死 fixture 进程即此漏出。
- [x] **实测(2026-09-26,不虚报)**:两树 `cmake --build` + ctest
      **15 组全绿**。Lua 侧总 **92.69%**(1699 行 134 缺,上轮
      91.33%):modules、plugins 加入 magic/highlight/complete/serve
      的 100% 行列,introspect 99.40%、repl 98.16%、luna 95.27%、
      rocks 77.52%。C 侧(gcovr 诚实账)总 **91%**(2457/2691,函数
      97.5%、分支 68.9%):kernel 98%、loop 91%(2208 行,上轮
      84%)、line 85%、main 89%(121/136——比上轮账面少 2 行:main.c 本轮
      零改动,差额落在随运行窗口波动的腿上(coverage 环境腿/REPL
      退出码腿),以本轮可复现的全量 ctest 新账为准)。
- 余量如实:loop 剩 194 行是明账——OOM 腿(tsock_new、connect/listen
  的 tls 新建、luaL_newstate)、同步 getaddrinfo/socket/connect 系统
  调用失败腿、l_tsock_write 零进展 WANT_* 腿、on_tserver_event 竞态
  accept 腿、os.* getter 系统调用失败抛错、UDP ICMP 腿、proc/fs/
  fs-watch 竞态腿;要么需 malloc 注入基建,要么断言依赖拆除顺序,
  见"明确不做"。bundled 的 loop/http.lua 82.67%(65 行)历轮未专项,
  计入 Lua Total。

## 下轮方向

- **C 侧 luna_line.c(85%,21 行)与 luna_main.c(89%,15 行)**:
  line 的余量集中在 tty 原始模式与 resize 腿,main 的余量是 OOM 与
  coverage 环境腿;同款"驱动到诚实极限 + 表征余量"套路。
- **loop/http.lua(82.67%,65 行)**:bundled HTTP 模块从未专项,
  单测基建(modules harness)现成。

## 明确不做(上一轮)

- luna_loop.c 剩余 194 行——OOM 腿需 malloc 注入基建,竞态/拆除顺序
  腿断言从宽换确定性,已逐函数表征(见上),不为行覆盖数字写会
  flaky 的测试;
- rocks.lua 网络下载分支(离线不可注入)、introspect.lua:241(pairs
  顺序依赖,已用确定输出断言替代)、luna.lua attach 竞态腿(均维持
  前轮判定);
- 未闭合引号跨行续行的语义修补(`s = "abc` 回车后无解)——`^C` 丢弃
  整块已可用,改语义动 `kernel.check` 契约,收益低(维持前轮判定);
- 打 tag / 发版(硬约束禁止)。
