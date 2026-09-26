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

## 下轮方向

- **`luna_loop.c` 分支**:事件循环分支 51%,多为 libuv 回调的错误/超时路径,
  需要针对性的失败注入(打不开的文件、断开的连接)而非新特性;
- **serve.lua 的 attach 捕获路径**:39.84% 的缺口集中在 attach 数据汇,需要
  对调度顺序不敏感的断言方式(轮询至静默而非定时快照)。

## 明确不做(上一轮)

- 未闭合引号跨行续行的语义修补(`s = "abc` 回车后无解)——`^C` 丢弃整块已可用,改语义
  动 `kernel.check` 契约,收益低;
- 包管理(luna.rocks)与 loop 面扩展——已有专文与测试,不在本轮范围;
- 打 tag / 发版(硬约束禁止)。
