# todo

需求清单。一条一句,完成项保留一行证据(commit 加测试数)。

## 待办

- [ ] 双每日管线(daily 加 nightly 滚动 Release)在下列任一条件成立时重评:发布职能归回 daily、nightly 交付通道更换、双构建配置漂移造成事故。

## 已完成

- [x] Node assert 模块(纯 Lua,deepEqual 与 deepStrictEqual 与 strictEqual 三口径)。5 组形状对拍,modules 107

- [x] util.parseArgs(Node v24.21.0 差分对拍,长/短/内联/`--`/strict/位置参/tokens)。5 组形状对拍,modules 108,3caedde

- [x] REPL 寄存器与时间魔法:In 记录、%time 与 %timeit 支持语句、%plugins 报遮蔽。ce21660,repl 4 例,magic 2 例
- [x] 错误提示与补全:did-you-mean 建议、require 目标 Tab 补全、luna_modules 包内子模块。8527d80,complete 6 例,modules 3 例
- [x] 标准库打磨批:fs 便捷层、%hist 范围、文档站同步、覆盖率接线。repl 21,magic 14,complete 21,modules 18,C 侧行 75.8%
- [x] replxx 行编辑桥:修 ^C 当 EOF、--no-color 不降级、Tab 吞前文、highlighter 空指针。line 17 例,linedit 29 例,luna_line.c 100%
- [x] 信号语义与可达性:^C 退出码 130、attach 失败面报错、错误对象与续行启发式、入口失败测试、插件清单失败原因。cli 5 例,serve 2 例,repl 4 例,kernel 98%
- [x] 覆盖率接线:luacov 接入嵌入式源,修 caller_dir 与 DNS 假绿。Lua 侧 84.14%
- [x] luna_loop 失败注入:修 server_close 句柄泄漏并补注入用例。loop 组 112 例,C 侧行 85.0%
- [x] attach 捕获路径:修 sethook 搁浅包装器与 magic.dispatch 吞错。serve 组 26 例,serve.lua 100%
- [x] magic 与入口覆盖率补齐,C 侧记账更正。magic 100%,luna 95.27%,highlight 100%
- [x] loop 长尾与 modules 与 plugins 边角:29 个新 C 测试,五个真产品 bug 由测试逼出。modules 111,plugins 88
- [x] line 与 main 余量及 http.lua 专项:修 redirect 吞错与 set_highlighter 空参段错误。http.lua 96.59%
- [x] Lua 侧长尾:repl 100%,luna 96%,rocks 92%,Lua 总 97.82%
- [x] C 侧 loop 覆盖:根因修 proc exec 失败句柄泄漏与 tsock ref 越界。loop 95%,ASan 186 例零报错
- [x] loop 暗行逐腿表征:107 条不可达或边缘分支逐条分类登记
- [x] tsock 结构体泄漏:回收 __gc 释放路径,顺带修无读回调热旋。ASan 189 例两轮零泄漏
- [x] loop 分支覆盖专项:补 OOM 守卫与参数契约面。分支 76.19%,193 例
- [x] 行编辑分支方向清扫:补 7 例 pty 测试。line 97.14%,分支 84.29%
- [x] 覆盖率汇总器:补 covsum 测试组锁 Lua 覆盖率输出。ctest 16 组,C 侧行 95.2%
- [x] C 侧分支方向清扫:kernel 分支 93.75%,main 19 条暗方向逐腿核对。cli 29 例,line 41 例
- [x] Node 方向立项:落选型文档与决策记录(tomlc17、timers 排水、不遮蔽 os、LPeg 格式面、手写 C 绑定)。6 条决策记录
- [x] csv 与 ini 批次(LPeg 实现):修覆盖率钩子饿死与 quote_string 漏逃两个真 bug。modules 37,ini 90.85%,a9fcf45
- [x] toml 批次(tomlc17 钉 R260821):修子表 keypath 拼装 bug。modules 45,toml 90.69%,085c4a6
- [x] yaml 批次(libyaml 0.2.5 加 lyaml v6.2.9,零补丁 5.5 适配)。modules 60,eefc2e0
- [x] xml 批次(expat R_2_8_5 加 lua-expat 1.5.2):DOM 三件套加 encode 加 sax。modules 69,d785ffe
- [x] path 与 util 与 events 批次(Node v24.21.0 实证钉版):修 resolve 尾斜杠等三处。modules 83,31ffd58
- [x] yaml 自动锚点(opts.anchors)。modules 92,95d2c9e
- [x] Windows TLS 移植:connectTls 与 listenTls 在 Windows 真跑,CI 加双腿。c8b084f,07c24eb,970ad95
- [x] path.win32 面解冻:全函数面加 posix relative 重写。326 例差分全绿,ddf2bbb
- [x] util.format 零参返空串。18 例差分,1241063
- [x] querystring 与 url legacy 面移植。168 例差分,modules 104,c211eb8
- [x] process 模块(Node 同步子集)。29 项形状对拍,modules 105,dec1275
- [x] console 模块(自建 ASCII 网格渲染器)。70+ 例形状对拍,modules 106,3a468f5
- [x] stream 批次(纯 Lua 流类加三适配器)。modules 91,loop 195,e4abf85
- [x] os 与 child_process 与 timers 批次:loop.os 补五件、exec 与 execSync、脚本尾部自动排水。loop 199,7622df0
- [x] lualogging v1.8.2 集成。6819041
- [x] REPL 定时面与行编辑定时唤醒。2b7598f,d0652cb,488eb8b
- [x] net 多地址回退(拨号留链、全败才报错)。3caea1f,99b46a6
- [x] net.serve 一行阻塞服务。d7b3850
- [x] P1 运行时内省:luna ps 加 %info %modules %gc %stats %globals %eval %load。f2bec84
- [x] P2 异步任务:require task 的 run await sleep promise promisify。8b658af
- [x] P3 生态:luna new 三模板加 examples 八官方示例。188dc7e
- [x] P4 安全运行时:luna --sandbox 四层加固。9b47407
- [x] 文档定位批:产品定位改 batteries-included runtime、三层模型主结构、标准库四类分法。78add60
- [x] Windows CI 全链路:编译阻碍五轮、守护层重做、平台硬编码修平、replxx 静态链接。f466d73,74fe229,5efc3cb,ccbe17b,27747d4,6fa5903
- [x] 全局文档对账批:文档与实现偏差逐条修。d2980e1
- [x] macOS 转正与平台修:SUN_LEN、sock_addr 竞态、fs.watch 首监视器、NPROC 注入、目录自身事件。e029abf,c557baf,2dfbcce,6edf2d2,d468676,c9987e7,5198b54
- [x] Windows 转正与 luna_lua.h 根修。12c90bd,7609204
- [x] 滚动 nightly Release 交付面与 install.sh 匿名直拉。8377aed,ce18fbf,08d2d07
- [x] line 套件 CI 连红修复:uv loop time 陈旧、幻影哨兵、^D 落 canonical 窗口三层根因。7ee4ca0
- [x] line 套件 macOS 根因:replxx 零读空行 commit 上游缺陷,改为 vendored 补丁。CI macos 16 组
- [x] 覆盖率树 repl 空转修复:暴露 request_interrupt 给 Lua、中断注入移进 chunk 内。ea3af30,dcbd5be,cov 树 15 组
- [x] line 套件 macOS 间歇挂根因:canonical 窗口 LF 不提交,补确定性回归测试。f82e46e,adf84e8
- [x] 文档与源码一致性审计:36 项差异登记入表,全部改文档侧修复,码侧零改动。docs/审计-文档一致性.md

## 已裁定的不做

- XML XPath 与 DTD 验证、libxml2 全家桶。
- YAML schema 与 tags 高级面。
- stream webstreams、异步迭代器、setEncoding、cork。
- util promisify 与 callbackify、types 判等族。
- WHATWG URL 全套,取 legacy url 面。
- 为覆盖率余量写无价值用例:rocks 20 行、luna 6 行、introspect 241 行、http.lua 13 行、loop 193 行,已逐腿表征。
- 未闭合引号跨行续行不修,属语言语义边界。
- Inspector 与颜色面,profile 与 timeStamp 恒 no-op。
- 多进程 cluster,用 loop.process 自组。
- Buffer 类,用 string 与 string.pack。

## 硬约束

- 不打版本号 tag,不建正式 GitHub Release,不跑正式发版 workflow,版本号只由用户手动给出。
- 每日构建与滚动 nightly 二进制照常。
- 未经验收测试通过的版本不得对外发布,不得出现在 Releases 页面。
