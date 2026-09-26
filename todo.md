# todo

2026-09-25 通读 README / docs / 源码后的缺口盘点。README 宣称的能力大多已落地,本清单只记
**文档承诺了但实现缺位**、**明显值得补**与**顺手打磨**三类,按优先级排序。
约定:每完成一项必须带测试,`cmake --build` + `ctest` 全绿后才 commit/push;不打 tag、不发版。

**状态:2026-09-26 上一轮全部完成**(逐组 commit 已推送,ctest 14 组全绿);行编辑桥一轮见下。

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

## 下轮方向

- **Lua 策略层覆盖率**:gcov 看不见 Lua,`repl`/`complete`/`magic`/`introspect`
  这些全 Lua 模块要靠 luacov(或等价探针)接进 ctest 才能逼近 100%;
- **`luna_loop.c` 分支**:事件循环分支 51%,多为 libuv 回调的错误/超时路径,
  需要针对性的失败注入(打不开的文件、断开的连接)而非新特性。

## 明确不做(上一轮)

- 未闭合引号跨行续行的语义修补(`s = "abc` 回车后无解)——`^C` 丢弃整块已可用,改语义
  动 `kernel.check` 契约,收益低;
- 包管理(luna.rocks)与 loop 面扩展——已有专文与测试,不在本轮范围;
- 打 tag / 发版(硬约束禁止)。
