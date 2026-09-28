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

## line/main 余量 + http.lua 专项(2026-09-26 第九轮)

- [x] **luna_line.c 85% → 97.14%(gcov 口径;gcovr 诚实账 97%,140 行
      136 执行)**:6 个新 pty 测试,全部走真实 REPL 会话从会话内
      换 hook——清 completion 后 Tab 走桥的 ref 守卫(113)、raising
      completion 桥静默(118-119)、坏形状(非表首返回值)丢弃
      (124-125)、raising highlighter 每次重绘被 C 侧接住(155-156)、
      清 highlighter(205-209)、history_save 坏路径双返回值上报
      (289-291)。会话内访问模块要用 require('linedit')(repl 里
      不是全局)。
- [x] **luna_main.c 89% → 90%(123/136)**:cli 新测试 env -u
      LUNA_COVERAGE 驱动 coverage_init(241)与 coverage_shutdown
      (279)的未插桩早退腿——子进程环境剥掉变量后照常出
      Out[1]: 42。
- [x] **loop/http.lua 82.67% → 96.59%(368/381,余 13 行)**:24 个
      新专项测试,现成 modules harness。客户端面:bare URL 补根路径、
      head/chunk-size/chunk-body 各自成片到达(块体跨界时解码器持
      片等待,经 60ms 分片稳定驱动,不吃环回合包的运气)、CL body
      跨读、坏 chunk size 与非 HTTP 头报错、重复头 join、五种
      Location 形状(绝对/协议相对/上跳 ..//深层/不可解析)、204 无
      体即完、onHead/onData 可 raise(err 经回调上报)、流式分片
      单片交付、无框架 EOF 体、头前 EOF、体中 EOF、连接拒绝、
      中途 RST 仍出错误。服务端面:重复 res 幂等、客户端闪断后
      恢复、runaway 头 400、重复请求头 join + 慢体、listen 无
      handler 报错。
- [x] **写测试逼出 2 处真产品 bug + 1 处测试时序余量**:
      1. `http.lua` 重定向吞错(本轮头号发现):feed 的 redirect
         分支先置 `st.done` 再 resolve_location,Location 不可解析
         时 finish(nerr) 撞上 finish 自己的 done 守卫被静默——
         请求永远挂死、回调一次不响。改为先解析、失败即 finish,
         成功才置 done/close/递归(旧套接字迟到投递保护保留);
      2. `luna_line.c` set_highlighter(nil) 段错误:清除腿调了
         replxx_set_highlighter_callback(g_rx, NULL, NULL),而
         replxx 的 C 包装对 NULL fn 也无条件 bind 成可调用体,
         下次重绘即空调用指针。改为对齐 set_completion 的形态:
         只清 g_highlight_ref,桥自己的 ref 守卫(151)兜底;
      3. serve attach pty 的 %exit 窗口 10s → 30s:投递靠目标端
         busy loop 的指令计数钩子,满载并发时 10s 内指令量不足
         (单跑绿、两树并发各挂一次的形态即此),窗口放宽为环境
         表征,非覆盖。
- 余量如实:line 剩 4 行全表征——54(wake-thread teardown)、62
  (双 init 构造不可达)、64(pipe 建立失败需 fd 耗尽)、176(尾部
  填色需 codepoint/size 不匹配);main 剩 13 行——78(openlibs 后
  不变量)、205-207+367(嵌入入口失败,无 CLI 形态可注)、
  269-272+282(需套件运行中变异共享 luacov.config,flake)、
  296-297(RLIMIT_AS 调参环境依赖)、371(luna.lua 恒
  return finish(integer),构造不可达);http.lua 剩 13 行——74
  (https 默认端口腿)、249/293/379/460(交叠与拆除顺序 return)、
  473+484-488(无框架流式缓冲,死防御分支)、514(写错误腿)、
  675(accept 错误竞态)。均不为行数写 flaky 测试。
- [x] **实测(2026-09-26,不虚报)**:两树 cmake --build + ctest
      **15 组全绿**(serve 30s 窗口修后两树各复跑全量一次通过)。
      C 侧(gcovr 诚实账)总 **91% → 92%**(2475/2689,函数
      97.5%、分支 70.0%):kernel 98%、loop 91%(2015/2208 持平)、
      line 97%、main 90%。Lua 侧总 **92.69% → 95.54%**(1839 行
      82 缺):http.lua 96.59%,modules/plugins/magic/highlight/
      complete/serve 100%,introspect 99.40%、repl 98.16%、
      luna 95.27%、rocks 77.52%。

## Lua 侧长尾:rocks/luna/repl + serve 真 flake 根因(2026-09-27 第十轮)

- [x] **rocks.lua 77.52% → 92.25%(258 行 238 执行,余 20 行,单文件
      最大缺额收掉 38 行)**,全部离线腿、零网络新增依赖:
  - modules 组(嵌入式 VM,注入 rocks 源 + os.exit 桩):dkjson
    缺席的加载报错(30)、vendor_dir 空值/不存在两腿在 luarocks
    加载前 die(134-135/150/160-162)、dispatch 未知命令 exit 1
    (484-486);
  - rocks 组(真实二进制,每个测试独立 scratch 目录):install
    usage(432-433)、update 无锁(339-340/445/476-477)、锁坏
    JSON(346-348)、空锁离线重锁输出 locked 0 rocks
    (447-449/456-457/238-239)、--from-lock 缓存 sha 不符拒绝
    (398-403)、.luna/rocks 树注入 package.path/cpath 含子目录
    walk-up(104-108/492-501)、空树 list 安静成功(run_luarocks
    主体 + 478-482)。
- [x] **repl.lua 98.16% → 100%(163/163)**:cli 组两个测试——坏表达式
      的 `?` 糖按普通求值错误上报且会话继续(show_help 错误腿
      191-192)、whos() 注入表列出全局(255)。
- [x] **luna.lua 95.27% → 95.95%(余 6 行,attach 中途杀目标驱动
      194)**:新 pty 测试杀目标后 Tab——补全回调收不到 OK 框,降级
      空列表(194);随后提交行拿到 "no reply/went away" 之一、
      客户端 exit 1。剩余 6 行全表征:24(preload 注册,
      coverage 口径产物)、158-159(attach 缺 socket.unix,捆绑
      构建恒在,不可达)、181/221-222(send-fail 腿,send 落进
      死端缓冲不报错、EPIPE 时序不可稳定注入;其 receive-fail
      孪生腿已驱动)。
- [x] **逼出并根因修复 serve 组 %exit 真 flake(测试侧)**:旧测试
      在"42"一出现就写 %exit\r,此时前台 attacher 还在两次
      linedit.read 之间(终端 cooked 模式)——击键被行规程回显并
      整行排队,replxx 回 raw 模式后按批处理该行,线缆上出现双
      重渲染后编辑器/客户端卡死,30s 收包超时超出一切 expect 窗口
      (~7% 复现,与负载无关;此前误判为计数钩子延迟放大窗口是
      错误表征)。修复:expect 针从裸 "42" 改为安定针
      "42\r\nattach>"(下一提示符只在编辑器回到 raw 模式后才画),
      窗口回到常规 10s;修复后套件 16 连绿。
- C 侧维持:gcovr 诚实账 92% 持平(2476/2689,+1 行;loop 91%、
  kernel 98%、line 97%、main 90% 均持平),malloc 注入基建按
  第八轮判定继续不投入。
- [x] **实测(2026-09-27,不虚报)**:两树 cmake --build + ctest
      **15 组全绿**。rocks 网络循环测试在负载 80+ 下首跑遇网络
      抖动挂一次(ansicolors 未进树,离线七测全绿,环境性),
      负载回落后复跑通过。Lua 侧总 **95.54% → 97.82%**(1839 行
      40 缺):rocks 92.25%、repl 100%、luna 95.95%、introspect
      99.40%(241 维持)、http 96.59%(维持)、其余 100%。

## C 侧 loop 91% → 95%:两个真产品 bug 根因修复 + 四条真缺腿(2026-09-27 第十一轮)

- [x] **根因修复两个真实产品 bug(ASan 实证,非猜测)**——此前
      各轮"漂移的堆破坏/SEGV/free(): invalid next size"全部归因
      于这两处叠加:
  1. **proc uv_spawn exec 失败句柄泄漏 → UAF**:libuv exec 失败
     从函数底部 return,跳过 error: 标号的 uv__queue_remove
     (deps/libuv process.c,内注 PR 3107),进程句柄留在静态
     g_loop 队列;proc userdata 在该失败用例 lua_close 时释放,
     下一个用例首个 uv__handle_init 经悬空链写穿已释放堆
     (ASan:分配于 l_process_run,写于下一个用例的 uv_pipe_init)。
     修复 proc_spawn_error 增 spawned 参数:uv_spawn 跑过则补
     uv_close(与 node 同契约);args 分配失败腿 spawn 未跑、无
     句柄,不 close。
  2. **tsock ref/unref 越界**:LUNA_HANDLE_CTL 把指针盒 userdata
     (struct tsock **,72 字节)整个当 uv_handle_t——uv_ref/
     uv_unref 把 GC 头当 type 字段读、越过盒尾读句柄字段(句柄
     ~90 字节),ASan 实证 heap-buffer-overflow READ 8。改为解引用
     盒体 + closed 守卫(close 后句柄已终化,ref 只会钉住一个不再
     应答的 loop)。
  3. 顺手真修:**TLS 连接拒绝误报 "bad file descriptor"**——libuv
     把裸 POLLERR 映射 UV_EBADF 后才回调(poll.c),status≠0 臂改问
     SO_ERROR 取真因(负 errno 才印 "connection refused");
     ERR_clear_error() 排空陈旧 OpenSSL 队列防误标后续 tls_fail。
- [x] **四条真缺腿收线 + 20 个吞噬/边界用例**(本会话累计 24 个新
      测试):超长主机名走回调报错(net+tls dns 双腿)、关服后
      accessor 报 getsockname 失败、TLS 拒绝报真因、TLS 对端死亡
      后写报错——另补 net/tls/udp/dns/process/fs 表错误吞噬、
      建连前 peer 抛错、UDP 广播、fs 负长 EINVAL、fd 耗尽、
      pre-handshake 写/close 双 cb、fs/proc/udp OOM 窗口腿。
- [x] **测试侧两个自伤根因修复**(互为表里的 flake 源):
  1. **accept EAGAIN 假死**:套件自投递信号 + Linux 把排队连接的
     中止以 EAGAIN 从 accept 抛出(man 2 accept 明示按 EAGAIN
     处理),echo_serve/sink_main 旧代码视作致命直接关 listener
     → 客户端拿到真拒绝。accept_transient:瞬态错误全重试
     (10s 墙钟界,资源类 1ms 退避),listener 不再自毁。
  2. **1 GiB string.rep 在 ASan 下 23.85s(实测)**:chunk 内先建
     big 后连接 → helper 的 accept 窗口先到期。seed_big_global()
     在 spawn 线程前从 C 建全局;两块有 listener 的 chunk 移除
     chunk 内构建。loop 组 TIMEOUT 60→240(净跑 ~50s,负载下
     2-4x,60 恒在假失败边缘)。
- [x] **实测(2026-09-27,不虚报)**:ASan 树 **186/186 零
      sanitizer 报错**(allocator_may_return_null=1 让 OOM 窗口走
      产品 NULL 路径);两树 cmake --build + ctest **15 组全绿**。
      rocks 组两树各遇 GitHub tarball 下载抖动挂 1-2 次(网络性,
      build 树重跑通过、cov 树间隔重跑 try1 通过,与第十轮同类
      环境性)。C 侧 gcovr 诚实账 **92% → 95.3%**(2579/2706,
      函数 100%、分支 73.5%):loop **91% → 95%**(2118/2225,
      107 缺)、kernel 98%、line 97%、main 90% 均维持;Lua 侧
      无改动,97.82% 维持。

## loop.c 107 缺行逐腿表征(2026-09-27 第十一轮复核,行号为当前树)

- **libuv 契约性死腿(构造性不可达)**:1716-1717/1719/1723-1724/
  1741/1744/1747-1748/1753-1755(server accept 错误臂——stream.c
  uv__server_io 只以 status 0 调 connection_cb,EMFILE/ENFILE 由
  uv__emfile_trick 内部消化,uv_accept 对已排队健康连接不失败;
  connref==LUA_NOREF 丢弃臂为防御)、3345-3346/3348/3352/
  3358-3359(tserver 同簇:poll 错误/EAGAIN/accept 失败臂)、
  1449(NOREF 防御守卫)、1492-1493(libuv 读回调 n==0 与
  UV_ENOBUFS 契约臂);
- **OOM knife-edge 不分离**(RLIMIT_AS 窗口对小额分配临界,离线
  不可确定性注入):1097(scandir 中途)、1352、2284、2334、2368、
  2655、2684-2685、2984、3012-3013、3038-3039、3397、3133、
  3631(proc argv 增长 realloc);
- **同步系统调用臂不可离线注入**:2405/2417/2429/2440/2451/2491/
  2527(os 的 home/temp/hostname/uname/uptime/cpu/接口——平台恒真);
  612/652-656(fs 写重启半途 EAGAIN)、955/959-961/967/988-989
  (fswatch 内核错误孪生);
- **竞态/孪生腿(异步兄弟已驱动)**:1265(mid-connect 用户 close)、
  1481(closing 双回调)、1492 孪生、1588/1603-1604(未知地址族,
  平台枚举外)、1623(getnameinfo 同步孪生;async 已驱动)、
  2008/2012-2014/2047-2049(udp recv 错误孪生)、2174-2176
  (uv_udp_send 失败)、2377-2380(getnameinfo 同步失败孪生)、
  2739(tls 无队列错误串)、2765/2854-2855/2883-2884/3124-3125
  (WANT_READ/WANT_WRITE 与握手竞态孪生)、2893/2906(SO_ERROR
  查询失败 fallback——getsockopt 对活 fd 不失败)、2917-2918
  (tls_fail 空串孪生)、3168(closeref 竞态孪生)、3423-3425
  (listen 后 accept errno 臂)、3446-3447(tserver close 孪生)、
  3680(proc 回调臂)、3811(proc title NULL)、4048(uv_loop_init
  失败,仅系统级资源耗尽)、3755(p:kill 报错臂——p:kill(9999)
  EINVAL 理论可驱动,为 1 行引入跨平台信号号假设,判定不值得);
- **TLS 同步 connect 双臂**:3071-3073(同步失败)/3079-3082
  (即时成功)——loopback 上 connect 恒 EINPROGRESS 或内核即时
  完成且不可禁用,两臂均不可离线确定性触发;
- 1222(半读回调 lstring 臂,半包时序不可稳定注入)。


## tsock 结构体泄漏专项:__gc 释放路径 + 无读回调热旋产品 bug(2026-09-27 第十二轮)

- [x] **tsock 释放路径(本轮唯一排期项)**:`struct tsock` calloc
  后确实从不释放,本轮回收。设计要点与竞态论证:
  1. **`__gc` 只在 `t->selfref == LUA_NOREF` 时 free**(l_tsock_gc,
     loop.c:3331):selfref 在握时注册表钉住指针盒,userdata 不可
     达则 `__gc` 根本不会触发;selfref 只在 on_tls_closed(loop.c:
     2966,uv_close 完成回调)里解除——此刻句柄已关、回调全部
     交付完毕,此后既无 Lua 方法能再拿盒、也无 uv 工作会碰 `t`,
     free 天然无竞态,不需要代际/引用计数。`__gc` 注册在元表
     本体(loop.c:4154,不在 luaL_Reg 的 __index 表):Lua 终结化
     只读元表自身,挂 __index 里静默不触发(首轮实测踩过)。
  2. **无 close 的弃置 sock 仍不释放结构体**(selfref 恒在):
     这不是漏——共享静态 g_loop 活得比 lua_State 久,free 会把
     活轮询句柄留在已析构 state 后面(与第十一轮 failed-spawn
     修复同构的跨 state 形状);结构体本身仅 ~200 字节且随句柄
     一起在 close 时归还。
  3. **tsock_close 引用清扫**(loop.c:2981-2997):close 时把
     pend/readref/connectref 全部交付并解除——此前 writeref 在
     EOF 时、readref 在用户 close 时永久钉在注册表(泄漏的是
     回调函数+闭包,不止 40 字节)。
- [x] **顺带根因修复一个既有产品 bug:无读回调热旋**——新 gc
  测试两度 100% CPU 自旋暴露:pump 的无读者守卫直接 return、
  不耗数据,水平触发 poll 恒 READABLE → on_tls_event→pump→
  return 死循环。新增 tsock_discard_readable(loop.c:2813):
  无读者时消费并丢弃数据块;close_notify(r==0)置 eof 并
  tsock_close;真错误 tls_fail。读循环里"回调把自己换下来"
  分支(loop.c:2905)同样走 discard——数据必须离套,否则 poll
  旋死。入口 `t->closed` 守卫(loop.c:2816)挡住 mid-pump 关闭
  后的 SSL_read:负控实验(守卫改 if(0))实测本机 OpenSSL
  3.5.5 公开 API NULL 安全(SSL_read(NULL)→-1、SSL_get_error→
  SSL_ERROR_SSL,3 行探针程序验证),路径退化为对已 close 套
  的 tls_fail(自身早退)——守卫是防御性(3.0 前的 OpenSSL
  对 NULL 解引用)而非承重,按实测表述记录。
- [x] **三个新测试**(189 个,原 188):
  1. test_tsock_gc_releases_struct_at_final_collection:服务端
     驻留盒 k,客户端全过后 `k=nil; collectgarbage('collect')×2`,
     二轮 TLS 往返踩过释放区(tostring/pcall 验证 closed 态),
     ASan 树绿 = 无 UAF、LSan 无泄漏;
  2. test_eof_with_flush_pending_still_answers_write_cb:EOF 而
     flush 未完时 write 回调拿到错误而非悬空(驱动 close 清扫
     的 pend 腿);
  3. test_tls_read_cb_close_mid_batch_never_touches_freed_ssl:
     读回调在第二包仍在途时关闭——驱动 discard 的 closed 守卫
     取用臂(负控证明路径真实到达:DBG 实测 discard 入口
     closed=1 ssl=nil)。
- [x] **实测(2026-09-27,不虚报)**:ASan 树 189/189 ×2 轮
  (detect_leaks=0 与 =1 各一整轮,**0 个 sanitizer 报告、0 泄
  漏**);两树 cmake --build + ctest **15 组全绿×2**(负载 ~45;
  本轮 rocks 首跑即绿,无抖动)。覆盖率 gcovr **95.1%
  (2609/2743)**:较上轮 95.3%(2608/2741)已覆盖行 **+1**,
  新增 2 可执行行(守卫 if/return)双双点亮,百分比回落纯因
  分母增长——按"净覆盖行数增加 + 暗腿逐条表征"口径判不倒退。
  本轮代码暗腿仅余:2835-2836(discard 的硬 SSL 错误臂;与
  2925-2926 有读者孪生同因——tsock 公开 API 无法在握手后发出
  fatal alert,离线不可注入)、2838(while 自然退出死腿,got_eof
  是唯一出口,防御性保留)。
- 环境披露:负控与 DBG 插桩已全部还原移除(git diff 核对);
  eof-flush 测试看门狗 10s→30s(并行门禁下一度过期误报,与
  accept_transient 30s 同理由);两树门禁 ctest 以 --timeout 900
  运行(库内 loop 测试 240s 上限在负载 45+ 下是误报陷阱,仅
  运行参数、库文件未动);首轮 ASan 复跑因负载 45 超出 400s
  预算 EXIT=124,加预算后复跑全绿。


## luna_loop 分支覆盖专项:OOM 守卫补齐 + 参数契约面(2026-09-27 第十三轮)

- [x] **口径与基线更正(非交互,自行假设并注明)**:派发单引用
  的"分支 51%"是早期轮次的陈旧数字;本轮起点实测(gcov -b,
  行号为当前树)**72.68%(705/970 臂)**。按「逐腿表征、不为
  行数写测试」口径,只补真实可达的腿:分配失败守卫的可达子集
  用既有 t_lowermem 窗口驱动,参数校验臂作为 API 契约面钉死,
  其余 231 条臂逐簇表征(见下),「明确不做」清单一律未碰。
- [x] **四个新测试(189 → 193),全部是真实行为契约**:
  1. test_tls_write_oom_leg_reports_a_clean_error:tsock write
     的两个 pend malloc 腿(loop.c:3167 握手前停车、:3190 握手
     后半写停车)在 t_lowermem 窗口下同步抛模块自己的
     "out of memory",连接仍可行走、无半建 pend 残留——TLS 面
     此前是 OOM 窗口唯一没碰到的角落(fs/udp/proc/sock 已有);
  2. test_closed_sock_method_contracts_throw_cleanly:plain/
     udp/tls 三面 16 个关闭态方法探针——write/end 抛
     "socket not connected"、read "socket is closed"、
     peer/sockname "sock is closed"、tsock 四法 "tls sock is
     closed"/"tsock is closed",外加 listenTls 缺 cert/缺 key、
     connectTls 端口越界、listenTls 端口被占(bind 失败腿,
     loop.c:3511)——全部同步抛、无一处碰死句柄;
  3. test_sync_arg_contracts_throw_cleanly:timer/interval 负
     值、signal 0/65、connect/listen/udp.bind/sendto 端口越界
     与负值、dns.reverse 非数字地址、pipe server :port()——
     14 个同步契约臂一次钉死;
  4. test_proc_arg_and_lifecycle_contracts:argv 非字符串拒绝
     (:3857)、run() 进程无 stdio(:4067)、退出后 kill 拒绝
     (:3832)、opts.cwd 真实生效(pwd 落在 /tmp,:3865)。
     备注:run 的 opts 表必须放第三参(args 之后的 [opts]),
     首写把 opts 放在 args 位被当空 argv 吞掉——测试因此多验
     了一个真实调用形状。
- [x] **实测(2026-09-27,不虚报)**:分支 **72.68% → 76.19%**
  (705→739/970,**+34 臂**);行 95.1% → **95.2%**
  (2610/2743,+1 行);gcovr 聚合分支 73.4% → 76.3%(905/1186);
  函数 100% 持平。ASan 树 **193/193**(detect_leaks=1 整轮,
  0 报告 0 泄漏);两树 cmake --build + ctest **15 组全绿×2**
  (--timeout 900)。
- **余 231 臂逐簇表征(行号为当前树,均不再为行数补测)**:
  1. **libuv/构建契约死臂**:accept 错误簇 1715-1752(stream.c
     只以 status 0 回调、EAGAIN/EMFILE 内部消化)、uv_loop_init
     失败 4129(仅系统级资源耗尽)、luaL_newmetatable 二次调用
     臂 4134-4196(单 state 进程恒首建)、alloc_buf 的
     suggested==0 臂 1502-1503/3702(内核不回 0)、470/3716/
     3721(proc 管道半包汇合的内部分派);
  2. **平台恒真臂**:2404-2526(os 簇 home/temp/hostname/
     uname/uptime/cpu/接口失败臂,Linux 上不失败,第十一轮
     表征维持);
  3. **小额分配 OOM knife-edge**:2654(tsock_new calloc)、
     2683(SSL_new)、3042/3478(SSL_CTX_new)、3439(tserver
     calloc)、3892/3977(proc 结构)——t_lowermem 的 128MiB
     头寸只对 ≥数百 MiB 的目标分配确定失效,KB 级目标即退回
     第八轮判定过的 knife-edge;malloc 注入基建维持不投入;
  4. **lit 行的守卫子臂**:`malloc(len ? len : 1)` 的 len==0 臂
     726/1502/3167/3190(0 长 payload 走 malloc(1),不会失败)、
     `len ? len : 1` 三元分派本身、||/&& 的另一半(954/1096/
     2007/2072 等closed||NOREF 守卫已驱一半);
  5. **异步孪生的同步臂**:fswatch 内核错误 958/964/987、udp
     recv 错误 2046、uv_udp_send 失败 2173、dns 内部 2281/
     2324/2333/2367/2376、mid-connect close 孪生 1264/1288/
     1480、SSL 硬错误孪生 2885-2887/3180-3182(对端握手后
     fatal alert,tsock API 不可发)、同步 connect 双臂
     3096-3137(loopback 恒 EINPROGRESS,第十一轮)、SO_ERROR
     查询失败 fallback 2944-2958(活 fd 不失败)、
     getpeername 失败 1640/3257;
  6. **TLS 内部时序支路**:2749(uv_is_closing 防御)、2752/
     2764/2783/2821/2832/2869/2894/2908/2922/2934/2952/2991/
     3000(pump 状态机的 WANT 孪生与 eof/close 交叠)、
     3212(read-after-eof)、3226(双 close(cb) 重钉)、3288
     (tostring handshake 臂)、3305/3314(ref/unref closed
     臂——close 后无事件再武装,防御)、3334-3335(__gc 的
     testudata 与 selfref 在握臂——注册表钉盒,终结化不可达,
     第十二轮设计不变式的防御性补充);
  7. **proc 交付门时序孪生**:3758/3761(delivered/exit_seen/
     pipes_closed 的到达顺序组合)、3816(running 臂——run()
     模式出口只在进程死后)、3836(uv_process_kill 失败——
     活进程对 SIGTERM 不失败)、3852(无 args 调用形状);
  8. **环境依赖**:2309-2310(dns 多地址循环——本机 localhost
     单 AF,不可确定性驱动)。
- 环境披露:本轮零产品代码改动(纯测试 + todo,git diff 核对
  无插桩残留);门禁首跑 build 树 loop 一败——与 ASan 轮并行
  导致 /tmp/luna-loop-sparse.bin 共享夹具竞态(ASan 进程中途
  unlink,ENOENT 冒充 ENOMEM;第十一轮 ASan 日志互串同类),
  串行复跑全绿;两树 ctest 均按 --timeout 900 运行参数,库
  文件未动;ASan 按既有加预算做法(720s)复跑。

## replxx 行编辑覆盖复核(2026-09-28 第十五轮)

- 前提勘误:第一轮注记「`luna_line.c` 2%,记入下轮方向不在
  本轮」已过时——第二轮即重写 line 组(真二进制 pty)并新增
  linedit 白盒组,「luacov 接线」同步就位(LUNA_COVERAGE=1
  导出,真二进制 gcda 落盘);白盒双计被 sweep 后的诚实账自
  第七/八轮起即为 97.14%。本轮按派发接手,实测为基线+增量。
- 基线实测(第十四轮全绿 build-cov gcda):行 97.14%(136/140)、
  分支执行 100%、方向命中 78.57%(55/70);4 条暗行全为防御臂。
- 新增 7 例(23→30,pty 驱动真二进制,断言全落在行的求值结果
  上):光标左右移+行中插入、^U/^K(行首杀/行尾杀)、^W+DELETE
  键(词杀/前向删)、历史 Down 走回空行、^L 重绘保留草稿、
  补全 span 拒收(-1 与 9999 双向,点亮 129 暗方向)、非字符串
  候选跳过({false,'77'},点亮 136 暗方向)。
- 实测发现:非法 UTF-8 字节经 pty 不可注入——0xFF 被输入层
  丢弃(s:byte(2)=121 为证),164 宽度回退臂属平台性不可达,
  不为行数写假测试;另 print() 输出绕过 Out[] 前缀,needle
  断言须落在 print 文本上(开发中自纠)。
- 残余 4 暗行逐腿登记(行覆盖诚实上限即 97.14%):54(唤醒线程
  循环出口——read EOF 仅进程死亡,白盒经管道关闭驱动)、
  62(双启动守卫——ensure_rx 单调用点一次性,构造不可达)、
  64(pipe() 失败——白盒 RLIMIT_NOFILE 注入直接覆盖
  (luna_test_linedit.c:173);二进制内注入动摇整个启动,刀口
  类,维持第八轮判定)、176(高亮尾填——replxx 尺寸一致性
  防御,walker 终态 cp==size)。
- 披露:54/62/64 腿在白盒套件有直接测试,唯 sweep 排除双计,
  诚实账不可见——「已测未计」而非「未测」。
- 门禁:build 树全量 ctest 15/15 绿(loop 191.4s,含并行会话
  6 个 loop OOM 测试的 199 用例态,该 6 测试经用户指示随本轮
  单提交落库);cov 树全量 14/15+rocks 重试绿 228.1s(首跑
  test_install_lock_reproduce_cycle 的 GitHub tarball 下载腿
  败一次,known flake 类,负载 31 时重试即绿);复测
  `luna_line.c`:行 97.14%(136/140,持平——上限即 4 暗行)、
  分支执行 100%、方向命中 82.86%(58/70,+3)、调用 100%
  (75/75)。
- 环境披露:负载风暴(47-107)贯穿本轮,门禁按既定做法等负载
  窗口(<40)再跑;库文件未动,仅 test/luna_test_line.c 与
  todo.md,负控/插桩零残留。


## C 侧覆盖扫描(2026-09-28 第十六轮)

- sweep 发现并修复:gcovr_summary 的 `-r` 原为源码根,会把兄弟
  build 树(其 build/ 也带全套插桩产物)的 gcda 一并扫入——
  build/test 的白盒 luna_main 副本(陈旧 198 行视图,main() 全
  暗)把诚实账拖到 64%(128/198)、TOTAL 93.4%;改 `-r
  BINARY_DIR` 后恢复 TOTAL 95.2%(2611/2743)。已在
  CMakeLists 注明(2026-09-28 观测)。
- 干净排名(cov 树诚实账):luna_loop 95%(112 暗行,第八/
  十三轮逐臂表征维持)、**luna_main 90%(13 暗行,本轮对象)**、
  luna_line 97%(4 暗行,第十五轮登记)、luna_kernel 98%(3 暗行)。
- luna_main 13 暗行逐腿:78(package 表——openlibs 后构造不
  可达)、205-207+367(run_chunk 错误臂与 rc=1——白盒
  luna_test_main.c:37 直接驱动,已测未计)、269-272(缺
  luacov 降级臂——白盒 luna_test_main.c:65 已测;cov 树内
  LUNA_LUACOV_SRC 为编译期追加,env 不可注入)、282(shutdown
  pop——stats 不可写类刀口)、296-297(newstate NULL——OOM
  类)、371(rc=0 臂——entry 恒返整数的不变式)。
- luna_kernel 3 暗行逐腿:203(tail_looks_incomplete 的
  len==0——空块 load 恒 OK,k_check 到不了,防御)、289(load
  错误对象非字符串——语法错误恒为字符串,OOM 类)、325(pcall
  错误对象非字符串——msghandler 对任意错误对象恒产出字符串,
  见下条实测)。
- 新增 1 例(cli 组 23→24,两树绿):敌意错误对象脚本模式端到
  端——`__tostring` 内 error(false) 不崩 runner,回退文本 +
  traceback 照常渲染,退出 1(fixture non_string_error.lua)。
  实测发现:5.5 的 message handler 在栈未展开时运行,该路径把
  callmeta 的 raise 吸收进回退文本——测试锁的是实测契约而非
  推断(初版按 5.4 直觉写"(non-string error object)"实测即纠)。
- 门禁:两树全量 ctest 15/15(build 329.9s、cov 316.1s,串行);
  覆盖率门禁 TOTAL 95.2%(2611/2743)、分支 76.6%(909/1186)。
- 环境披露:并行会话本日再落 7 个 luna_line 测试(cc09142,
  line 组 37 例态,已推送);本轮 CMakeLists 仅动 gcovr 报告
  范围,产品代码零改动;rocks 网络腿本轮未涉足。

## C 侧分支方向清扫(2026-09-28 第十七轮)

- 前提:Lua 侧已到登记上限(97.82%,四文件暗腿全在"明确不
  做"),本轮转 C 侧从未系统扫过的**分支方向**(taken 0%):
  luna_main 62%(19 暗/50)、luna_kernel 83.33%(16 暗/96)。
- kernel 16 条逐腿处置,10 条落真测试、6 条登记:
  - chmod 模式解析三暗臂(尾部垃圾/负数/超 07777):一个 cli
    测试三 pcall 全锁(实测 `bad mode "..."` 文本);
  - check:修剪臂 Tab/CR(空格臂先期已亮)+ 未闭合长括号,
    聚合断言四个 incomplete;243 第二 strstr("unfinished
    long")按消息形状重叠登记——实测 5.5 该消息恒带
    `<eof>`(`unfinished long string (starting at line 1)
    near <eof>`),241 先截,该臂为死防御;
  - exec:name 槽与脚本参数(实测 `true 2 a b`)+ 无 name 单
    参形态(全套件从未用过,实测 `true 5`);
  - msghandler __tostring 两暗臂与第十六轮"会炸的
    __tostring"拼成三形态契约:**返回字符串**走早路径,消息
    直出且**无 traceback**(fixture tostring_message.lua);
    **返回非字符串**(42)落回退,`(error object is not a
    string)`+traceback(tostring_returns_non_string.lua);
  - colors:LUNA_COLOR 空值非强制(env 测试,落到 TTY 判定);
    TERM=dumb 与 TERM 未设两臂需 isatty(1)==true 才短路到,
    只有 pty 可达——harness 加 spawn_repl_term(TERM 参数化,
    NULL 即 unsetenv),dumb 断言 SGR 缺席、未设断言 SGR 在
    场(锁"未设回退 TTY 能力"语义);
  - 登记 6 条:200 branch1+202(修剪到空——全空白块 load 恒
    OK,构造不可达)、236+288(load 失败无消息——OOM 类)、
    324(运行时非字符串错误对象——第十六轮实测 5.5 handler
    吸收,OOM 类)、243 branch2(消息形状重叠,见上)。
- main 19 条暗方向逐腿核对:全部落在第十六轮已登记暗行的同
  族区域(78/93/203-204/250/257/268/281/295/349/364/368——
  构造保证/白盒已测未计/OOM 类),无新增可注入腿;分支 taken
  维持 62%(31/50),登记升级为行+方向双层口径。
- 账面:kernel 分支 taken 83.33% → **93.75%**(90/96);聚合
  分支 76.6% → **77.5%**(919/1186);行账 95.2% 不动(零产品
  代码改动,符合预期)。测试账:cli 23→29、line 37→39。
- 门禁:两树全量 15/15 串行绿(build 先行,cov 随后;负载峰
  值 59,rocks 组未假挂);覆盖率门禁如上。
- 方法披露:分支编号不直观处(gcda 的 branch N 与源码子条件
  的对应)一律以行命中数交叉定案——如 msghandler 早路径先判
  为未亮,264 行 23 次命中证伪,残暗实为另一子条件。


## 下轮方向

- tsock 结构体泄漏:**已完成(第十二轮,见上)**。
- luna_loop 分支覆盖专项:**已完成(第十三轮,见上;余量全为
  逐臂表征)**。
- malloc 注入基建维持第八轮判定(仅在真实回归疑点时再评估;
  第十三轮确认 t_lowermem 窗口已覆盖其对大分配的全部可达面)。
- 覆盖率专项收官:C/Lua 两侧余量全为逐腿/逐臂/逐方向表征
  (第十七轮后含 C 分支方向层:luna_main 62%、luna_kernel
  93.75% 的残暗方向全数登记),转入按需修补。

## 明确不做(上一轮)

- rocks.lua 剩 20 行——ensure_dir 递归(64)、openssl 后端缺席
  (79)、缺失 rockspec 哈希(83)、luarocks 内部 bug 腿(199)、
  多版本排序(253)、缓存抓取腿(309-311/320-321)、362-363、
  网络腿(409-410/450/454-455)、需真实 .src.rock 的装后腿
  (421-423):离线不可注入或依赖网络时序,均不为行数写测试;
- luna.lua 剩 6 行(24/158-159/181/221-222,见上)、introspect
  241(pairs 顺序)、http.lua 剩 13 行(第九轮逐腿表征)——维持;
- luna_loop.c 剩余 193 行(第八轮逐函数表征)——维持;
- 未闭合引号跨行续行的语义修补(维持前轮判定);
- 打 tag / 发版(硬约束禁止)。
- 巡检注记(第十四轮,2026-09-28,基线 59b7db4):全量 15 组复跑——build 树 1026.7s(loop 322.9s 过新 600s 门,ASan 树同套 193 用例 578s 零泄漏)、cov 树 589s 15/15 绿;覆盖率门禁 lines 95.2%(2610/2743)、luna_loop 分支 76.19%(739/970)、聚合 76.3%(905/1186);本轮唯一修复:loop 组 TIMEOUT 属性 240→600(显式属性压过 ctest --timeout,59b7db4);rocks 组负载 87-107 三连败(2×Timeout+1×Failed,内层 timeout 420 掐网络重锁腿,负载假挂类),负载 38 复跑绿 253.6s,同轮 cov 树曾绿证非回归;工作树含并行会话 +76 行(test/luna_test_loop.c,6 测试,199 用例态同轮全绿)未纳入提交,gcov 临时产物已清。
