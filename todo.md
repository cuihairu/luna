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

## 行编辑模块分支方向清扫(2026-09-28 第十八轮)

- 前提:main(19)/kernel(6)/loop(231) 的分支方向已全数登记
  (第十七轮与八/十三轮),四模块中从未做过方向层的只剩
  luna_line——70 条分支,taken 82.86%(12 暗),本轮批次。
- 12 条逐腿处置,1 条可达落测试、11 条登记:
  - 可达:补全候选 1000 截断上限(L133)——pty 测试造 1002
    个 zzq* 全局,Tab 后 replxx 分页器直接报 "Display all
    1000 possibilities",上限先于候选数生效,可直接断言;
    答 n 后 ^C 弃稿,6*7 证会话无恙。
  - 登记 11 条:47(唤醒循环退出——read EOF 仅进程死亡,第十
    五轮同族)、49(rx NULL——g_rx 初始化先于唤醒可观察,构造
    不可达)、61(双初始化守卫——单次 init)、63(pipe 失败——
    第十五轮白盒 RLIMIT 刀口同族)、67(pthread_create 失败
    ——资源耗尽类)、112+150 branch1(回调先于 ref 注册——
    回调仅在 Tab 触发,注册先于用户输入,防御)、150
    branch4(size<=0——replxx 契约:空行不调高亮)、160
    branch3+175(replxx size 一致性族,第十五轮 176 同族)、
    164 branch1(非法前导字节宽度 1 回退——replxx 永不喂非
    法 UTF-8,第十五轮平台结论同族)。
- 实测纠偏三处(测试写法全按实测落):
  - astral 往返:4 字节字符(U+1D11E)经 pty→编辑器→VM 完整
    往返,#s==4 直证;但 164 的 4 字节真边早在 cc09142 白盒
    test_four_byte_utf8_in_highlighter 里已亮——该测试定为端
    到端行为锁,不冒充新方向;
  - ^C 后 replxx 重挂 raw mode,提示符先印而随后字节会被
    tcsetattr 冲掉——提示符出现后安定 250ms 再打字;且 ^C 与
    新提示符常同块到达,两者之间不得设 mark(会把提示符切出
    窗外,空窗口超时);
  - 输入计数:求值输入才推进 In[N],^C 弃稿不推进——cap 测
    试收尾提示符是 In[3] 不是 In[2](窗口里重绘帧叠加的重复
    字符是剥 ESC 的正常产物,非双重输入)。
- 账面:luna_line taken 82.86% → **84.29%**(59/70);聚合分支
  77.5% → **77.6%**(920/1186);行账 95.2% 不动(零产品代码
  改动,符合预期)。测试账:line 39→41。
- 门禁:两树全量 15/15 串行绿(负载峰值 49,rocks 组未假挂);
  覆盖率门禁如上。
- 至此四个 C 模块的分支方向层全部表征完毕:可达的都落了真
  测试,余量逐腿登记,C 侧再无未扫层。


## 非 C 侧巡检(2026-09-28 第十九轮)

- 快照:C 行账 95.2%(2611/2743)、聚合分支 77.6%(920/1186)、
  Lua 97.82%(1799/1839)——C 方向层收官后首轮全面重测,与
  第十七/十八轮持平。
- 非 C 一方代码盘点(绑定/宿主/工具链):
  - src/luna.c 与 src/luna_hook.c:各 1 行占位编译单元,无
    可执行代码——gcovr 报告缺席是应当的(报告 4 文件行数恰
    为 2743),登记即闭,非缺口;
  - test/replxx_completions_shim.cxx(43 行 C++ 垫片):4 个
    入口(new/free/count/at)全被 linedit 白盒组驱动——测它
    就是测测试基建,不硬凑;
  - cmake/*.cmake:声明式构建逻辑,无测试惯例,维持;
  - lua/ 层:97.82%,四文件暗腿全在"明确不做"(第十七轮
    复核维持)。
- 缺口 top1:**cmake/lua_cov_summary.lua**(21 行)——Lua 覆
  盖率汇总器,产出构建日志里众人读的汇总表,此前零测试;
  解析坏了是静默空输出而构建照样绿(典型哑账风险)。
- 新增 covsum 组(4 例,cli 同款 popen 过真二进制 + 合成报
  表夹具,15/15→16/16):表体回显契约(表头+文件行+Total
  入账,正文段源码行与 ===== 规则线不外泄)、Total 即终点
  (恰一条规则线入账,收尾规则线止步)、报表缺席响亮失败
  (exit≠0 + cannot open)、缺省路径腿(无参按缺省
  luacov.report.out 在子进程 cwd 解析,同样响亮)。组入
  Profiling 树 LUNA_COVERAGE=1 名单。夹具命名披露:fixtures
  目录忽略 *.out(防产物入库),报表夹具用 .fixture 后缀。
- 登记余量:汇总器 21 行不计入 luacov 账(config include 仅
  覆盖 lua/ 与 staged 模块层;扩配置=改账本分母,不做),行
  为由 covsum 组锁定;luna.c/luna_hook.c 占位单元与 shim 如
  上。
- 门禁:两树全量 16/16 串行绿(负载峰值 ~30,rocks 未假挂);
  覆盖率门禁双侧如上;零残留。
- 至此非 C 一方代码(绑定/宿主/工具链侧)全部盘点并定性:
  可测的落了真测试,不可测/不值测的逐项登记理由。


## Node 方向立项:格式标准库 + Node 底层能力(2026-09-29 第二十轮,规划落盘)

- 本轮只落规划不写实现(派发单两批拆分的第一批):选型详单、API 草案、
  错误口径、批次拆解与验收标准全部落进 **docs/node-parity.md**(新设计
  文档,与 loop-backend-design 同体例),本节记立项决定与验收账。实现从
  批次 2 起逐批做,每批单提交、两树全绿后才落下一批。
- **现状盘点(源码核实,非推断)**:派发单 B 面所述「事件循环推迟」已
  演进——`require "loop"` 第一批早已落地(luna_loop.c 4234 行,timer/fs
  (+watch)/net(+TLS)/udp/dns/signal/os/process 八个 C 面 + loop.http 纯
  Lua 面,libuv 1.53.0 静态链);**timers/child_process/os 三项已在**,
  真缺口是 path/util.inspect/events/stream 四项 + timers 的全局化/脚本
  自动排水决策。README 概览表「事件循环推迟的 rationale」指向上文,
  表述滞后但指向的 architecture.md 本尊是现行版(opt-in 第一批)。
- **决策记录**(详单与理由见 node-parity.md,此处只列结论):
  1. **tomlc99 → tomlc17**:派发单指定的 tomlc99 上游已挂 OBSOLETE 横幅,
     改推同作者继任者 tomlc17(MIT、TOML v1.1 过官方 toml-test、双文件
     amalgamation、零依赖);C API 同族,绑定层可平移;c17 编译受阻时
     回退钉死的 tomlc99 vendor。
  2. **timers:启用「脚本尾部自动排水」,继续推迟全局化与 REPL 集成**。
     run_script / -e 主 chunk 正常结束后若 loop 有活句柄自动 run()
     (Node 的「事件循环跑到空退出」契约,unref 语义不变);全局
     setTimeout 与 REPL 集成维持推迟(行编辑阻塞读、^C/130、attach
     轮询点三张同步契约不动)。REPL 每求值后 drain(nowait) 记入下轮
     方向的条件项。
  3. **不遮蔽 Lua 全局 os**:Node os 的补齐进 loop.os(arch/release/EOL/
     userInfo/availableParallelism),顶层 `os` 是官方标准库的地盘。
  4. **新格式不加 parse/stringify 别名**:别名是 json 的历史包袱,新格式
     decode/encode 唯一入口;错误口径统一 `nil, err`(不抛错),
     err 带行列(`<fmt>: <原因> at line N, column M`),参数类型错才 raise。
  5. **csv/ini 走 LPeg 不走 C**:deps 已有 lpeg(已注册),行导向格式
     引 C 库方向反了;「绑定成熟 C 库」针对规范重的 xml/yaml/toml。
  6. **绑定层不用 sol2,维持手写 C luaopen 直绑官方 C API**(2026-09-30
     评估,选型对比入 architecture.md「绑定层」节):sol2 最新发布
     v3.3.0 不支持 Lua 5.5——支持停在未合并 PR(ThePhD/sol2#1723,
     2025-07 起),维护者零回应、下游注 increasingly unmaintained;
     接入即 vendor 未合并补丁集 + C++17 模板库进「薄 C 内核」。
     「直接集成官方 Lua 5.5、不经绑定框架」本就是现状:deps/lua 钉
     **v5.5.1 = 上游最新 5.5 发布 tag**(master 领先 6 个小修,均未进
     发布,锁版本纪律等 5.5.2),五族绑定(lfs/luasocket/lua-zlib/
     luaossl/loop)全走官方 C API,批次 3–5 的格式绑定沿用同模式。

### 批次拆解与验收标准

- [x] **批次 2:csv + ini(LPeg,零新依赖)**。LPeg 语法 + `lua/modules/
      {csv,ini}/init.lua` 包装(现有 LUNA_STDLIB_WRAPPERS glob 自动 staged);
      offset→line/col 公共辅助落在包装层。用例进 modules 组:RFC 4180 边界
      (引号内分隔符/换行/双引号、CRLF/LF 双收、headers 表键行、delimiter、
      坏输入行列)、ini(段/段前裸键/两种注释/引号值/重复键/cast/坏输入)。
      **验收**:decode/encode 全口径用例绿;两树 cmake --build + ctest 全绿;
      guide/modules.md 内置模块表更新;luacov 账收编新文件。(实录见下节)
- [x] **批次 3:toml(tomlc17 vendor)**。deps/tomlc17 子模块钉版本 + 静态
      库;`toml.core` 进 register_c_modules;包装层 decode 直通、**encode
      为自写 Lua 面**(tomlc17 无 encoder,键序"标量在前"由包装层重排);
      datetime → 表映射({year,…,secfrac?,offset?})。用例:toml-test 摘选
      代表性 valid/invalid(多行字符串、Unicode 转义、整数边界、日期时间)、
      encode 往返、错误行列。**验收**:同批次 2 + .gitmodules 登记与版本
      钉死;构建零新增系统依赖(vendor 自带源码编译)。(实录见下节)
- [x] **批次 4:yaml(libyaml + lyaml vendor)**。libyaml 上游 CMake 接线
      (EXCLUDE_FROM_ALL,参考 libuv 段);lyaml 绑定源编入静态库;null→nil
      默认 + opts.nullval 哨兵;anchors/aliases → 共享表引用,encode 循环
      报错;decodeAll(多文档)批内定去留。用例:标量类型家族、块标量、
      流式集合、多文档、坏输入行列、encode 循环。**验收**:同上;Lua 5.5
      适配若需小补丁,vendor 内登记改动点。(实录见下节)
- [x] **批次 5:xml(expat + lua-expat vendor)**。DOM 三件套 {tag, attrs,
      kids}、文本节点为字符串、属性恒字符串、命名空间前缀原样;encode
      转义 + opts.indent;xml.sax 透传 lxp handler(流式面)。用例:实体/
      CDATA/嵌套/属性、坏输入行列(mismatched tag 等)、encode 往返、SAX
      增量喂。**验收**:同上。(实录见下节)
- [x] **批次 6:path + util + events(纯 Lua,零依赖)**。path 按 Node
      path.posix 逐函数对照钉死(resolve/normalize/join/relative/parse/
      format 的 Node 边界语义各配用例);util.inspect(depth/循环 [Circular]/
      截断/键引号规则)+ util.format(%s %d %f %x %X %o %j %%,无符连接,
      超参尾接);events 全 API(on/once/off/prepend/listeners/
      listenerCount/setMaxListeners/缺省 10 警告/'error' 无监听 raise/
      newListener 内建事件)。**验收**:三模块用例绿;两树全绿;文档
      (guide/modules.md 表 + node-parity.md 状态勾稽)。(实录见下节)
- [x] **批次 7:stream(基于 events,纯 Lua)**。Readable/Writable/Duplex/
      Transform + pipe 背压(write false 停推等 drain 续推/unpipe)、
      highWaterMark 记账阈值;适配器至少两个:sock(loop.net)→Duplex、
      内存块→Readable(loop.http onData→Readable 视余量)。用例:背压
      往返、error→destroy→'close' 联动、pipe 错误传播。**验收**:同上。
- [x] **批次 8:os 补齐 + child_process 糖 + timers 自动排水**。loop.os
      增 arch(x86_64→x64 映射)/release/EOL/userInfo/availableParallelism
      (loop.c os_funcs 表内 ~60 行);process.exec = run("sh",{"-c",…})
      糖、execSync = io.popen 糖(close 三元组拿退出码,非零 raise);
      **脚本尾部自动排水**(run_script 与 -e 尾部,loop 出
      luna_loop_maybe_drain(),REPL 不启用)。**验收**:既有全部 loop
      用例原样全绿(自动排水对自调 run() 的用例必须是无操作);新增:
      脚本 setTimeout 不调 run 也触发、全 unref 立即退出、-e 同口径;
      ^C/130 与 attach 契约用例不回归。(实录见下节)
- 每批共同门禁:单提交;两树 cmake --build + ctest 全绿(本立项日实为
  **16 组**——README 旧文「九组」清单已顺手修正,历史轮次记录里的
  九组/15 组是当时的账,不改);负载 >40 不取 rocks 终绿的纪律照旧;
  不打 tag、不发版。

### 本轮(批次 1)产出与门禁

- [x] docs/node-parity.md 选型文档(五格式候选表与淘汰理由、API 草案、
      错误口径、Node/Lua 生态对照;B 面逐项盘点表、timers 现状核实与
      建议、path/util/events/stream/os/child_process 的层归属与草案);
      侧栏挂「设计」组;README 测试行修正(九组→十六组,清单同步)。
- [x] 门禁:ctest 16 组全绿(普通树);文档构建通过(vitepress build);
      git fetch --rebase origin main 后单提交推送;零实现代码改动
      (本轮只动 todo.md、docs/、README 一行)。

### 批次 2(csv + ini)实录(2026-09-30)

- [x] `lua/modules/{csv,ini}/init.lua`(LPeg,零新依赖;现有
      LUNA_STDLIB_WRAPPERS glob 自动 staged)。csv:RFC 4180 全边界
      (引号内分隔符/换行/双写引号、CRLF/LF/混合、headers 表键行、
      任意单字节 delimiter、空记录/尾分隔符无幻影字段)、encode
      按需引用、`lines()` 逐记录迭代器(引号内换行留在字段里)。
      ini:段/段前裸键/`#` 与 `;` 双注释/引号值 `\\` `\"` 转义/段合并
      重复键 last-win/`cast` 选项;错误统一 `<fmt>: <原因> at line N,
      column M`(列按 UTF-8 字符计),offset→line/col 公共辅助在包装层。
- [x] modules 组 +11 用例(26→37,`test/luna_test_modules.c` +356 行):
      decode/encode 全口径 + 坏输入行列断言(含 UTF-8 列计数)。
- [x] **门禁翻出的两个真产品 bug,均根因修复**:
      1. 覆盖率合并钩子下 `^C` 与 attach 轮询可确定性饿死——luacov 逐行
         记账在钩子帧内执行的几百条指令同样消耗 count 预算,count 归零
         落在 `allowhook=0` 的钩子帧内时被静默重置,事件永不浮出到用户
         代码;紧凑循环的每迭代指令预算使该相位确定成立(空循环必饿死,
         非空循环凭相位运气)。修复:合并钩子对行事件与 count 事件一律转发
         一步 `kernel.count_hook()`(行事件每迭代必发,ldebug.c 跳回即调
         行钩子,不依赖指令预算落点),luna_kernel.k_count_hook 注释
         留全机理;初版专设的 `interrupt_pending` 探针随之撤销。
         现象账:cli 组 `test_eval_interrupt_exits_130` 挂死(空循环,
         3/3 复现);serve 组 `test_attach_pty_completes_and_leaves_
         via_exit_magic` 远端补全超时(TAB 补全请求发到 socket 但目标
         忙循环的 serve 轮询饿死,wire 只剩 `attach> z`)。修复后 serve
         组 5/5 稳定、cli 组 3.5s 绿,普通(非覆盖率)路径行为不变。
      2. introspect 的 `quote_string` 漏逃 NUL 与其余控制字节——
         `%whos` 对 `_G` 行的值预览截进 `utf8.charpattern` 的字面 `\0`
         时,attach 帧 reply 在 C 侧被截断,`strstr(reply, "attachgx")`
         偶发失败(每进程随机串哈希种子 → 全局表预览键序不定 → 抖动)。
         修复:控制字节统一 `\xNN` 逃逸(introspect 测试钉住契约),
         终端显示与帧体两清。
- [x] 环境披露(门禁解读前提):build 与 build-cov 两树实为**皆
      Profiling**(ctest 对全部组导出 LUNA_COVERAGE=1),故上述饿死
      路径在两树全量门禁下都会走到;本轮另有并行会话,负载 9–14。
- [x] 覆盖率账:luacov include 收编 `luna_modules/{csv,ini}/`;
      csv 96.06%(122 行 5 暗)、ini 90.85%(129 行 13 暗)、Lua 侧
      总 97.25%;C 侧 lines 95.2%(2611/2743)维持。
- [x] 文档:guide/modules.md 内置模块表加 csv/ini 行 + 错误口径段
      (与 json 同契);README/architecture/index 三处标准库清单同步;
      getting-started.md 合并 hook 段改写(行/count 双事件转发)。
- [x] 门禁:两树 cmake --build + ctest 16 组全绿(build 355.0s、
      build-cov 346.0s,串行);单笔提交,push 前 fetch origin main
      核对 SHA;不打 tag、不发版。

### 批次 3(toml)实录(2026-09-30)

- [x] **vendor 与钉版**:deps/tomlc17 子模块按 **R260821 release tag**
      (e0e8868)钉死,.gitmodules 登记;双文件 amalgamation 直接进静态库
      (`add_library(tomlc17 STATIC deps/tomlc17/src/tomlc17.c)`),
      C17 单翻译单元、零外部依赖,构建零新增系统依赖。上游 master 领先
      7 个提交(超长数字字面量拒绝、闰秒 :60 拒绝、subnormal float 等
      修正),**明知不取**:锁版本纪律按 release tag 钉(与 deps/lua
      v5.5.1 同法),且已核实 tag 上 int64 溢出字面量同样报行号拒绝
      (用例钉住),上游修正等下一个 release 再评估。
      接线坑:cmake/FetchSubmodules.cmake 每次 configure 都跑
      `git submodule update --init`,把工作树拉回**索引**里的 SHA——
      先 `git add deps/tomlc17` 落索引再 configure,否则重钉会被
      静默还原(本轮 12:34 复现一次才定位)。
- [x] **decode 绑定(src/luna_toml.c,luaopen_toml_core)**:
      `toml_parse(src,len)` → 递归转 Lua 值后立即 `toml_free`,无 C 侧
      所有权外泄;深度守卫 100(方括号/花括号嵌套 tomlc17 自 capped 30,
      链式表头不受其约束)。datetime → 组件表:DATE {year,month,day}、
      TIME {hour,minute,second,secfrac?}、DATETIME 两者、DATETIMETZ 再加
      offset(分钟,Z 为 0);secfrac 是秒的小数且仅当源有小数位
      (tomlc17 只存整微秒,"07:32:00.0" 与 "07:32:00" 解码等价——按
      微秒粒度等价处理,已核实接受)。`luaopen` 打开 check_utf8
      (tomlc17 默认关,TOML 1.0 要求合法 UTF-8;进程级全局,单 VM/进程
      无碍)。错误口径:**三种错误串形**(SETERROR 的 "(line N) <原因>"、
      UTF-8 检查的 "<原因> on line N"、内部兜底 "Error near line N")
      统一规整为 `toml: <原因> at line N`,**无列子句**(tomlc17 只报
      行号,datum 虽带列、错误串不带)——与 csv/ini 的行列口径就这一点
      显式分叉,guide/modules.md 与 node-parity.md 均已记账。陷阱实录:
      `lua_pushfstring` 不支持 `%.*s`(精度符原样拷贝、不消费参数,
      后续 %d 吃错位)——先 snprintf 截再 `%s`。
- [x] **encode 自写 Lua 面(lua/modules/toml/init.lua)**:decode 直通
      `core.decode`;encode 约束"标量键在前、表键在后"(TOML 硬规则,
      分区后先发标量再发子表,不依赖调用方)、每表头前空行、无尾换行;
      纯表数组 → `[[header]]` 逐元素,混合/标量数组、datetime、空表
      内联(`[1, "a b"]`、`07:32:00.5`、`{}`);裸键 `^[A-Za-z0-9_-]+$`
      否则加引号;控制字节 `\uXXXX` 大写十六进制;float 取
      {%.14g,%.15g,%.16g,%.17g} 中**读回等值的最短拼写**(Lua tostring
      的 %.14g 会丢往返),无 `.`/`e` 补 ".0";nan/inf/-inf 字面量;
      datetime 组件表识别口径:键集 ⊆ 组件集且带 year 或 hour 才认,
      普通数据表带任一其它键即避开(逃生门写进 node-parity.md);
      环引用检测(ancestor 集,全退出路径清账)。**测试翻出的真 bug**:
      子表 keypath 拼装初版写 `{ table.unpack(path), fk }`——构造器中
      非末位的函数调用被调整为**单值**,`table.unpack({})` 调成裸 nil,
      子路径成 {nil, fk} 且 # 塌到 0,二级以下嵌套表头被拍平([sub.deep]
      变 [deep]);换显式拷贝辅助 child_path,深度 3 形状用例钉死。
- [x] modules 组 +8 用例(37→45):decode 标量全拼写(两种 int64 界、
      hex/oct/bin/下划线分组、inf/nan/-0.0/3.0、字面串与转义串)、多行
      基本串(首换行裁剪、行尾反斜杠吞白)与多行字面串、\u/\U 转义
      落真 UTF-8、[表]/[[表数组]]/点键/内联表/嵌套数组、四形态 datetime
      与 secfrac/offset 映射、int64 溢出拒绝、非法 UTF-8 与代理对转义、
      duplicate key/unterminated/ENDL 行号;encode 精确形状钉(分区、
      [[aot]]、内联、带引号键、\u0000、深度 3 表头路径)、全量往返
      deep-equal(含 DEL/inf/1/3/maxinteger/secfrac/offset)、错误契约
      (坏数据 nil+err:函数值、month=13、time 带 offset、环引用、非串键;
      参数类型错才 raise——**数字实参照 string.format %s 先转字符串**,
      再按坏数据走 nil, err,已用例钉住)。
- [x] 覆盖率与文档:luacov include 收编 `luna_modules/toml/`;
      guide/modules.md 内置模块表加 toml 行、错误口径段记 no-column
      分叉;README/architecture/index 三处标准库清单同步;
      node-parity.md toml 节按实现勾稽(datetime 组件键名 minute/second
      定稿、识别口径、encode 键序、错误串形已核)。
- [x] 覆盖率账:toml 90.69%(185 行 19 暗;首测 87.75%,补钉 nan/
      -inf 拼写、offset 0→Z、secfrac/offset 三错误臂后 90.69%,余暗为
      分支末 `end` 类逐臂残余);Lua 侧总 96.46%(2232/82);C 侧 lines
      94.8%(2689/2837,含新增 luna_toml.c)、branches 77.6%(948/1222)、
      functions 100%。
- [x] 门禁:两树 cmake --build + ctest **16 组全绿**——build 360.6s、
      build-cov 445.1s(串行;期间负载 18 起伏至并行会话高峰,rocks 未
      假挂);中途一次教训入库:ctest 不重建,先 `cmake --build build-cov`
      再跑,否则 cov 组跑的是旧二进制(首轮 87.75% 即此因,重建重跑后
      台账才真)。单笔提交,push 前 fetch origin main 核对 SHA;
      不打 tag、不发版。

### 批次 4(yaml)实录(2026-09-30)

- [x] **vendor 与钉版**:deps/libyaml 子模块钉 **0.2.5**(tag 核实,
      2c891fc)、deps/lyaml 钉 **v6.2.9**(cdc8a08;`git describe` 曾示
      v6.1-72-gcdc8a08 有误导性,以 `git tag --points-at` 为准),双双
      .gitmodules 登记。libyaml 上游 CMake 接线 `add_subdirectory(
      EXCLUDE_FROM_ALL)`:必须先 `set(BUILD_TESTING OFF)` 再进——其
      CMakeLists 的 `include(CTest)` 会把 11 个 C 测试挂进我们的树
      (CMP0077 NEW 下普通变量即够,ctest -N 仍 16 组核实);其
      `cmake_minimum_required(3.0)` 靠 libuv 段已设的
      CMAKE_POLICY_VERSION_MINIMUM 3.5 放行,**yaml 段必须排在其后**。
      lyaml 取绑定源(yaml.c/emitter.c/parser.c/scanner.c 四翻译单元)
      编入 `luna_yaml` 静态库,luke 构建系统的 `VERSION` 宏由 CMake
      `VERSION="6.2.9"` 定义(无它则 MYVERSION 拼不成立即可见)。
      submodule add 陷阱再犯一次实录:`git submodule add` 落索引的是
      **分支 HEAD** 而非检出 tag,FetchSubmodules 又按索引还原——
      checkout tag 后须显式 `git add deps/libyaml deps/lyaml` 再
      configure(批次 3 同坑,这次先验索引后配置,零返工)。
- [x] **Lua 5.5 适配:零补丁**(好于派发单预期)。lyaml.h 自带的
      5.2–5.4 垫片在 5.5 不触发,而 luna 随附 Lua 的 luaconf.h 仍保留
      `lua_strlen`/`lua_objlen` 函数式兼容宏(365–367 行),四个 C 翻译
      单元原样过。**反面教训**:初版想用命令行 `-Dlua_objlen=lua_rawlen`
      对象式宏补缺,与 luaconf.h 的函数式宏重定义告警 ×8——垫片本就
      不需要,删除后零告警。vendor 内**零改动**,"小补丁登记"一条
      因此空置。
- [x] **注册与包装层**:`yaml.core`(luaopen_yaml)进 register_c_modules
      (luna_main.c mods[]);`lua/modules/yaml/` 四文件 staged(lyaml
      lib 源拷贝:init.lua 公共接口尾换成 luna 面,explicit/implicit 的
      require 改 `yaml.*`,functional.lua 原样)。包装层契约:decode/
      decodeAll/encode + `null` 哨兵 + `_VERSION`;null→nil 默认、
      opts.nullval 替换**只动值不动键**(静默删条目比留哨兵更糟)、
      数组 null 同落 nil(尾部收缩)、null 文档在 decodeAll 是 nil 槽;
      替换遍历带访问集(共享锚点与自引用表不重复走、不成环),且
      **原地替换保共享**;encode 环检测走祖先集(共享兄弟引用各自完整
      序列化,不误报)。decodeAll **保留**(node-parity 表记"已实现"):
      同一 pcall(load) 逐文档 substitute,opts 透传。
- [x] **错误口径**:lyaml 的 Lua 层把 C 消息 gsub 掉 ` at document:`
      起的一切(含 libyaml 自己的 problem_mark),坐标实为**最后一个
      成功解析事件**的 1 基起始 mark,列按 UTF-8 字符计——常不在出错
      行,实测钉住(`a: 1\n  b: 2` 报 line 1, column 4;UTF-8 列用例
      `ké: v` → column 5);未定义别名恰在别名处。包装层 fail() 把
      `N:M: 原因` 规整为 `yaml: 原因 at line N, column M`,带
      file:line 前缀的库错误只留原因。guide/modules.md 错误口径段
      记此分叉。
- [x] modules 组 +15 用例(45→60,**门禁首轮全绿零返工**——每条
      断言先经三轮探针对真二进制钉值,含 flow 上下文里的显式标签与
      冒号标量):标量家族(bool 六拼写/quoted str/hex/inf/nan)、
      **YAML 1.1 数值家族**(二进制 0b、前导 0 八进制、六十进制
      sexagesimal/sexfloat、`_` 分组、各负号臂)、块标量 |/>|-、流式
      集合、多文档(decode 取首文档、decodeAll 空流/nil 槽/opts 透传/
      坏输入 fail 腿)、坏输入行列 4 精确钉 + 行号追踪 + UTF-8 列、
      null 三态、锚点共享(x==y、自引用)、**显式标签**(!!str/!!int/
      !!bool/!!float/!!null 全臂 + `!!float` maybefloat 各底座 +
      拒绝值错误腿)、**merge 键**(`<<: *d` 映射合并、`<<: [*a, *b]`
      序列合并、`!!merge` 正式拼写、两形态坏源错误)、encode 12 精确
      形状(含 `-.inf`)、富往返 deep-equal、encode 错误(函数值、环)、
      参数契约(opts 非表 raise、非串 raise)。
- [x] 覆盖率账:Lua 侧总 **96.32%**(2693/103;批次前 96.46%,首测
      落 94.53% 后按暗行补 YAML 1.1 数值/显式标签/merge 键/-.inf 四族
      回升);分文件 init 96.06%(13 暗)、implicit 94.68%(5 暗)、
      explicit 94.87%(2 暗)、functional 94.74%(1 暗)。余暗全部
      结构性、不入账为债:implicit 的 tointeger IIFE 自检分支(闭包
      引用自身 local 时恒为 nil,宿主臂永不可达)、explicit 两行是
      anyof 构造器尾项的 luacov 归属(resolver 本体已亮)、functional
      的 `__call` 臂(默认 resolver 表全为普通函数)、init 的发射侧
      anchors 机件 6 行(**opts.anchors 明确后续批次**,node-parity
      已记)+ 7 行防御不变量(STREAM_START/DOCUMENT_END 守卫、
      `opts == true` 兼容臂、非串 msg 兜底)。C 台账口径 src/(deps/ vendor 不入账,libuv/lua 同法):lines 94.8%(2689/2837)、functions 100%(249/249)、branches 77.6%(948/1222),与批次 3 持平——本轮 C 侧增量即 luna_main.c 的声明 + 注册两行,绑定源全在 deps/。
- [x] 文档同步:guide/modules.md 内置表 yaml 行 + 错误口径段记
      "last-event mark"分叉;README/architecture/index 三处标准库清单
      加 yaml;node-parity.md 候选表钉版勘定(零补丁/luaconf.h 宏/
      VERSION 由 CMake)+ API 三勘定(null 只动值、祖先集环检测、
      错误 mark 语义)+ 能力表 decodeAll 已实现;cmake/luacov.config.in
      include 收编 `luna_modules/yaml/`。
- [x] 门禁:两树 cmake --build + ctest **16 组全绿**(串行)——build
      441.87s、build-cov 459.64s;期间并行会话负载冲到 100(5 分钟均值
      83–100,超出"80+ rocks 假挂"警戒窗),rocks 两树均实绿未挂——
      结果可信,但下次冲 90+ 前值得先歇手。ctest 不重建纪律照旧——
      先重建后跑。单笔提交,push 前 fetch origin main 核对 SHA;
      不打 tag、不发版。

### 批次 5(xml)实录(2026-09-30)

- [x] **vendor 与钉版**:deps/expat 钉 **R_2_8_5**(4b3f0b0,tag 核实)、
      deps/luaexpat 钉 **1.5.2**(947d2e9),双双 .gitmodules 登记。
      expat 上游 CMake 在**嵌套目录** deps/expat/expat(顶层只是包装),
      接线 add_subdirectory(EXCLUDE_FROM_ALL):五个公开开关全灭
      (EXPAT_BUILD_TESTS/TOOLS/EXAMPLES/DOCS/PKGCONFIG)+
      EXPAT_ENABLE_INSTALL off——expat_shy_set 对预定义变量害羞,
      plain set() 即落(同 libyaml 段);cmake_minimum_required(3.17)
      靠 libuv 段的 CMAKE_POLICY_VERSION_MINIMUM 3.5 放行(段序同
      yaml)。**EXPAT_SHARED_LIBS 显式 off**:其缺省跟随
      BUILD_SHARED_LIBS,新树缓存无它时默认 ON——共享 expat 即系统
      依赖,违背"零新增系统依赖";新树 configure 核实 libexpat.a +
      ctest -N 仍 16 组。luaexpat 取绑定源 lxplib.c 编入 luna_xml
      静态库。submodule add 索引陷阱第三次实录(分支 HEAD vs tag)——
      本批先 checkout tag 再 git add 钉索引,零返工。
- [x] **Lua 5.5 适配:零补丁**。lxplib.c 通篇 5.2+ API
      (luaL_setfuncs/luaL_Buffer/luaL_checkudata/lua_setuservalue),
      连 lua_objlen/lua_strlen 都不出现——lyaml 的 luaconf.h 宏
      问题在这里根本不存在。vendor 内零改动。
- [x] **注册与包装层**:`lxp`(luaopen_lxp)裸名进 register_c_modules
      (luna_main.c mods[])——C 模块自己的名字;用户面 `xml` 是
      staged 包装层(lua/modules/xml/init.lua 单文件)。包装层契约:
      decode/encode/sax + 共同口径(坏数据 nil,err、参数类型错
      raise)。DOM 三件套 {tag, attrs, kids},文本节点是 kids 里的
      普通字符串;lxp attrs 的数字键(文档序)不进 DOM;相邻文本片段
      (实体/CDATA 边界分片)在元素边界合并——往返后 & 不双重转义;
      空白文本节点保留(标准 DOM 行为,美化 XML 往返靠它);声明/
      注释/DOCTYPE 不进 DOM。encode:属性值强制 tostring、文本
      转义 &<>、属性转义 &<>"、opts.indent=N 美化(纯文本子节点
      内联、含元素子节点逐行、混合 kid 标量独占一行)、nil attrs/
      kids 按空、标量 kid 走 tostring;环检测祖先集 → nil,
      "xml: cyclic table reference"(error level 0,消息不带
      file:line 前缀),共享兄弟不误报;坏 DOM 形状(非串 tag/非表
      attrs/kids)同走 nil, err。sax = lxp.new 裸别名(不做第二套
      抽象,handler 键校验由 lxp checkcallbacks 白送)。
- [x] **错误口径**:expat 的 XML_ErrorString + 行列经 reporterror
      (nil, errmsg, line, col, byteindex)规整为 `xml: 原因 at line N,
      column M`;**列按 UTF-8 字符计**(与 yaml 同口径,
      `<r>张三<x></r>` 报 column 11 而非字节 15);草案钉的
      `mismatched tag at line 1, column 9` 实测逐字吻合。decode
      必须 parse(s) 后再空参 parse() 收尾(lxp 的 final 标志是
      s==NULL)——未闭合标签的错误只在收尾时浮现;收尾失败的
      parser 上 close 会再抛,pcall 兜住(坏输入不 raise)。
- [x] 用例:modules 组 +9(60→69),探针先钉值再写断言。首轮 5 条
      失败**全是测试侧问题**、库只动一行:C 串里 `\n` 变 Lua 源
      原始换行致短字符串跨行(三处,改 `\\n`)、CDATA 期望值多引号、
      cycle 错误消息带 file:line 前缀(库侧改 error level 0 修掉)。
      修复后全绿。覆盖:DOM 三件套/数字键丢弃、实体+CDATA+文本合并、
      声明注释 DOCTYPE 不进 DOM、NS 前缀原样、坏输入 12 钉
      (mismatched/no element found/junk after doc/invalid token/
      undefined entity/unclosed CDATA/duplicate attribute + 多行
      行号追踪 + UTF-8 列)、encode 精确形状(紧凑/indent/自闭合/
      转义/tostring/nil 容错)、往返(紧凑+深 indent)、encode 错误
      (环/共享兄弟/坏形状/参数 raise)、SAX 增量喂(分片交付序列 +
      单 chunk 整文档)、参数契约(decode 非串/encode 非表/opts 非表
      raise、sax 坏键与无参由 lxp 报)。
- [x] 覆盖率账:Lua 侧总 **96.42%**(2801/104;批次 4 收官 96.32%,
      首测落 96.25% 后按暗行补坏形状三臂 + 混合 kid 美化臂回升,
      **超批次 4**);xml/init.lua 99.08%(108 语句 1 暗,余暗是
      fmt_err 的非数字行列兜底——lxp 报错恒带数字坐标,公开 API
      不可达,结构性)。C 台账口径 src/(deps/ vendor 不入账,libuv/
      lua 同法):lines 94.8%(2681/2829)、functions 100%(249/249)、
      branches 77.6%(948/1222),与批次 4 持平——本轮 C 侧增量即
      luna_main.c 的声明 + 注册两行,绑定源全在 deps/。
- [x] 文档同步:guide/modules.md 内置表 xml 行 + 错误口径段加 xml;
      README/architecture/index 三处标准库清单加 xml;node-parity.md
      API 草案勘定(handler 首参 self——草案漏了)+ 7 条实现勘定
      (空白文本保留/文本合并/UTF-8 列/encode 容错面/环检测/parse
      收尾语义/EXPAT_SHARED_LIBS 钉静态);cmake/luacov.config.in
      include 收编 luna_modules/xml/。
- [x] 门禁:两树 cmake --build + ctest **16 组全绿**(串行)——build
      419.71s;build-cov 410.80s 时 loop 组假挂两处(并发窗口负载高,
      OOM 注入未触发 + 证书加载异常,同"80+ 假挂"家族),负载回落后
      单跑 loop 组 294.63s **实绿**——结果可信。ctest 不重建
      纪律照旧——先重建后跑。单笔提交,push 前 fetch origin main
      核对 SHA;不打 tag、不发版。

### 批次 6(path + util + events)实录(2026-10-01)

- [x] **vendor:无**(纯 Lua 零依赖,与派发一致)。三模块落
      `lua/modules/{path,util,events}/init.lua` 单文件,经既有
      LUNA_STDLIB_WRAPPERS glob(CONFIGURE_DEPENDS)staged 到
      `luna_modules/`,零构建接线、零注册代码——新目录需重 configure
      才进构建树(file(COPY) 只在 configure 跑),本轮以 staging 与源
      逐文件 diff 核对防副本过期。
- [x] **语义钉版:Node v24.21.0 机器实证,探针先行**(四轮
      /tmp/probe_node_*.js 对真二进制钉值),推翻三处文档级假设:
      normalize/resolve **折叠**前导双斜杠(旧「保留双前导斜杠」不成立,
      normalize 与 resolve 在尾斜杠上分叉——前者保留后者剥掉);
      util.format 的 **%d/%f 不截断**(3.7→"3.7",仅 %i 向零截断,
      转换走 JS Number() 收窄,"42abc"→NaN);**%x/%X Node v24 没有**
      (原样留白不消费参数)——todo 契约点名要求,按十六进制做记档扩展。
      其余:dirname 是文本操作、extname 全点前缀规则、format 的 dir
      原样拼接与空串缺席(JS 真值)、inspect 深度塌缩 [Object]/[Array]
      与 ASCII 三点、events 的 newListener 前/removeListener 后/LIFO
      清除/emit('error') 四形态。
- [x] **实现**:path 段栈 normalize + resolve 右起拼接(cwd 经 lfs 带
      缓存、回退 $PWD→"/")+ parse/format 的 JS 真值口径,参数类型错
      raise(`path.<fn>: ...`);util 的 inspect 确定性键序(序列段
      在前,数字升序、字符串字节序)与祖先链环检测 `[Circular *N]`,
      format 走转换符表 + CONSUMING(未知符原样不消费、%% 仅格式化时
      折叠、%j 经 dkjson 失败退 inspect);events 快照分发(自移除本轮
      照常、once 内重挂下轮生效)、超限警告照抄 Node 文本写 io.stderr
      每 (emitter, 事件名) 一次、移除/setMaxListeners 后重臂、0/负
      上限不限,emit('error') 无监听一律 error(..., 0) 不带位置前缀
      (与 xml 环错误同口径)。
- [x] modules 组 +14 用例(69→83):path 5 个(normalize/join/
      isAbsolute、resolve+relative+cwd 锚定、dirname/basename/extname、
      parse/format、参数契约 7 raise)、util 5 个(转换符全谱、快路径
      与超参尾接、inspect 形状/转义/键序/函数/深度、截断与环、参数
      契约)、events 4 个(分发与快照/prepend 序/自移除/once 重挂、
      内建事件前后置与 LIFO、error 四形态+警告文本精确断言+重臂+
      0 上限、参数契约 5 raise)。警告文本断言经 **io.stderr 全局字段
      替换**捕获(零产品 API 污染)。首轮 5 败全为测试侧期望值笔误,
      库侧仅三处返工:resolve 剥尾斜杠、inspect 负 depth 不钳
      (maxArrayLength/maxStringLength 才钳 0)、error level 0。
- [x] 覆盖率账:Lua 侧总 **96.87%**(3307/107;批次 5 收官 96.42%,
      **超基线**);events 100%(127/0)、util 99.53%(1 暗 = `return
      "%" .. spec` 死防御,CONSUMING 表全覆盖后不可达)、path 98.82%
      (2 暗 = lfs 缺席回退 $PWD 与 "/" 两腿,luna 随附恒在 lfs,
      结构性登记不入债)。C 台账与批次 4/5 持平:lines 94.8%
      (2689/2837)、functions 100%(249/249)、branches 77.6%
      (948/1222)——本轮零 C 产品代码,只动测试与 Lua/文档。
- [x] 文档同步:guide/modules.md 内置表三行(后端列「纯 Lua」)+
      表后 Node 语义模块说明段(非格式模块,无 nil,err 口径,参数
      类型错一律 raise,行为按 v24 钉版,分叉见 node-parity);
      node-parity.md B 面表三行翻 ✅ + path/util/events 三节实现
      勘定块;README/architecture/index 三处标准库清单加三模块
      (README 与 index 的编辑被并行会话 logo 提交 8174e5f 顺带
      收走,内容完好);cmake/luacov.config.in include 收编三目录。
- [x] 门禁:两树 cmake --build + ctest **16 组全绿**(串行)——build
      196.70s、build-cov 321.71s(rocks 103.97s 实绿,负载 34 无假挂)。
      同窗顺手完成用户点名的 CI 修复(cmocka FetchContent 换
      gitlab.com/cmocka/cmocka 镜像,tag 56eb3a18 双源核对一致,全新
      scratch 树实测从镜像拉取成功),已单独提交。单笔提交,push 前
      fetch origin main 核对 SHA;不打 tag、不发版。

- XML 的 XPath/DTD 验证/libxml2 全家桶面、命名空间前缀展开
  (fast-xml-parser 同款「前缀原样」口径);
- YAML v1 的 anchors 发射(opts.anchors 列后续批次再议)、schema/tags
  高级面;
- stream 的 webstreams/异步迭代器/setEncoding/cork 小面;
- util 的 promisify/callbackify(Lua 无 promise,契约无锚)与 types
  判等族;path 的 win32 面(平台面 Linux/macOS);
- 全局 setTimeout 与 REPL 集成(维持推迟,启用条件见下轮方向);
- tomlc99 回退不预设,仅当 tomlc17 vendor 编译受阻时启用(决策记录 1)。

### 批次 7(stream)实录(2026-10-01)

- [x] **vendor:无**(纯 Lua 基于 events,零依赖,与派发一致)。单文件
      `lua/modules/stream/init.lua`(690 行),经既有 LUNA_STDLIB_WRAPPERS
      glob staged 到 `luna_modules/`,零构建接线、零注册代码。
- [x] **实现**:Stream 基类(destroy 幂等收口:opts._destroy 摘外部
      资源、err 非 nil 先发 'error'(无监听按 events 语义 raise)、恒发
      一次 'close';未决 write 回调**在途一笔 + 排队若干**以
      "stream destroyed" 结账)+ Readable(on/once 覆写——首个 'data'
      监听真正落位后才开流:events 的 newListener 在落位前发出,在
      钩子里开流会把头几块丢给空气;拉式 _read 走 _pushseq 干涸检测,
      _read 里的 push 只入账不投递防递归无底)+ Writable(队头分发
      _write、drain/finish、end_ 与 s["end"] 双拼、write after end/
      destroy 发 error 事件)+ Duplex(两脸 mixin)+ Transform(flush
      完成即 finish、读侧缓冲排空才 end)+ pipe(data→write 返回
      false 即 pause、drain 续推、unpipe 摘四钩;源 end 默认带
      dest:end_(),opts["end"]==false 关掉;源 error → unpipe 后
      destroy(同错)到目标——**与 Node 裸 pipe 的有意分叉**,把
      pipeline() 的契约并进 pipe;目标 error/close 只 unpipe 源不炸)
      + autoDestroy 两脸判据(纯流单脸到位即关,duplex/transform 要
      'end' 与 'finish' 都到齐)。
- [x] **适配器三件**:duplexFromSock(sock)(write 转发、常驻读回调转
      push、_destroy 关 sock、读错误走 error→destroy 联动)、
      readableFromChunks(字符串或块表,一次一块按需推)、pushReadable
      (loop.http onData 的「视余量」形态:push 返回 false 即收手,
      缓冲排空时 _read 回灌)。
- [x] modules 组 +8 用例(83→91):readable_flow(chunks 源顺序收口/
      暂停态 readable/监听器内 pause 即刻生效/once 开流)、
      backpressure_roundtrip(hwm 记账与 drain、pipe 慢汇 20 块往返
      80 字节、unpipe 摘钩后数据与 end 都不再进汇)、error_destroy_
      close(_read 炸联动/destroy(err) 无监听 raise/write after end
      +destroy 对在途回调结账/finish→autoDestroy→close)、
      pipe_error_propagation(源错毁目标/目标错只 unpipe/opts["end"]
      开关)、transform(中继+flush 尾块 finish→end→close/两跳 pipe
      全链/cb(err) 联动)、adapters(fake sock 全生命周期:写转发/
      EOF→end/两脸→close/destroy 恰关一次、读错误、pushReadable 30
      块视余量+EOF)、edge_legs(非串块按 1 记账、end_ 尾块/finish
      回调/destroy 后静默、settle 幂等、write cb(err) 与 _write raise
      两条 destroy 路、write after destroy、push after destroy/EOF、
      干涸 _read、双 pause/流动中 resume 早退、transform 的 drain/
      cb 成功/raise/settle 幂等/destroyed 后 settle、flush cb(err)/
      raise/幂等/flush 内 destroy 守卫、duplexFromSock 写错误与迟到
      读回调、opts._destroy raise 走 io.stderr 警告)、参数契约 8
      raise;loop 组 +1 真 sock 回显集成
      (pthread echo 服务器,写转发/EOF→end/两脸到齐→close,
      194→195 用例)。**首轮 5 败 + 修复后浮出 1 败 + 边腿用例 2 败,
      均为测试侧期望值笔误**(同步源上挂 'data' 即开流、双笔 write
      各结一次才 drain、unpipe 摘不掉已发生的 end_、push 满额提前
      返回 EOF 要再拉一轮、end_ 尾块会进 sink、error 串带 chunk
      前缀),库侧仅一处返工:destroy 对**在途** write 回调也结账
      (原实现只结排队中的;Node 语义在途+排队都结,补 _wflight 记账)。
- [x] 覆盖率账:Lua 侧总 **97.08%**(3720/112;批次 6 收官 96.87%,
      **超基线**);stream **98.80%**(413/5)——edge_legs 用例把首轮
      89.71%(43 暗)的可达腿尽数收掉(非串块记账、两条 destroy 结账
      路、settle 幂等、EOF/destroy 后 push、干涸 _read、pipe 类型、
      end_ 三形态、transform/flush 的 raise 与守卫腿、适配器错误面、
      _destroy raise 走 stderr),余 5 暗全为结构性防御腿登记不强测:
      _autoclose 的 closed 重入守卫、pipe 尾流启动 body(条件行可达,
      body 因 `self:on("data")` 经覆写先开流而不可达)、_finish 与
      _flush_and_finish 两个重入守卫。C 台账与批次 6 持平:lines
      94.8%(2689/2837)、functions 100%(249/249)、branches 77.6%
      (948/1222)——本轮零 C 产品代码,只动测试与 Lua/文档。
- [x] 文档同步:guide/modules.md 内置表 stream 行(后端列「纯 Lua」)
      + Node 语义模块段补 end_ 拼写注;node-parity.md B 面表 stream
      行翻 ✅ + stream 节实现勘定块(工厂命名与 end_、开流时机、拉式
      源循环、背压双向、autoDestroy 两脸判据、pipe 分叉、记账简化、
      适配器三件与用例清单);README/docs index 标准库清单加 stream;
      cmake/luacov.config.in include 收编 luna_modules/stream/。
- [x] 门禁:两树 cmake --build + ctest **16 组全绿**——终轮 build
      240.67s、build-cov 416.48s 干净全绿。过程中 shield/theseed 并行
      会话高频跑 ctest,全程等真空档(连续 60s 无外来 ctest)再起跑,
      cov 树曾见 linedit 一败(并行时段瞬时,单跑 0.19s 实绿、复跑
      干净 16/16),普通树曾见 rocks 一败同判(单跑 21.08s 实绿)。
      单笔提交,push 前 fetch origin main 核对 SHA;不打 tag、不发版。

### 批次 8(os 补齐 + child_process 糖 + timers 自动排水)实录(2026-10-01)

- [x] **vendor:无**(零新依赖,全落既有 src/luna_loop.c 与 lua/luna.lua)。
- [x] **实现**:loop.os 增五件——`arch`(uname machine 的 `x86_64`/`amd64`→`"x64"`、
      `aarch64`/`arm64`→`"arm64"`,其余原样;Node 的名字,不是 uname 原文)、
      `release`、`EOL`(字段常量 `"\n"`,`luaL_newlib` 后 setfield,非函数)、
      `userInfo`(uv_os_get_passwd→`{username,uid,gid,shell,homedir}`,shell 无条目
      时键缺席)、`availableParallelism`;五件同步直返、失败 `luaL_error`,与既有
      os 面同款,REPL 的官方 `os` 全局不受影响。child_process 两糖:`exec` 在栈上
      拼出 `sh, {"-c", cmd}` 后逐字转调 `l_process_run`(聚合/cwd/交付语义零分叉),
      `execSync` 走 popen——fread 4K 循环读尽 stdout(任何退出状态都先返回)、
      pclose 三元组拿 wait 状态,非零退出或死于信号 raise。**实现勘定**:两糖落
      **C 面 process_funcs** 而非派发单初记的"Lua 层"——糖要随
      `luaL_requiref(L,"loop")` 打开模块的每一处(CLI、两个 cmocka harness、嵌入方)
      同脸出现,Lua 装饰层会漏掉后两者(node-parity.md 已记账)。
      **脚本尾部自动排水**:loop.c 出 `maybeDrain()`(无活句柄 no-op 返 false,
      有则 `g_interrupted=0` 后 `uv_run(DEFAULT)`),`lua/luna.lua` 的 `run_script`/
      `run_eval` 在主块**正常结束**后调用(出错不排——Node 未捕获异常即退;REPL
      不排,三张同步契约不动;`-i` 先排再进控制台);排水中 `^C` 与 `run()` 同款
      raise `interrupted`,`exit_code_for` 出 130——一张契约贯穿脚本体与排水段。
- [x] **顺手根因修复一个真产品 bug(prepare-ref)**:keep-alive 的 prepare 钩子
      此前是 ref'd 句柄——只要还有**任何**用户句柄(unref 与否)它就撑着循环,
      "只剩 unref 句柄"时 `run()` **永不返回**(实测 `-e` unref interval + run()
      挂死 rc=124;libuv 的 `uv__loop_alive` 只数 ref'd 活句柄,deps/libuv 源码
      核实)。修复:prepare 于 init 时 `uv_unref`——存活账本全归用户句柄自己,
      钩子照常在循环每一拍照跑(^C 翻译与 attach 轮询不受影响);副作用是把
      "unref 的一次性定时器在 run() 里照常触发"翻转为 Node 语义:循环不转,
      永不触发(新测试钉住)。
- [x] **测试**:loop 组 +4(195→199)——os 五件契约、exec 糖(shell 聚合/cwd/
      无 cb 用法错)、execSync 面(20000 字节跨 4K 读循环、exit 3 raise 带
      code 3、`kill -9` 信号腿)、maybeDrain 契约(idle→false、挂起 timer→
      drained、全 unref→安静退出、unref interval + run() 返回——钉住 prepare
      修复);cli 组 +4——脚本 setTimeout 不调 run 也触发、全 unref 立即退出且
      NEVER 不出现、`-e` 同口径、排水中 ^C → 130(drain_wait.lua 用 1200ms 短
      定时器:epoll EINTR 只在下一个唤醒点恢复,标记由下一拍的 prepare 读到);
      新增 3 个夹具。验收线:既有全部 loop 用例(自调 run)原样全绿。
- [x] **覆盖率账**:Lua 侧总 **97.32%**(3737 行 103 缺,批次 7 收官 97.08%);
      **luna.lua 95.95% → 98.72%**(余 2 暗行均为旧登记结构腿:chunkname 普通
      构建回退、socket.unix 捆绑必在——排水用例顺带点亮了此前的 attach 竞态
      恢复腿)。C 侧 lines **94.6%**(2761/2919;批次 7 收官 94.8% 2689/2837,
      分母 +82 为批次 8 新 C 面,72 亮/10 暗)、functions **100%**(256)、
      branches **76.8%**(968/1260)。10 暗行逐条登记:arch 的 uname 失败
      raise/arm64 两臂/裸机透传臂(本机 x86_64 平台恒定)、release 与 userInfo
      的 getter 失败腿(平台恒真)、execSync 的 popen NULL(fork/资源耗尽)与
      pclose==-1(wait 失败)——平台/资源类不为行数写假测试;maybeDrain 与
      exec 全亮,execSync 信号死亡臂由 kill -9 用例点亮。坑:gcovr 首跑撞
      GcovrMergeAssertion——9-27 的旧对象名 `luna_loop.gcda/.gcno`(行号平移
      前的计数)与今日 `luna_loop.c.*` 并存,on_kick 起始行 91/92 冲突,清掉
      陈旧对后出账(与第七轮"陈旧 gcda 配对"同族)。
- [x] **文档同步**:guide/loop.md(API 表 +maybeDrain 行、"脚本尾部自动排水"
      一节、os 表 +5 行、process 节改四入口与行为约定);node-parity.md B 表
      timers/child_process/os 三行翻 ✅ + 三节实现勘定(maybeDrain 形态与
      prepare-ref 根因、os 键名定稿、exec/execSync 的 C 面落地理由);
      architecture.md 的 prepare 句(自身 unref,keep-alive 账本只记用户句柄)
      与 loop 段补记;README/index 无涉(loop 只出现在测试清单)。
- [x] **门禁**:两树 cmake --build + ctest **16 组全绿**——build 树 231.26s
      (loop 157.65s、rocks 21.10s 实绿);cov 树全量 15/16 后 rocks 单跑
      128.37s **实绿**(首跑失败查明为环境:并行会话把 /tmp tmpfs(26G)填到
      19G+ 后写满,rocks 夹具的 scratch 落 /tmp 撞 ENOSPC;该窗口连工具输出
      都写不进,df 核实后等并行会话清理即恢复,单跑复绿证明非回归)。文档构建
      4.02s。单笔提交,push 前 fetch origin main 核对 SHA;不打 tag、不发版。

## 下轮方向

- tsock 结构体泄漏:**已完成(第十二轮,见上)**。
- luna_loop 分支覆盖专项:**已完成(第十三轮,见上;余量全为
  逐臂表征)**。
- malloc 注入基建维持第八轮判定(仅在真实回归疑点时再评估;
  第十三轮确认 t_lowermem 窗口已覆盖其对大分配的全部可达面)。
- 覆盖率专项收官:C/Lua 两侧余量全为逐腿/逐臂/逐方向表征
  (第十七、十八轮后含 C 分支方向层:luna_main 62%、
  luna_kernel 93.75%、luna_line 84.29% 的残暗方向全数登记;
  第十九轮非 C 侧盘点收官),转入按需修补。
- **Node 方向补齐(第二十轮立项)**:批次 2–8 逐批实现(见上节),
  每批两树全绿后单提交推进——**批次 2–8 已全部完成(2026-10-01 止,
  各批实录见上);B 面表原记「后续批次」的 net.connect/connectTls
  多地址回退亦于 2026-10-01 完成(见上)**。REPL 集成循环(每求值
  后 drain nowait)维持推迟,启用条件:行编辑可超时读或唤醒线程可
  定时(见 docs/node-parity.md timers 节)。
- **net.connect/connectTls 多地址回退(2026-10-01 CI 勘定)**:**已完成
  (2026-10-01,见上)**。两条拨号路径解析结果留链逐地址回退(Node
  autoSelectFamily 语义):失败句柄关掉重初始化换下一条,全败才报
  最后一条错误;plain 与 TLS 同修(TLS 侧失败经 poll SO_ERROR 异步
  浮出,poll 句柄要随 fd 一起关掉重开)。回归用例
  `test_dial_localhost_v4_only_listener_succeeds`:v4-only 监听 +
  localhost 拨号必须成功,plain/TLS 一条用例两腿同盖。
- **lualogging 集成(2026-10-03 用户定案,同日完成)**:集成 LuaLogging
  (lunarmodules/lualogging,1.8.2,MIT,log4j 式 appenders:
  console/file/rolling_file/socket/email/sql)进 luna 环境——默认
  `require 'logging'` 开箱可用、随环境分发,不走用户手动装;依赖
  LuaSocket 已有。**落地实录**:子模块 deps/lualogging 钉 v1.8.2
  (465c994);CMakeLists 按 dkjson 同款 staging(src/logging.lua +
  logging/ 目录进 luna_modules/,随产物分发);可选驱动(sql 的
  DBI、copas、ngx)都在函数体内懒 require,随发集合干净加载。
  **Lua 5.5 兼容验证(上游 CI 只到 5.4,luna 是新地面)**:上游回归
  在 luna 下逐文件实测——env 10 项 + console/file/rolling/socket/
  SQL 全过(SQL 无 luasql 驱动走上游自带 SKIP 分支;socket 上游只
  发不收,另起本地回环证明真实投递);generic 10/11,唯一红的是
  format_error_stacktrace 把栈分隔符硬编码 ==3——`luna test.lua`
  下得 4(脚本运行器多一行可计数帧 `(luna):163 run_script`;
  rewrite_stacktrace 的 gmatch 不吃无尾换行的末行,故 (luna):320
  不计)。同断言 stock 5.5 直跑(`lua_host generic.lua`)得 2 同样
  红——只有经 dofile 的精确形状才得 3,纯宿主帧形状敏感,与 5.5
  语义无关;源码零补丁、子模块零改动;mail 需外部 SMTP 未在本地
  跑,其唯一依赖 socket.smtp 已随发就位。若日后上游收 5.5 进
  CI,此处即为现成记录。四腿手工冒烟(级别过滤/
  file 落盘/200B 滚动到 .1/socket 回环收包)全过;唯一差异是
  generic.format_error_stacktrace 把栈深硬编码 ==3——luna 脚本
  运行器多一行可计数帧 `(luna):163 run_script`
  (rewrite_stacktrace 的 gmatch 不吃无尾换行的末行,故 (luna):320
  不计),属宿主帧敏感非 5.5 语义差异(stock 5.4 直跑 3 过、任何
  Lua 帧包装器下都会 +1),源码零补丁、子模块零改动;若日后上游
  收 5.5 进 CI,此处即为现成记录。示例四段(级别/格式/文件/滚动)
  贴真实输出;docs/stdlib/logging.md + 侧栏 + 总览行 + README
  能力清单行。**门禁实录**:15/16 绿,rocks 组
  test_install_lock_reproduce_cycle 红于网络腿——三条 manifest
  镜像(luarocks.org/moonrocks-mirror/loadk)全部下载失败,主机
  直连 curl 对 luarocks.org 与 raw.githubusercontent.com 亦 TLS 即断
  (unexpected eof);同测试 09:41 全绿、luarocks.org 10:30 仍可达,
  属主机网络中断非本轮改动(失败点在 vendored luarocks 抓 manifest,
  先于任何模块解析);网络恢复后复跑 rocks 全绿再 push。
  **复核(同日 13:40 后,记录校准时)**:全量重跑 16/16 绿(Total
  256.82s,rocks 12.39s 过),网络已恢复,早前红确为网络中断。

## nightly 平台矩阵试运行转正清单(2026-10-02 登记)

矩阵跑 36970096993 实录:linux 双绿;**macos 构建通、ctest 11/16**;
**windows Configure 止步**。转正条件与已挖证据如下(无本地 mac/win,
每轮验证 = 一次 CI 队列往返,建议专门会话成批做):

- **macOS 运行期(5 组红→run 36995166976 后 3 组;run 37038767850
  仍 3 组:serve 19 挂 / loop 14 挂 / linedit 1 挂,13/16)**:
  - `cli` 与 `loop`:**已转绿(a0bc4a7,mac 实证 12/16)**。127 共因
    (GNU `timeout` 缺席)与 realpath/strerror/信号号三处平台硬编码,
    CMake find_program(timeout|gtimeout) + 期望值平台中立化修平。
  - `rocks`:**已转绿(b1ec090 后 run 36997100684 实录 13/16)**——
    `run_luna_in` 第二处 timeout 漏网补平即过。
  - `loop`:**间歇**(run 5 过 / run 6 600s Timeout / run 7 45.7s 挂
    **34 个**(前次登记"四点"系漏计)/ run 8=37009767450 600s
    Timeout / run 9=37038767850 **14 挂,113s 正常收尾**)。挂点
    实录(349c3a8 树行号):889(watch 族断言 false≠true)、1200
    (七连判第 5 假,时序敏感)、1230(输出截断 `no-lo`)、
    **1785 `listen failed: Invalid argument`——已修(b9fe708)**:裸
    bind 以 sockaddr_storage 全长(128)作 addrlen,XNU 把 sa_len
    改写成 buflen 后 in_pcbbind 校验族长即 EINVAL;Linux 不读
    sa_len 故一直无事。改传 family 精确长度(与 libuv 内部一致)。
  - **loop 600s Timeout 根因(run 8 定案,f89bf8d 已修并实证)**:
    `test_net_sock_addr_tcp` 客户端回调读服务端回调才赋值的
    `cshared`(注册序 6102,在全部 TLS 测试之前)——mac 上 kqueue
    顺序常使客户端回调先跑 → 索引 nil 报错 → `srv:close()` 被跳过 →
    `loop.run()` 永不退出。四轮证据:36995166976/36997100684/
    37009767450 三次同卡同错、36999185064 赢竞态通过(与 b9fe708
    无关且先于它)。两会合点改写后 **run 9 实证:sock_addr_tcp/pipe
    双 OK,TLS 段首次跑达且全绿**——`test_tls_listen_with_custom_ca_
    roundtrips`(1785)、`default_verify_rejected`、`bad_cert_throws`、
    `http_get_over_tls`、`tls_listener_coordinates`、`tls_listen_
    rejects_bad_hosts`、`tls_client_dials_by_hostname` 等 run 7 的
    EINVAL 连坐全数转绿:**b9fe708 mac 实证到手**。
  - **loop run 9 残余 14 挂(存量,run 7 同在)**:3 存量断言
    (889/1200/1230)+ 11 个**错误注入/资源类**:`proc_run_cwd`(3153)、
    `fs_write_enospc`(4513)、`sock_write_oom`(4642)、`udp_broadcast_
    send`(4716)、`udp_send_oom`(4733)、`fs_oom_legs`(4812)、
    `proc_oom_cleanup`(4840)、`tls_pre_handshake`(5334)、
    `tls_fd_exhaustion`(5624,实录 `expected EMFILE-flavoured…got
    connection refused`——mac fd 耗尽语义差)、`tls_write_oom`(5668)、
    `tls_write_pend_oom`(5722)。单查一组,疑 macOS ulimit/RLIMIT 与
    错误注入手段的平台差。
  - `linedit`(749):`test_read_cancels_wakes_and_reports_eof` 的
    `forkpty` 返回 -1(`assert_not_equal(-1,-1)`),而 line 组同函数
    全绿——疑与该测试前序状态/资源耗尽相关,单查。
  - `serve`(140/47,336s 慢跑):**19 挂两轮同名单**(run 8/9:
    `test_socket_file_created` 起 19 个,attach socket 占大头)。
    监听 squat/rebind 语义系列(`listener back…`、`bind works again
    once the squat is gone`),macOS SO_REUSEADDR/端口复用行为差,
    逐断面看。
  - 已修(3 轮 CI 定位):`pty.h`→`util.h` 三处(line/serve/linedit,
    f8abd28)——构建期已通。
  - **serve 组转绿(2026-10-08,run 37829368692 mac 14/16)**:根因
    为 vendored luasocket 的 `unixstream_trybind/tryconnect` 以
    `sizeof(sun_family)+len` 算 sockaddr 长度——Linux 拼法。macOS 的
    `sa_family_t` 只有 1 字节,sun_path 偏移 2,于是内核把 socket 文件
    绑到比路径少末字符的短名(`luna-<pid>.sock`→`.soc`,teardown 残留
    清单 od 实证);bind/connect 同式算短名故通道自洽可用,而
    stat/os.remove/access 按真名全落空、重绑恒 EADDRINUSE。改传
    SUN_LEN(offsetof(sun_path)+strlen,unixdgram 本就如此)。子模块
    保持钉 v3.1.0(上游 master 亦未修),修复以 `deps/luasocket-patched/
    unixstream.c` 覆盖件承载、CMake 接线(c557baf);Linux 两式同值
    零行为变化。serve 组 + attach 连坐 + linedit pty 池连锁皆此一根因。
  - **loop `test_net_sock_addr_tcp/pipe` 竞态(2dfbcce+6edf2d2,run
    37829368692 mac 实证绿)**:回调次序无契约,客户端 connect 回调可
    先于服务端 accept 回调到达,报告串若在客户端回调内冻结则服务端侧
    字段为 nil(报告与关流必须同在 finish() 门内拼装——与 TLS 同名
    用例一致)。教训:此题 f89bf8d 已修过,74fe229「29 新测试」整体重写
    luna_test_loop.c 时丢了修复,2dfbcce 重新落地。
  - **loop fs.watch(6edf2d2 的 300→2000ms 无效;d468676 定案另修)**:
    失败用例是进程内**首个** watcher——`watch()` 返回后 CF 线程才异步
    建 FSEventStream,immediate 里的 writeFile 抢在 `FSEventStreamStart`
    前落地即永久丢事件(加宽窗口无效正说明丢而非慢;同进程后续文件型
    watch 在热 CF 循环上即时达)。触发写改 setTimeout(300) 让流先活,
    检查窗 2300,失败分支吐原始事件串。**run 37860858374 mac 实录:loop
    组转绿**(16 组仅 1 挂,即 linedit)。
  - **linedit `test_read_cancels_wakes_and_reports_eof` forkpty EAGAIN
    定案(c9987e7 修)**:亚 errno 35 且 serve 绿后仍挂,非 pty 池连锁。
    真因是本组 `test_wake_thread_failure` 压 RLIMIT_NPROC soft=1 后
    **恢复被 macOS 拒绝**(setrlimit 报错、rlim_cur 一直读 1;探针实录
    `nproc rlim_cur 1, system procs 582~603`),其后本组唯一 fork 必然
    EAGAIN(与 line 组从不碰 NPROC 而 17 例 forkpty 全过互证)。修复:
    注入窗口整个搬进 fork 出的子进程,父进程永不碰 limit,子进程以退出
    位上报(8=注入未被接受/1=管道起/2=无新线程);pty 用例的重试与
    诊断报告保留为瞬态兜底。
  - **leak guard / 诊断(8e8e76d, 94c28ac)**:cmocka 断言 longjmp 会
    跳过测试尾 kill/waitpid/close,挂单 master+slave 占住宿主 pty 池
    ——10 个 serve 用例 + linedit pty 用例登记 per-case teardown 统一
    杀子回收 master。
  - **fs.watch FSEvents 目录自身事件(5198b54)**:转正首跑
    run 37877698938 拉响(转正安全网首战建功)——子文件创建时 macOS
    FSEvents 额外投递被监视目录**自身**的事件(`luna-loop-fs-watch|
    rename`,inotify 无此形态;libuv 1.44.2 fsevents.c 直通,len==0
    回溯 basename + Created 置 kFSEventsRenamed 复合掩码),成员判定
    加收目录自身 basename,平台真相非噪声;失败分支的原始事件串诊断
    一次定位。
  - **✅macOS 腿已转正(2026-10-09,e029abf)**:matrix `experimental:
    true→false`,构建或测试红同权拉响夜间(Test 步 continue-on-error
    旋钮保留)。转正即验证:首跑 37877698938 立即拉响 fs.watch 目录
    事件形态(见上条),修后 **run 37879305912 mac 16/16 全绿**——
    首条全矩阵转正下四腿 success 实录(此前 37865418197 已 16/16)。
    运行期病根五项全部根因级收口:SUN_LEN(c557baf)、sock_addr
    finish() 门(2dfbcce+6edf2d2)、fs.watch 首监视器竞态(d468676)、
    NPROC 注入 fork 隔离(c9987e7)、目录自身事件(5198b54)。
- **Windows**(探针三层实录,36999185064 轮;run 37009767450 复证
  清单 2–5 五处并发报错、zlib/lfs/lua-zlib 编过,清单 1 `mode_t`
  该轮未现身):vcpkg 静态 zlib →
  Configure 过 → LUA_USE_POSIX 平台收窄(349c3a8,Windows 暂无
  io.popen)后,**完整阻碍清单出炉**(按编译序):
  1. `src/luna_kernel.c(89)` `mode_t` 未声明——k_chmod(Windows 走
     `_chmod`/`<io.h>` 或守卫);**✅已修(2026-10-03)**;
  2. `deps/luasocket/src/usocket.c(21)` `sys/poll.h`——CMake 无条件编
     unix 源,需按平台选 `wsocket.c` 系;**✅已修(2026-10-03)**;
  3. `deps/luasocket` `unixstream.c`/`unixdgram.c` `sys/un.h`——Unix
     domain socket,Windows 无,目标整体跳过(socket.unix 是 attach 面);
     **✅已修(2026-10-03)**;
  4. `src/luna_loop.c(45)` `arpa/inet.h`——loop 网络层 POSIX 头,需
     winsock2 等价面或走 libuv 抽象(设计裁定点);**✅已修(2026-10-05,
     用户派发跨平台修,冻结解除):_WIN32 分支 include winsock2.h +
     ws2tcpip.h(ntohs/htons/addrinfo 同义),TLS 手写层(1179 行,fd/
     errno 语义 POSIX 专属)整层编译出,connectTls 走无 TLS 既有降级;
  5. `test/luna_test_linedit.c(28)` `poll.h`——测试 harness 同修。
     **✅已修(2026-10-03)**。
  正式依赖口径(源码 vendor 与否)待决。daily.yml matrix windows 项
  experimental:true **✅已转正(2026-10-08,12c90bd)**:POSIX 面守卫全落
  (luna_main/luna_line/luna_test_main 等),CMake 工作流 windows 全量
  测试连日绿(37549008691 双腿 success 以降);转正首跑 run 37799557602
  windows 腿 success——experimental:false 下构建红即拉响夜间。
  macOS 腿同日跟进转正(e029abf,2026-10-09,实录见 macOS 节)。

  **1/2/3/5 消项实录(2026-10-03,run 37038767850 九处报错定清单)**:
  1. k_chmod 的 `_WIN32` 臂走 `_chmod(path,(int)m)`(`<io.h>`,CRT 的
     chmod,只认 0200 写位——kernel.chmod 只用于 attach socket 的 600,
     够用);`<errno.h>` 补进 `_WIN32` include 块(原块只 process.h,
     `strerror(errno)` 在该臂要用);
  2+3. `LUASOCKET_SOURCES` 按 `if(WIN32)` 分臂,与上游 `socket.vcxproj`
     逐一对齐:Windows 只留 `wsocket.c`,unix 三件(`unix.c`/`unixstream.c`
     /`unixdgram.c`)整体跳过——`unix.c` 引用 `unixstream_open`,只砍两件
     会 LNK2019,故三件同进同出;`LUASOCKET_INET_PTON` 只在非 Windows
     定义(它是"平台没有 inet_pton 才自造"的兜底宏,Windows 下定义会与
     ws2tcpip 的 `__stdcall inet_pton` 声明撞 linkage——上游 vcxproj 亦不
     定义);`ws2_32` 链接进 luasocket(WinSock DLL,上游同款)。配合
     `src/luna_main.c` 的 `luaopen_socket_unix` 声明与 mods[] 条目加
     `#ifndef _WIN32`——否则砍了 unix.c 后 luna.exe 链接期 LNK2019;
     Lua 面无需改,serve.lua 本就有 "socket.unix unavailable" 降级腿。
  5. `test/luna_test_linedit.c` 整个 harness `#ifndef _WIN32` 包裹,
     Windows 出空套件 stub `main`(forkpty/poll/waitpid/重执行 pty 全 POSIX,
     只挡 poll.h 会立刻撞下一行 pty.h——按平台修一次到位);同目标
     `util`(libutil,forkpty 所在)在 WIN32 不链(`test/CMakeLists.txt`),
     否则编过也 LNK1104。
  **预判下一层(CI 实证后定案,均未动)**:`luna_test_line.c(19)` 与
  `luna_test_serve.c(7)` 的 `poll.h` 是同构造孪生(仅 linedit 上过 CI
  报错表,疑因 msbuild 失败即停调度);`luna_line.c(20-21)` pthread.h/
  unistd.h、`luna_main.c(5)` unistd.h(两者均挂在 luna.exe/luna_test
  的 item 4 依赖后面,此前从未被调度到);line/serve 两目标的 `util`
  链接同 linedit 待条件化。

  **CI 实证(run 37059350461,2026-10-03,13e37fb 推送后)**:
  - **清单 1/2/3/5 逐条核销**:windows job 全量 619 处唯一报错中,
    `luna_kernel.c` 仅剩 C4996(`strerror`,警告级)、luasocket 工程
    零报错、`luna_test_linedit` 零报错——四项对应源文件全部从错误表
    消失。run 结论 success(linux×2、macos 三腿绿,windows
    experimental 腿红不拉倒整个 run——daily.yml 既定设计,该标志
    本轮不动)。探针深度:上轮止步 4 个工程,本轮 12 个测试工程
    进到编译期。
  - **新层 1(612/619 同根,本轮主体)**:`build/generated/luna_lua.h`
    (configure 期由 `cmake/luna_lua.h.in` 生成)以 C++ 原始字符串
    字面量 `R"LUNA_ENTRY(...)LUNA_ENTRY"` 装十段嵌入式 Lua 源,
    消费方却是按 C 编译的 TU——6 个白盒测试(repl/plugins/magic/
    introspect/highlight/complete)各 102 处报错全指此头。
    本地复核(非推断):同一头文件 gcc 默认模式(gnu17)编过、
    `gcc -std=c11` 复现 MSVC 同类错(`missing terminating "
    character`)、g++ 干净——Linux 全绿靠 GNU 方言放行,MSVC 按 C
    编译即炸。`src/luna_main.c` 本身未现身错误表(排在 item 4
    的 luna_loop 依赖后面)。根修方向:模板改转义常规 C 串逐行
    拼接,弃原始字符串。
  - **新层 1 根修完成(2026-10-03,16/16 绿后提交)**:`CMakeLists.txt`
    新增 `luna_c_string_literal()`,转义序固定 `\\`→`\\\\`、`"`→`\"`、
    CR→`\r`,每个 LF 换成 `\n"`+换行+`"`(每源行一条相邻字面量,编译
    期拼接),十段 `file(READ)` 变量逐个过转换;`cmake/luna_lua.h.in`
    十常量改 `static const char X[] =\n@X@;` 形(模板注释同步改 ASCII:
    原 `lua/*.lua` 内嵌 `/*` 触发 -Wcomment,破折号是为 MSVC 码页留
    意)。验证三层:①字节级往返 10/10 全一致(逐字面量解码比对
    `lua/*.lua` 原文,最大 rocks 18298 B/515 字面量);②严格编译
    `gcc -std=c11 -Wall -Wextra -Werror` 与 `g++ -std=c++11
    -pedantic-errors`(免豁免)双双干净;③C 模式 pedantic 仅余
    `-Woverlength-strings`——C99 只保证 4095,MSVC 硬限 65535 而最大
    常量 18298,是保证下限非能力限制(旧原始串在 g++ pedantic 下不
    点此名,属 GCC 未对该形态实现此告警,非形态优势)。预期 Windows
    CI 消 612(新层 1),余量=item 4+新层 2+未调度层,待 daily 触发
    核销。
  - **新层 1 CI 核销(run 37089224548,2026-10-03 手动触发,7609204
    推送后)**:windows 腿 luna_lua.h 报错 **612→0**;唯一错误清单收敛
    到 7 处 C1083,与登记严丝合缝——luna_loop.c(45) arpa/inet.h
    (item 4,按裁定不动)、luna_test_line.c(19)/luna_test_serve.c(7)
    poll.h、luna_test_cli.c(13)/luna_test_covsum.c(15) sys/wait.h、
    luna_test_rocks.c(17)/luna_test_modules.c(10) unistd.h(新层 2);
    六个白盒目标已正常编出(luna_test_introspect.exe 产物在日志),
    luna_main.c(5) 仍未被调度到(luna.exe 卡在 luna_loop 依赖后,
    与预判一致)。619→7,同根 612 清零收官。
  - **待裁定——派发机制条款与登记方向相反**:派发文字"改以 C++ 原始
    字符串拼接,弃用普通字符串拼接"恰为本条登记方向的倒装;按派发
    自身"按 todo.md 登记方案执行"落地了登记方向(转义常规串)。
    两方向目标同为 612 清零,登记方向改动面小(头文件+模板单点;
    反向需 extern 声明+新 C++ TU 链进约 11 个目标);疑为派发文字
    名词对调笔误。机制条款定案待用户裁定,结果不受影响。
  - **新层 2(预判证实 + 三处未预判,均测试 harness POSIX 头)**:
    `luna_test_line.c(19)`/`luna_test_serve.c(7)` `poll.h`(孪生
    预判命中)、`luna_test_cli.c(13)`/`luna_test_covsum.c(15)`
    `sys/wait.h`、`luna_test_modules.c(10)`/`luna_test_rocks.c(17)`
    `unistd.h`(后两族未预判)——修法同 linedit(平台包裹/守卫),
    line/serve 的 `util` 链接待条件化。
  - **新层 2 消项实录(2026-10-03,16/16 绿后提交)**:按登记方法
    落地。五个端到端 POSIX harness——`luna_test_line.c`/`luna_test_serve.c`
    (forkpty+poll+waitpid+mkdtemp)、`luna_test_cli.c`(fork/pipe/dup2
    信号腿 + sh 命令行经 popen)、`luna_test_covsum.c`(POSIX shell
    单引号命令行,cmd.exe 不剥引号故无运行期对应)、`luna_test_rocks.c`
    (system() 的 `rm -rf`/`mkdir -p`/`cd '&&'` + timeout 守卫)——
    整文件 `#ifndef _WIN32` 包裹 + Windows 空套件 stub `main`,顶部
    注记逐文件写明 POSIX 面(同 linedit 先例);`luna_test_modules.c`
    走守卫不包裹:POSIX 面只有 unistd 的 getcwd/chdir(相对 require
    测试),`_WIN32` 下换 `<direct.h>` + `_getcwd/_chdir` 宏,91 例
    嵌入式 VM 套件在 Windows 保留可跑;`test/CMakeLists.txt` line/serve
    的 `util`(libutil,forkpty 所在)链接按 linedit 同款
    `if(NOT WIN32)` 条件化,否则编过也 LNK1104。验证两分支各自成立:
    POSIX 分支全量构建 + ctest **16/16**(203.2s,rocks 10.98s 实绿,
    负载 2.9),`-std=gnu11 -Wall -Wextra` 逐文件语法检查零新增告警;
    `-D_WIN32 -fsyntax-only` 五个包裹文件全过(stub 分支零依赖)。
    预期 windows CI 7→1(仅剩 item 4),待 daily 触发核销;
    `luna_main.c(5)`/`luna_line.c(20-21)` 仍未被调度到,MSBuild
    下界照旧。
  - **新层 2 CI 核销(run 37111296367,2026-10-03,708a3cf 触发)**:
    六个 POSIX 头报错全部从错误表消失——plugins/modules/rocks/cli/
    covsum/line 编译期零报错;windows 腿剩余错误从 7 收敛到 3:
    item 4(luna_loop.c(45) arpa/inet.h,冻结)+ 两个新浮出的
    LNK1120(plugins/modules 各 2 个 unresolved externals:`setenv`
    /`unsetenv`——MSBuild 下界按预判再兑现,两目标首次走到链接期
    才暴露)。
  - **新层 3 消项实录(同日,16/16 绿后提交)**:链接层 setenv/
    unsetenv 属同一 harness POSIX 面家族,当轮一并收掉——
    `luna_test_plugins.c` 与 `luna_test_modules.c` 顶部 `_WIN32`
    守卫加 `_putenv_s` 宏对(空值赋值即移除变量)。modules 的空值
    腿语义经产品源码核实无损:rocks.lua:135 本就把空
    LUNA_VENDOR_DIR 读作 unset,159-162 的 die-before-luarocks 对
    nil 同路,Windows 上空值赋值→移除→getenv nil→同一条腿。
    line/linedit/serve 的 setenv 用点已在新层 2 的包裹内,Windows
    不可见;`luna_test_main.c` 的用点仍堵在 item 4 后(链 luna_loop
    未被调度),登记待 item 4 解冻后随层处理。验证:POSIX 分支全量
    构建 + ctest **16/16**(306.3s)。
  - **新层 2+3 CI 终验(run 37112664683,2026-10-03,070523b 触发)**:
    windows 腿报错 **7→1 达成**——唯一剩余即 item 4(luna_loop.c(45)
    arpa/inet.h,冻结待裁定);plugins/modules 两目标链接通过
    (LNK1120 消失;luna_test_modules 链接期见 LNK4098 defaultlib
    'LIBCMT' 冲突,警告级非致命,与 strerror 的 C4996 同容忍类,登记
    不动)。`luna_main.c(5)`/`luna_line.c(20-21)`/`luna_test_main.c`
    的 setenv 仍未被调度,堵在 item 4 后,MSBuild 下界照旧。
  - 第 4 条(luna_loop.c arpa/inet.h)仍开、按派发不动;
    `luna_main.c(5)` unistd.h 与 `luna_line.c(20-21)` 仍未被调度到。
    MSBuild 失败即停的调度特性意味着这张表是下界,修完上述后
    可能再浮出。
- 转正动作:matrix 对应项 `experimental: true→false`、Test 步
  `continue-on-error` 随之归零(daily.yml 已注明)。**windows 侧已落
  (2026-10-08,12c90bd;首跑 run 37799557602 windows 腿 success 实证)**;
  **macOS 侧已落(2026-10-09,e029abf;run 37879305912 四腿全绿实证,
  全矩阵至此无试运行豁免腿)**。
- **待裁定——双每日管线并存**:`daily-build.yml`(旧,01:23 UTC,
  固定名 `daily-build` 产物 + `pkg-*`,3 天保留,不跑测试)与
  `daily.yml`(新,21:13 UTC + 手动,`luna-nightly-<os>-<arch>` 矩阵,
  14 天保留,跑全量测试)每天各跑一次,产物两套并存。一键安装三件套
  只认新名。旧管线是否退役(删文件或去 schedule)属设计裁定点,未动;
  文档已统一指向新管线(getting-started/FAQ/README,2026-10-03)。
- **滚动 nightly Release 交付面接通(2026-10-04,巡检派发)**:用户报
  install.ps1 装不上(`no nightly artifact for
  luna-nightly-windows-x86_64`)——根因:安装器查 Actions artifacts
  API,需 token 且该名 artifact 从未存在(真实产物只有 daily.yml 的
  luna-nightly-* 矩阵件),而舰队规范=滚动 nightly Release(固定 tag
  `nightly`,清旧传新,artifacts 不算交付),仓库此前没有任何
  Release 步骤。两修(8377aed + ce18fbf):
  1. daily-build.yml 补发布:平台打包改资产坐标 `luna-nightly-<os>-
     <arch>.zip` + `.zip.sha256` 侧车,根平铺二进制 + luna_modules/
     侧车(旧打包缺侧车,装到任何机器必死 module 'argparse' not
     found,一并修掉);collect 尾接 Publish nightly Release(soar
     同款 edit-or-create → PATCH refs/tags/nightly → 清全部旧资产 →
     notes 重写 → --clobber;加 chirp 的 --prerelease --latest=false
     ——仓库已有正式 v0.1.0,nightly 不抢 latest 位);Windows 试运行
     腿进矩阵(continue-on-error,daily.yml 探针口径,item 4 冻结中
     编译不过,移植落定当天资产自动进 Release);文件头部旧铁律段
     (产物只进 artifact、不得引入 Release)按舰队规范改写。
  2. install.ps1 改匿名直拉 `releases/download/nightly/<资产名>.zip`
     (公开仓库免凭据),附 .sha256 校验(mismatch 只告警不拦——夜间
     滚动换资产的竞态,装后 --version 冒烟才是真门);token/Mirror
     保留作兜底,非 404 失败且手持凭据时回落旧 artifacts API;404
     报错文案指向 Release 页。
  实录:首发 run 37169394386 的 Publish 步栽在 gh 无 git 上下文
  (collect job 不 checkout,`gh release view` 靠 git remote 发现仓
  库直接扑空)——env 加 `GH_REPO=github.repository` 解(ce18fbf);
  复跑 run **37169762132 全绿**(posix×2+macos 绿,windows 试运行腿
  红=已知的 item 4,continue-on-error 不拖垮,Configure 过、
  Build 止步同前)。链路实测(无凭据 curl):直下 linux-x86_64 zip
  912K + `.sha256`,`sha256sum -c` OK;zip 根平铺 luna +
  luna_modules/(argparse/init.lua 实存)+ PLATFORM-NOTES.txt,与
  两安装器的解包断言逐一对上;**解包二进制实跑 `luna 0.1.0` /
  `-e '6*7'` → `Out[1]: 42`**;windows 资产 URL 现为 404(脚本 404
  臂指向 Release 页,资产随 item 4 解冻自动上线);notes 表格渲染
  实查无串行。nightly tag 指向 ce18fbf=main HEAD,下轮 schedule
  自愈重指。
  **核销(2026-10-04,run 37174407997 main 触发)**:自愈重指实证——
  tag `nightly` ce18fbf→**08d2d07(=main HEAD)**;清旧传新幂等:9 资产
  删光重传成功、无残留;notes 重写带新 sha(`nightly-20261004-
  08d2d073`);run 页 artifact **daily-build 出现**(第一次核销发现
  重写时丢掉了上库步,汇总 zip 只组装没人传、清理步成死码——按
  soar 先例补回,commit c114f8f);链路三级复验:匿名直下 + 侧车
  sha256sum -c 过 + 解包 `Out[1]: 42`(与上轮哈希不同属 zip 时间戳,
  走自身侧车验证即可)。create 时冻结的 targetCommitish=ce18fbf 是
  GitHub 元数据,下载 URL 跟 tag 走,与 soar/chirp 同形态。
  **install.sh 同款匿名化(2026-10-04,commit 08d2d07)**:主路径改
  releases/download/nightly/ 匿名直拉 + .sha256 校验(mismatch 告警
  不拦),token 保留兜底、非 404 回落旧 artifacts API,404 指路
  Release 页;LUNA_INSTALL_MIRROR 环境变量落实(此前只在文案里)。
  实测三类路径:纯匿名(无 token 无 gh)直装本机成功(`Out[1]: 42`)、
  --token 回归绿、404 臂 exit 1 指路;修一处 set -e 吞失败态隐患
  (curl 失败时 -e 直接杀脚本,die 文案永远不出现——RC 显式承接)。
- **待办登记(2026-10-04,本轮不动)**:install.sh 同款匿名化改造
  (派发只圈了 ps1;其 token 提示文案现同旧口径);docs/README 安装
  面文案仍写"无 tag 无 Release、artifact 下载需凭据"(README.md:68
  / faq.md:39 / getting-started.md:21),与 Release 交付面相悖——
  挂文档站重设计会话口径一并翻新;daily.yml 头注"不打 tag、不发
  release"亦分叉,随双每日管线退役裁定处理;Windows 资产上线条件
  = item 4(luna_loop.c arpa/inet.h)解冻,发布机制已就绪。

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

## 架构评审方向登记(2026-10-04,只登记不动,无实现授权)

用户对 luna 的完整架构评审(方向性输入,非派发;文档/README/定位
属文档站重设计会话口径,代码层不派生任何实现)。要点:

- **产品定位调整建议**:核心不再讲"通用 Lua 集成环境"或"Lua 版
  Node.js",改定为 **"A batteries-included Lua runtime for scripting,
  tooling, and lightweight services"**(开箱即用的 Lua 通用运行时,用
  于脚本、开发工具与轻量服务)。Node.js 降为 architecture reference,
  不是 product identity。README 第一屏三例:裸 REPL、`luna build.lua`、
  `luna -e 'require("http").serve(...)'`,加一句
  "Lua with a REPL, batteries-included standard library, Node-style
  modules, plugins, package management and opt-in async I/O"。
- **三层模型作为文档架构主结构**:User Space(script/REPL/CLI/app)/
  Luna Runtime(modules/stdlib/plugins/loop/package/tooling)/ Native
  Runtime(Lua 5.5/libuv/OpenSSL/replxx/LPeg)。现有"Node.js 对照"叙述
  保留作 rationale,不充当身份。
- **标准库四类分法**(文档分组与命名建议):Core(json/fs/path/net/
  http/crypto/process/os/loop)、Data-Format(csv/ini/toml/yaml/xml/
  zlib)、Dev tooling(logging/repl/introspect/magic/complete/highlight)、
  Ecosystem(LuaRocks/plugins/luna_modules)。防"什么都往里塞"。
- **P0-P4 roadmap(已解挂,转正式任务序见下节)**:P0 稳定 Runtime 核心(现状即
  P0,不新增功能);P1 runtime introspection(attach 升级:luna ps /
  %info / %modules / %plugins / %tasks / %gc / %globals / %stats /
  %eval / %load,评审认为 attach 是最被低估的一条线);P2 异步模型
  (coroutine/future/await/cancellation/handle lifetime,**明确"不要
  继续加 loop.xxx"**);P3 生态(plugin/package/template + 官方示例
  hello-cli/http-server/tcp-server/file-tool/web-scraper/build-tool/
  game-script/automation);P4 安全运行时(sandbox/permissions/module
  allowlist/resource limits,trusted vs sandbox 双模)。
- **插件系统升级方向**:REPL 扩展机制 → 生态扩展机制(luna-plugin-* /
  `luna git status` 形态);manifest/failure isolation/override 现设计
  保留。
- **不耦合游戏服务器**:luna 保持通用,游戏场景经 require("game")
  或 plugin 接入;不因任何业务域引入 ECS/Actor/Zone 概念。
- 评审认可点(维持不改):不造 Lua、C 层尽量薄、同步标准库不被事件
  循环污染(require "loop" 才进异步)、LuaRocks 只包装不自建 registry、
  luna.lock 走复制锁(lock/package/ux 三者职责分离)。
- 巡检注记(第十四轮,2026-09-28,基线 59b7db4):全量 15 组复跑——build 树 1026.7s(loop 322.9s 过新 600s 门,ASan 树同套 193 用例 578s 零泄漏)、cov 树 589s 15/15 绿;覆盖率门禁 lines 95.2%(2610/2743)、luna_loop 分支 76.19%(739/970)、聚合 76.3%(905/1186);本轮唯一修复:loop 组 TIMEOUT 属性 240→600(显式属性压过 ctest --timeout,59b7db4);rocks 组负载 87-107 三连败(2×Timeout+1×Failed,内层 timeout 420 掐网络重锁腿,负载假挂类),负载 38 复跑绿 253.6s,同轮 cov 树曾绿证非回归;工作树含并行会话 +76 行(test/luna_test_loop.c,6 测试,199 用例态同轮全绿)未纳入提交,gcov 临时产物已清。

## 评审计划解挂:正式任务序(2026-10-04 派发生效)

上节登记的架构评审由用户派发解除挂起,P0-P4 转正式任务序,按序开工,
每完成可验收增量提交推送。原登记块保留作源记录,与本节冲突处以下节
及各增量记录为准。

**执行序**(状态:☑ 完成 / ▶ 进行中 / ☐ 未动):

- ☑ **①文档/定位批**——README 第一屏三例(裸 REPL / `luna build.lua` /
  `luna -e 'require("http").serve(...)'`)+ 定位句
  "A batteries-included Lua runtime for scripting, tooling, and
  lightweight services" + 三层模型文档主结构 + 标准库四类分法
  (Core / Data-Format / Dev tooling / Ecosystem),Node.js 降为
  architecture reference 不作产品身份;顺带核销安装面过时文案
  (README"无 tag 无 Release"/faq:39/getting-started:21,匿名
  Release 已通,原文反向);
- ☑ **②P0 稳定 Runtime 核心**——不新增功能,守现状 + 巡检:门禁
  全绿即 P0 达标,巡检记录随增量落本节;
- ☑ **③P1 runtime introspection**——attach 线升级:`luna ps` +
  %info / %modules / %plugins(已有) / %tasks / %gc / %globals /
  %stats / %eval / %load;%tasks 的数据源是 P2 任务模型,随 P2 落地
  回补(2026-10-05,见增量账 P2 条);
- ☑ **P2 异步模型**——require "task":task.run/await/sleep/
  promise/promisify + 句柄 :cancel(协作式)+ %tasks;红线守住:
  零新 loop.xxx;唯一 C 改动是修 luna_loop.c 既有崩溃(回调落
  创建线程→挂起协程上 pcall 即坏,现落驱动态 g_L,与 attach 轮询
  同款),本地 15/15 组全绿含 loop;
- ☑ **P3 生态**——`luna new plugin/package/script` 内置脚手架(嵌
  策略层,拒覆盖 + 名字校验 + --list)+ `examples/` 八官方示例
  全真实走查;顺带修 net.serve 吃 ^C 的真实缺陷(accept 永阻塞,
  "^C 即停/130" 承诺落空 → 250ms 期限 + count_hook 转身),
  automation 例补 http.serve 同款心跳,loop.md ^C 条目补齐前提;
- ☑ **P4 安全运行时**——`luna --sandbox`(-S)同一 Lua 态四层加固:
  模块白名单(全局 require 卫兵 + `LUNA_SANDBOX_MODULES` 按名放行,
  `package.require` 实测不存在故全局替换即全覆盖)、禁原生加载
  (loadlib 拿掉 + package 掏空冻结——`__newindex` 只拦新键,裸赋
  cpath 会绕过,改「冻结副本 + `__index` + `__metatable` 锁」)、权
  限(动作类响亮拒绝 / 读环境返 nil 不炸,load 强制文本,debug 上值
  与帧局部探测拒绝、getinfo 留给内省)、资源限额(kernel.fuel /
  memcap 由 `LUNA_SANDBOX_FUEL`/`LUNA_SANDBOX_MEM` 驱动后摘表);
  信任模式零行为变化(新 allocator cap=0 逐字节等价);attach 通道
  保留且灌入行仍受卫兵,插件跳过;本地 15/15 组全绿。

**红线(全程有效)**:不耦合游戏服务器(禁 ECS/Actor/Zone,游戏场景
经 require("game") 或 plugin 接入);LuaRocks 只包装不自建 registry;
C 层尽量薄;同步标准库不被事件循环污染(require "loop" 才进异步)。

**增量账**:

- 2026-10-07 Windows CI 运行期修复(续上条;run 37540571704=5efc3cb
  实录:**ubuntu success**,windows 编链全通(守护手术生效)后唯一红
  组=modules 9 例,三类根因):①lua/luna/modules.lua 的 caller_dir
  只按 "/" 切分——Windows argv 反斜杠路径切不出目录,且 "D:/x" 不以
  "/" 开头被误判相对、再拼 cwd 成垃圾根 → bare require 向上走查必空
  (walk-up 六例同根:file_dir/manifest_main/walks_up/subpath×2/
  loaded_caches);修=source 先归一 \\→/,盘符(^%a:/)与 UNC(//)
  计为绝对,parent() 同步归一。②fs.readdir 用例的 os.tmpname() 在
  Windows 是 tmpnam 式带尾点根相对名,Win32 拒绝在其下 mkdir(且
  289 行同款用例在 CI 磁盘根扔了临时物)→ 改 lfs.currentdir() 锚定
  + 预清理。③MSVC 的 %p 渲染无 0x 前缀(function: 00A41208),测试
  四处硬编码 "0x"(util format %j 回退、inspect thread/function 两
  族,含一处前序断言失败后未跑到的潜伏位点)改配稳定子串。修复
  ccbe17b 推送;本地 ctest -E rocks 15/15(58.7s,modules 91/91 直
  跑)+ mingw -D_WIN32 语法验证;windows 腿以新 run 核销(本轮教训:
  门禁前 grep "error" 计数当过构建健康信号,实测构建失败被旧二进制
  假绿掩盖——**门禁必看退出码**,已纠正)。
- 2026-10-07 Windows CI 腿修复轮(CMake@main 红链
  37519918175→37523518248,用户点火令"修到绿为止,以 CI 实测为准"):
  ①run 37523518248(6fa5903)取证:编译层已过(mode_t/S_IS* 那修
  生效),新断点=链接层——luna.exe/luna_test_main.exe LNK2019
  `__imp_replxx_*` ×10。根因:replxx.h 在 _WIN32 且未定义
  REPLXX_STATIC 时把 API 声成 `__declspec(dllimport)`,消费者
  (luna_line.obj)引用 `__imp_` 导入 thunk 而 STATIC 库从不产生
  该符号(库自身同装饰下另有 C4273 inconsistent dll linkage)。
  本地 mingw 链接级双臂复现:无 define 消费者 `U __imp_replxx_*`
  →ld undefined reference(与 CI 错误逐字同构),有 define 链接
  通过;win_write(terminal.cxx 的 _WIN32 依赖)要求 windows.cxx
  回源列表,否则修完 dllimport 后 replxx 自身 LNK2019——修复
  (REPLXX_STATIC PUBLIC + 恢复 windows.cxx,撤 9e306e5 的历史排除)
  由并行会话落为 27747d4 推送;Linux 14/14 组绿,loop 组同 C 树有
  6fa5903 提交时的 15/15 实录(当日两次 loop 红=宿主负载 55–86 的
  flake,两轮挂点不同互证)。②74fe229(并行会话)以 109 例新世系
  整体替换 test/luna_test_loop.c(-4173/+775),Windows 守护层
  (文件头注/头分流/SIGPIPE 守卫/整组 #ifndef + 10 例冒烟 #else)
  随之丢失——Windows 腿在测试编译层必炸;同笔混入 2 根 TRACE
  fprintf 调试桩进 src/luna_loop.c(on_server_closed/
  on_tserver_closed)。本轮修复:新世系上重做同构手术(理由注记按
  新世系更新:/tmp 落盘的 fs 与 fs.watch 家族、SIGUSR2 递送、
  /bin/sh process 助手、pthread/裸 socket/TLS 面全 POSIX),剥
  TRACE ×2;双臂语法验证(Linux 原生 + mingw -D_WIN32)全绿后全量
  门禁。③排队 run 37537062661(27747d4)预计仍在测试编译层红
  (其树无守护),本笔推送后以新 run 双腿核销。
- 2026-10-07 Windows CI 运行期修复·二(run 37544942908=ccbe17b 实录:
  windows 腿 9 红→1 红,唯一幸存=modules 组
  test_fs_readdir_sort_and_error_paths,Lua 片段第 11 行断言)。
  根因=产品级平台分歧:lfs.dir 构造器(dir_iter_factory)在 POSIX
  侧 opendir 失败即 luaL_error "cannot open %s: %s"(lfs.c:713),
  _WIN32 侧构造器永不失败(只拼 pattern、hFile=0,lfs.c:704-709),
  失败推迟到迭代器 _findfirst==-1L 时返回 nil+strerror——generic-for
  把 nil 读成遍历结束 → fs.readdirSync(不存在路径)在 Windows 返回
  空表(静默成功),pcall 不炸,断言 `not okr` 必挂。修复=产品面
  lua/modules/fs/init.lua 的 readdirSync 入口先验 lfs.attributes
  mode~="directory" 即 error "cannot open",两平台消息同构、行为
  归一(examples 的 build-tool 是唯一调用方,无静默依赖)。落盘链
  核实:CMakeLists 的 file(COPY) 是 configure 期 + CONFIGURE_DEPENDS,
  改动自动触发 reconfigure 进 build 树(diff 校验 STAGED_COPY_IN_SYNC)。
  本地 ctest -E rocks 15/15(70.4s,负载 30;构建退出码显式核验
  BUILD_RC=0);外来会话 ctest 撞车窗口按协调规则轮询避让 20s。
  **终局核销:run 37549008691(354050d)双腿 success**(windows 214s
  / ubuntu 141s)——CMake@main 红链 37523518248→37540571704→
  37544942908→37549008691 全程闭环,Windows 腿修复收官。
- 2026-10-04 ①文档批(e7a65e7e 前身 78add60):README 定位句/三例/
  Node 降参考 + docs 三层模型主结构 + stdlib 四类分法(总览页分节 +
  sidebar 分组)+ 安装面文案核销(README:66-68/faq:39/getting-started:
  21 的"无 tag 无 Release"过时语全部翻新为滚动 nightly Release
  口径);VitePress 本地 build 12.8s 过;
- 2026-10-04 ③P1(f2bec84):kernel.alive(kill(pid,0) 探针,Windows
  下恒 false 有注释)+ kernel.started(启动锚点);`luna ps`(扫
  $LUNA_SOCK_DIR,lfs.dir 经 pcall 拆分 (iter,state) 对后 state 要
  显式回传——踩坑实录见 serve.lua 注释,for 协议缺 state 报
  "directory metatable expected");七个魔法 %info/%modules/%gc/
  %stats/%globals/%eval/%load,REPL 与 attach 同一执行面,%load 按
  __attach 换挡(REPL 缓冲、attach 直跑);实测:pty 目标全链 attach
  绿、luna ps live/stale 正确、12 门禁组本地全绿(loop/rocks 组拖
  CI 全量门);
- 2026-10-04 ②P0:本轮无 P0 专属代码——现状即 P0,守门禁:上述
  12 组 + CI 全量(含 loop/rocks)为 P0 巡检口径,未做新增功能;
- 2026-10-04 CI 核销(①③②链 e7a5e7e→b8e14fb):cmake.yml
  run 37181792774(e7a5e7e)success、run 37182599811(b8e14fb 最终树)
  success,含 loop/rocks 全量组——P0 巡检全绿;docs 部署
  (37181976485/37182548831)为 Pages 发布,不计门禁,与每日
  schedule 的 Daily Build(37184473423)同受 pool 拥堵,不阻塞;
- 2026-10-05 跨平台修(_WIN32,用户点名 src/test include 面,冻结项 4
  同批解冻):六文件。src/luna_loop.c 头部分流——_WIN32 收
  winsock2.h+ws2tcpip.h(ntohs/htons/addrinfo 同义)、自补 S_ISLNK
  宏(MSVC sys/stat.h 没有,libuv Windows lstat 照填 S_IFLNK 位),
  arpa/inet/signal/sys/wait 收进 #ifndef;fcntl/netdb 条件化;TLS
  手写层(1179 行,raw fd/fcntl/errno 全 POSIX 语义)双条件
  `LUNA_LOOP_HAVE_OPENSSL && !_WIN32` 整层编出,Windows 下
  connectTls 走既有「无 TLS」降级(独立移植另议);execSync 换
  _popen/_pclose,退出码即 wait status(WIFEXITED 折叠是 POSIX 专属,
  Windows 分支直接取值);l_signal 预留位 Windows 仅 SIGINT,信号名表
  6 项(MSVC signal.h 无 SIGUSR1/SIGPIPE/HUP/CHLD…);os.type 改
  uv_os_uname 摘掉 sys/utsname.h。src/luna_main.c:unistd.h 守护,
  main() 的 sigaction/SIGPIPE 改 Windows 直 signal(SIGINT),
  install_sigusr1 整体编出(SIGUSR1 不存在)。test/luna_test_loop.c:
  可移植头/POSIX 头分流,rlimit+fd 注入块、SIGPIPE/setup/teardown
  段守护,POSIX 例组 203 例(pthreads 回显与 TLS 服务、AF_UNIX、
  rlimit 探针、/tmp 落盘、信号语义)整体 #ifndef 编出;#else 留 10
  例纯 libuv 冒烟组(定时器/立即/stop/unref/prepare/^C,与 POSIX
  同名同体)+ 独立 main,文件头注明「CMake 从未在 Windows 排除测试
  (ENABLE_UNIT_TESTS 默认 ON),不能靠空组假绿,故留可跑冒烟」。
  test/luna_test_main.c:CRT 宏映射 setenv/unsetenv→_putenv_s。
  .github/workflows/cmake.yml:matrix 加 windows-latest 腿——vcpkg
  zlib:x64-windows-static + vcpkg toolchain,build/ctest 带 -C
  (单配置生成器);POSIX harness 各组(cli/rocks/covsum/line/linedit/
  serve)Windows 编空组空过、loop 跑冒烟组、其余七组全量(注释写明)。
  跳过面与理由:上述六组是 forkpty/fork/waitpid/poll 专属 harness,
  等价实现=重写测试本体,按「写明理由整体 #ifdef」处置;TLS 层属
  产品面推迟(降级可用),不算测试跳过。验证:全仓
  x86_64-w64-mingw32-gcc 交叉编译 src 8/8 + test 16/16 绿;Linux
  重建 + ctest -E rocks 15/15 全绿。**Windows CI 实跑(run
  37519918175,Ubuntu 腿绿)**:首层即 MSBuild 下界兑现——
  luna_loop.c:549 `mode_t` 未声明(MinGW 有 mode_t,本地交叉编译
  抓不到;正是 2026-10-03 登记「修完上述后可能再浮出」的那层)。
  取 UCRT sys/stat.h 原文核对,实锤三缺口:无 mode_t、无任何 S_IS*
  谓词(luna_loop.c:448 旧注「MSVC 拼法相同」是错的)、连 S_IFLNK
  都不声明(旧 S_IF* 别名全在 _CRT_INTERNAL_NONSTDC_NAMES 后)。
  修法:mode_t→uint64_t 直取 uv_stat_t.st_mode;sys/stat.h include
  **之后**补 #ifndef 兜底块(S_IFMT/S_IFREG/S_IFDIR/S_IFLNK +
  S_ISREG/S_ISDIR/S_ISLNK,include 后定义避开与 UCRT 旧别名重定义);
  头部原孤立 S_ISLNK 兜底移除(宏展开点在 448 行 include 之后,
  孤立定义保证不了 S_IFLNK 到场)。全量敌意面清扫(MSVC 未测面
  luna_loop/luna_main/luna_line/luna_test_loop/luna_test_main 主体):sig 名表/保留位/sigaction/SIGPIPE/
  readlink/unistd/fork 面全在守卫内,strcasecmp/strdup/VLA/复合
  字面量/指定初始化零命中,ssize_t 由 uv/win.h `typedef intptr_t
  ssize_t` 供应,冒烟分支链接面(eval_string:73、setup_loop:245、
  teardown 的 rlimit 变量)全在可移植区或 #ifndef 内;15/15 复绿
  后推进,Windows 腿结果随下轮 CI 核销。
- 2026-10-05 P4 安全运行时:进程级 `luna --sandbox`(-S)同一 Lua 态四层
  加固。C 侧 `lua_newstate(luna_alloc, NULL, luaL_makeseed(NULL))` 定制
  分配器(收缩回收的双向字节账,上限 0 逐字节等价默认)+ 计数钩子扣
  fuel + `kernel.fuel(strikes)`、`kernel.memcap(bytes)` 导出;装填层
  lua/luna/sandbox.lua(白名单卫兵/冻结/剥旋钮/横幅)沿五触点嵌入,
  luna.lua `-S` 派发预载 linedit+luna.serve、跳过插件。走查实录:
  `__newindex` 只拦新键致 cpath 裸赋值绕过→改「掏空+冻结副本+
  __index+__metatable」;`debug.getlocal` 抽卫兵帧实测够不到上值
  (getupvalue 已关)仍一并关闭,getinfo 留给内省;attach 灌入行照受
  拒绝,fs 放行后写面仍拒;LUNA_SANDBOX_FUEL/MEM 限额报错到位;
  luna/serve.lua 快照 SOCK_DIR 与 os.remove 供安装后调用。文档:
  guide/sandbox.md 新页 + 侧栏/cli-repl/kernel(限额节)/README 挂载;
  ctest -E rocks 15/15。
- 2026-10-05 P3 生态:①脚手架 `luna new`(lua/luna/new.lua 走
  __LUNA_*_SRC 嵌入路径;plugin/package/script 三模板,名字校验禁
  `..`、目标已存在即拒、--list;epilog 补行,luna.lua 与 rocks/
  serve/ps 同款截获分发);②examples/ 八例(hello-cli 用 `--`
  分隔脚本旗标——入口 argparse 先吃旗标;http-server;tcp-server;
  file-tool;web-scraper 仅 http://,离线配 luna serve 走查;
  build-tool csv→json 流水带计时与单段运行;game-script 表驱动
  + json 存档,无 ECS/Actor/Zone 红线内;automation fs.watch)
  全例真实走查(一次性四例 stdout 全对,常驻三例 ^C 全 130);
  ③顺带修 net.serve 真缺陷:accept 永阻塞使 README/net.md 的
  "^C 即停/退出码 130" 落空(实测 137 = 靠 -k SIGKILL 才死)——
  master settimeout(0.25) + kernel.count_hook 定期转身,已存在的
  "Ctrl-C stops it too" 注释由假转真;handler 拿到的 client 超时
  不受影响(慢客户端 1s 后照常回话实测);loop.md ^C 条目补
  "静默循环不转 EINTR" 前提(http.serve 心跳注释的公开化),
  automation 例补 100ms 心跳同口径;④docs:guide/examples.md 新页
  (示例与脚手架,八例表格 + 五段真实抓取会话 + luna new 契约)
  + 侧栏 + cli-repl 启动模式行 + plugins/modules 生成器指路 +
  README 工具集两行与文档 bullet 补齐;Feature-first 零新测试;
- 2026-10-05 P2 异步模型:lua/modules/task/init.lua(AWAIT 哨兵
  yield 协议,裸 coroutine.yield 记任务错误;promise 一次性结算,
  双 settle 抛错;cancel 协作式,CANCELLED 落 await 点可被捕获,
  取消先 clearTimeout;rec_of 弱键侧表,句柄外表不漏内部)+
  %tasks(被动读 package.loaded.task,不自动加载)+ loop 回调
  落点修复(box_call/on_closed 从创建线程改驱动态 g_L;协程里
  setTimeout 的 pcall 打到挂起线程会 SEGV,实测 T6/T8 复现、
  修复后六腿走查全绿:sleep/results/取消/promisify/裸 yield/
  双 settle/拒绝/%tasks 双态/排水序);docs:guide/tasks.md 新页
  + 侧栏 + cli-repl %tasks 行(缓议句清除)+ stdlib 总览异步面
  句 + README 工具集行 + loop.md 交叉引用;门禁:本地 15/15 组
  224.9s 全绿(含 loop 组,C 改动故本地必跑;rocks 网络腿留 CI);
  实测脚本尾部排水与 deadline 唤醒序(a=20ms 先于 b=60ms);
  Feature-first 零新测试;
  期间取消两个被最终树覆盖的冗余 run(37181976479/37182548801)
  让出队列。
- 2026-10-04 ④全局对账批(文档 vs 实现,全量扫):偏差清单逐条修——
  交付面三处滞后:README 徽章/链接与每日构建 bullet 指向旧流水线
  daily.yml(纯 artifact)而非 daily-build.yml(滚动 nightly 交付线),
  bullet 里"每天定时构建+全量测试"与"两者均红"按实际矩阵改写
  (Release 构建+冒烟,纯产物流水线,全量测试归 push CI;仅 Windows
  是试运行腿),artifact 保留期钉准(pkg-<os>-<arch> 3 天、汇总
  daily-build 14 天);build.md 的"CI:Daily Build"整节还是
  "不打 tag、不发 Release"旧口径,照 daily-build.yml 重写,deps 清单
  补 lualogging、tomlc17 版本钉 v1.1、base64/jemalloc 未编入注明;
  P1 滞后两处:kernel.md API 表补 kernel.alive/kernel.started
  (P1 f2bec84 引入未入档),config.md 命令行开关表补
  -i/--interactive、-e/--eval、-v/--version 三行与子命令注;其余对账
  修正:architecture.md "事件循环为何推迟"旧框架→"边界在哪"、
  计数钩子 20 万→10 万(对齐 luna_kernel.c:373),introduction.md
  删定位批残留的重复句,getting-started deps 列表补五个已发布依赖+
  通用开关表补 --no-serve,stdlib/index.md 上游链接钉准
  (lua-zlib brimworks、lyaml gvvaughan)+ 死锚 #分层→页内三层模型,
  toml 三处"TOML 1.0"→"TOML v1.1"(tomlc17 README 口径),rocks.md
  "约 400 行"→"约 500 行",faq utf8 不算"额外内置",node-parity
  里"README 表述滞后"一句按现状收口;核对通过不动:loop.md(逐行
  对过 C 导出表)、plugins.md(四扩展点+overridden/failed 全真)、
  modules.md 净表(15 模块 API 声称脚本化核对全命中)、logging.md
  (deps/lualogging 上游九 appender 全在)、errors.md、docs/index.md;
  VitePress 本地 build 过。
