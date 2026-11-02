# todo

需求清单。一条一句，完成项保留一行证据（commit 加测试数）。

## 待办

- [x] en 文档补齐(2026-11-02):33 个 en 页全数落库(guide 11、stdlib 17、other 4、node-parity 1);6 页 guide 工作树 WIP 先核完整后按批提交,余 27 页按审计表 A1 顺序分三批提交(guide 5→stdlib 17→other 4+node-parity 1),每批 docs build 绿后单 commit+push(禁 force push);最后补齐 EN sidebar(标准库四类分法/Other/Design 全链)并归一化全部跨页链接为 en 目标。README/sidebar 链接自此自然可达。5897506,840762e,17b7196,8438d38,e293366
- [x] util.parseEnv 引号多行值跳行死代码:lua/modules/util/init.lua:701 的跨行跳过被 :714 无条件覆盖,续行被重解析为 key=value(如 'A="x\ny=z"\nB=1' 多出 y='z"')。P2,码审 Finding 1。已修:底部回退改为仅当 i<=nl 时推进,跨行跳过不再被覆盖;新增 2 例跨行引号含 = 回归。lua/modules/util/init.lua:700,test/luna_test_modules.c:3550
- [ ] serve.ps 读 /proc/<pid>/cmdline 无 nil 检查:lua/luna/serve.lua:285 `fh:read("a")` 结果直接 gsub,读失败(pid 回收/IO 错)抛"attempt to index a nil value"。P3,码审 Finding 3
- [ ] attach 命令的 kernel.exec 未包 pcall:lua/luna/serve.lua:181-185,目标 stdout 断管时错误越过 kernel.sink(nil),捕获 sink 滞留,后续输出被吞。P3,码审 Finding 4
- [ ] attach 会话 inputs 表永不裁剪:lua/luna/serve.lua:136,进程存活期随每条命令增长。P3,码审 Finding 5
- [ ] stream _want_read 同步紧循环:lua/modules/stream/init.lua:232-253,常就绪同步源每推即耗时不让出,饿死定时器与中断钩子。P3,码审 Finding 6
- [ ] %hist 范围不封顶:lua/luna/magic.lua:150,`%hist 1-99999999999` 挂起会话。P3,码审 Finding 7
- [ ] rocks.lua 路径穿越:lua/luna/rocks.lua:259-262 cache_file 以 name/version 拼路径(name 可含 `../`),:299 os.remove 可删任意路径,:80 sha256_file io.open 可读任意文件(触发面 `luna install --from-lock`)。P2,安全审 F2
- [ ] http.serve Windows 路径穿越:lua/modules/http/init.lua:196-203 只挡 `..` 段,POSIX 有效但 Windows 反斜杠分隔可绕过,静态文件面可越界读。P2,安全审 F3
- [ ] l_net_connect OOM 泄漏:src/luna_loop.c:1600-1603 malloc(resolver) 失败走 luaL_error,但 sock_new 已 pin selfref/keepalive/connectref,longjmp 后三者永不释放,keepalive 计数滞留。P2(仅 OOM 触发),码审 C3
- [ ] l_sock_write 长写截断:src/luna_loop.c:1671 `uv_buf_init(copy, (unsigned)len)`,len>UINT_MAX(4GiB) 时写长截断,>4GiB 数据静默不发送。P3,码审 C4
- [ ] prepend_module_dir 路径截断:src/luna_main.c:128-131 `snprintf(merged, 10240, "%s%s", add, cur)`,package.path 超长(用户 LUA_PATH)时截断破坏路径(非崩溃)。P3,码审 C5
- [ ] _FORTIFY_SOURCE 未开:编译面仅 -fstack-protector-strong,_FORTIFY_SOURCE=2 缺失(需 -O2 且 glibc)。P3,质量门

## 已完成

- [x] 文档-源码一致性审计(2026-11-02):14 项差异登记(桶一 1/桶二 2/桶三 11),13 项已修全部文档侧、码侧零改动,A1(27 个 en 页缺)待翻译。docs build 绿
- [x] 沙箱逃逸封闭(2026-11-02):P0——loop/socket.core/socket.unix/mime.core/_openssl.* 出基线并清 package.loaded,lfs 硬化(禁 chdir/link/lock/mkdir/rmdir/rename/setmode/touch/unlock),kernel.exec/check 拒二进制块,kernel.chmod 拒绝。f6fceb6,cli 10 例沙箱组
- [x] loop.http 请求体封顶(2026-11-02):P1——Content-Length 与流式体双双限界(默认 16 MiB,http.listen opts.max_body 可调),超限 413;头只解析一次,去 O(n²) 重扫。63b4e79,loop 110
- [x] SSL_new 空指针修(2026-11-02):P1——tsock_new 中 SSL_set_mode 在 `if (!t->ssl)` 之前被调,SSL_new 失败即解引用空指针;检查上移,失败路径 tsock_close 统一关 fd/丢 pin,accept 调用方不再重复关 fd。e8eb22f,loop 112
- [x] DNS 解析期 UAF 修(2026-11-02):P1——net.connect/connectTls 关闭-在-解析中时 on_sock_closed/on_tls_closed 立即丢 selfref,GC 先于 on_resolved 回收结构体即 UAF;改为 resolve_pending 标志,关闭回调与解析完成两者都到后才丢 pin。a113410,loop 112
- [x] Node assert 模块(纯 Lua,deepEqual 与 deepStrictEqual 与 strictEqual 三口径)。5 组形状对拍,modules 107
- [x] 双每日管线重评(2026-10-14):裁定互补保留(daily=验证门禁,daily-build=交付面),Windows 状态四份文档与 daily-build.yml 回显串同步到实态。docs build 绿,ctest 16/16
- [x] util.parseArgs(Node v24.21.0 差分对拍,长/短/内联/`--`/strict/位置参/tokens)。5 组形状对拍,modules 108,8a63b97
- [x] util 四件套:parseEnv/stripVTControlCharacters/styleText/debuglog(Node v24.21.0 差分对拍,NODE_DEBUG 条目不 trim/OSC 未闭合落 CSI/参数 4 位上限/#RGB ×17)。modules 110
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

## 审核记录

### 审核任务（只审不改）6ee7a717ec08..HEAD（2026-11-02）

- [x] 审核发现：本轮 diff 为纯文档变更（36 文件 / +4322-85，6 提交：5897506,840762e,17b7196,8438d38,e293366,16f5a76），未引入代码级安全/正确性/测试缺口。已核：config.mts 双侧栏/导航链接全部可达、locale 结构正确（构建产物无 `/zh//zh/` 双重前缀）；变更 md 交叉链接全部可达（3 处为 Lua 内联代码 `s["end"](...)` 误报）；sandbox 拒绝 `os.execute`/`io.popen`、csv/ini/toml/yaml 错误格式、`task.run/await/promisify`、`--sandbox`、`LUNA_SANDBOX_MODULES`、`examples/game-script/main.lua`、`/images/architecture-en.svg` 均与实现/仓库一致；root 页面为英文（无 locale 内容错配）；首页断链 `/architecture.en`→`/architecture` 为修复。docs build 绿,链接全达
- [x] 审核发现（低，安全提示，既有行为）：getting-started.md:19 明文记载安装脚本对 `.sha256` 不匹配「只警告不失败」（install.sh:168 一致）——被篡改/截断的下载仍会继续安装并仅告警，属于供应链加固缺口；系既有策略、非本 diff 引入。已修：install.sh/install.ps1 校验不匹配由警告升级为硬失败（die/Die），文档同步。install.sh:167,install.ps1:144,getting-started.md:19
- [x] 审核发现（低，卫生，非 diff 内容）：工作树存在空目录 `docs/zh/"docs`（含引号字符，shell 误操作遗留），未跟踪、不影响构建，建议清理。已清空目录

- 审核发现（低，文档一致性）已裁定不属实：luna 以键缺省(nil)表达 Node 的 null，Lua 无 null 字面量；loop.md:257 与 node-parity.md:485/488 均一致描述 key absent (nil)，与实现 luna_loop.c:2908 一致，无实际矛盾。
