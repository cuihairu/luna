# todo

需求清单。一条一句，完成项保留一行证据（commit 加测试数）。

## 待办

- [x] en 文档补齐(2026-11-02):33 个 en 页全数落库(guide 11、stdlib 17、other 4、node-parity 1);6 页 guide 工作树 WIP 先核完整后按批提交,余 27 页按审计表 A1 顺序分三批提交(guide 5→stdlib 17→other 4+node-parity 1),每批 docs build 绿后单 commit+push(禁 force push);最后补齐 EN sidebar(标准库四类分法/Other/Design 全链)并归一化全部跨页链接为 en 目标。README/sidebar 链接自此自然可达。5897506,840762e,17b7196,8438d38,e293366
- [x] util.parseEnv 引号多行值跳行死代码:lua/modules/util/init.lua:701 的跨行跳过被 :714 无条件覆盖,续行被重解析为 key=value(如 'A="x\ny=z"\nB=1' 多出 y='z"')。P2,码审 Finding 1。已修:底部回退改为仅当 i<=nl 时推进,跨行跳过不再被覆盖;新增 2 例跨行引号含 = 回归。lua/modules/util/init.lua:700,test/luna_test_modules.c:3550
- [x] serve.ps 读 /proc/<pid>/cmdline 无 nil 检查:lua/luna/serve.lua:285 `fh:read("a")` 结果直接 gsub,读失败(pid 回收/IO 错)抛"attempt to index a nil value"。P3,码审 Finding 3。已修:read 结果先判 nil 再 gsub,读失败保留 cmdline nil;新增确定性回归(桩 io.open/lfs.dir 强制 read→nil)。lua/luna/serve.lua:285,test/luna_test_serve.c:254
- [x] attach 命令的 kernel.exec 未包 pcall:lua/luna/serve.lua:181-185,目标 stdout 断管时错误越过 kernel.sink(nil),捕获 sink 滞留,后续输出被吞。P3,码审 Finding 4。已修:dispatch 经 exec_pack 包 pcall 调 kernel.exec(三处:sugar/首个 eval/失败回退),raise 以 {false,msg} 上浮,cleanup 恒达 kernel.sink(nil);新增回归 test_raising_exec_does_not_strand_sink(桩 kernel.exec 抛 EPIPE,断言 ERR 帧且 sink 释放)。serve 组绿。lua/luna/serve.lua:132,test/luna_test_serve.c:223
- [x] attach 会话 inputs 表永不裁剪:lua/luna/serve.lua:136,进程存活期随每条命令增长。P3,码审 Finding 5。已修:record_input 按绝对 in_n 记录,超 MAX_INPUTS=1000 裁最旧(留空隙,%hist 跳过),in_n 恒计数;新增回归 test_attach_history_recorded_and_bounded(1002 条后窗口 1000、最旧丢弃、最新保留、in_n 连续)。serve 组绿。lua/luna/serve.lua:91,test/luna_test_serve.c
- [x] stream _want_read 同步紧循环:lua/modules/stream/init.lua:232-253,常就绪同步源每推即耗时不让出,饿死定时器与中断钩子。P3,码审 Finding 6。已修:_want_read 连续同步拉取超 SYNC_PULL_BUDGET=64 即 loop.setImmediate 让出(read 槽跨 tick 保持,_pump(false)+_want_read 续),无 loop 同步世界保持迭代;新增回归 test_stream_sync_source_yields(桩 loop.setImmediate 排队,断言过预算即 defer 且数据顺序完好)。modules/serve/loop 组绿。lua/modules/stream/init.lua:23,test/luna_test_modules.c
- [x] %hist 范围不封顶:lua/luna/magic.lua:150,`%hist 1-99999999999` 挂起会话。P3,码审 Finding 7。已修:%hist 的 lo/hi 先 clamp(lo=max(lo,1),hi=min(hi,in_n)),超顶大范围不再旋转;新增回归 test_hist_range_is_capped(%hist 1-99999999999 与 0-2 均封顶且顺序完好)。magic 组绿。lua/luna/magic.lua:152,test/luna_test_magic.c
- [x] rocks.lua 路径穿越:lua/luna/rocks.lua:259-262 cache_file 以 name/version 拼路径(name 可含 `../`),:299 os.remove 可删任意路径,:80 sha256_file io.open 可读任意文件(触发面 `luna install --from-lock`)。P2,安全审 F2。已修:新增 check_rock_component 校验 name/version 仅 [A-Za-z0-9._-]+ 且拒 `..` 与空串,cache_file/rockspec_sha 入口先校验(经 os.remove/sha256_file 的越界读删被堵);新增回归 test_from_lock_refuses_path_traversal(锁内 name=../../../../etc/passwd → 拒「unsafe name」)。rocks 组绿。lua/luna/rocks.lua:47,test/luna_test_rocks.c:296
- [x] http.serve Windows 路径穿越:lua/modules/http/init.lua:196-203 只挡 `..` 段,POSIX 有效但 Windows 反斜杠分隔可绕过,静态文件面可越界读。P2,安全审 F3。已修:解码后路径含反斜杠直接 403(Windows 视 `\` 为分隔符,`..\` 段绕过分段检查);新增回归 test_http_static_refuses_backslash_traversal(桩 loop.http.listen,断言 `/..\..\etc\passwd` → 403)。loop 组绿。lua/modules/http/init.lua:199,test/luna_test_loop.c
- [x] l_net_connect OOM 泄漏:src/luna_loop.c:1600-1603 malloc(resolver) 失败走 luaL_error,但 sock_new 已 pin selfref/keepalive/connectref,longjmp 后三者永不释放,keepalive 计数滞留。P2(仅 OOM 触发),码审 C3。已修:malloc 失败先 sock_close(s)(unref connectref/关句柄,on_sock_closed 落 selfref pin)再 luaL_error,三 pin 不再滞留。src/luna_loop.c:1618
- [x] l_sock_write 长写截断:src/luna_loop.c:1671 `uv_buf_init(copy, (unsigned)len)`,len>UINT_MAX(4GiB) 时写长截断,>4GiB 数据静默不发送。P3,码审 C4。已修:len>UINT_MAX 直接 luaL_error(单缓冲超 libuv 上限),不静默丢字节。src/luna_loop.c:1689
- [x] prepend_module_dir 路径截断:src/luna_main.c:128-131 `snprintf(merged, 10240, "%s%s", add, cur)`,package.path 超长(用户 LUA_PATH)时截断破坏路径(非崩溃)。P3,码审 C5。已修:snprintf 返回值超 10240 即弃 prepend 并 stderr 警告(不截断);新增回归 test_long_lua_path_not_truncated(LUA_PATH 40KB → 警告且不崩溃)。cli 组绿。src/luna_main.c:131,test/luna_test_cli.c
- [x] _FORTIFY_SOURCE 未开:编译面仅 -fstack-protector-strong,_FORTIFY_SOURCE=2 缺失(需 -O2 且 glibc)。P3,质量门。已修:luna 目标在 UNIX 且非 MSVC 加 -fstack-protector-strong 与 Release/RelWithDebInfo/MinSizeRel 的 -O2 -D_FORTIFY_SOURCE=2;Release 编译实测 nm 见 __memcpy_chk/__sprintf_chk 等。CMakeLists.txt:340
- [x] 覆盖率巡检(2026-11-11):before C 行 83.6%(2692/3222)/函数 89.8%(254/283)/分支 60.9%(884/1452)、Lua 89.82%(serve 48.62%、magic 64.21%),最低五模块 serve/magic/toml(83%)/loop(81%,TLS 子系统 ~464 暗行需真实 TLS 对端,外部 I/O 上限如实记录)/kernel(88%);after C 行 83.6%(2693/3222)、分支 61.0%(885/1452)、函数持平,Lua 91.66%(serve 86.19%、magic 80.49%)。选 serve.lua/magic.lua/luna_toml.c 三模块补真行为测试 11 例:serve 补 capture 上限封帧(fake client 证 1.1MB 慢客户端本会被 EAGAIN 丢)、counted dispatch(置空 __LUNA_SERVE_STEP 使显式 S.step 走活钩子,并断言 luacov runner.data 对 dispatch/record_input/magic/help/complete/空 magic/ps/EXIT 九处行计数——根因是 luna_count_hook 轮询挂起钩子期间消费 dispatch,luacov 不可见的测量盲区,此测试锁死盲区回归);magic 补 %info 身份/%modules 已载与未载与未知/inspect sidecar 经 package.preload 注入;toml 补 30 层嵌套数组恰好达 cap 可解码、31 层拒、键段 9 段过 11 段拒(tomlc17 扫描器 caps 括号 30/键段 10 先于 luna MAX_CONVERT_DEPTH=100 生效,luna convert-cap 行为 defense-in-depth 公开输入不可达,如实记录)。另修 serve 组既有 teardown 残留:pty target 死亡测 SIGKILL 后缺 unlink(path)(姊妹测有此行),二进制 exit 0 长期掩盖,补一行后 teardown 干净无残留目录。两树 ctest 17/17 全绿、零警告。e4e0c5f,885c901,a008c03


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

### 审核任务（只审不改）16f5a76fea52..HEAD（2026-11-03）

覆盖：3 提交（9331b22,83b7456,1018076）、10 文件全数（todo.md 为记账文件除外）。已实证：parseEnv 新旧对照（旧码 r[9]/r[10] 多出 `y<z">` 多余键，新码正确）与仓库内真实 util 模块加载验证均通过；serve.ps 测试桩仅影响 io.open/lfs，`kernel.alive` 为 C 绑定 `kill(pid,0)` 不受桩影响；install.ps1 的 Die=exit 1 不被 catch 吞、install.sh 的 die 定义于 line 38 早于使用。另注：工作树存在未提交改动（lua/luna/serve.lua、test/luna_test_serve.c，系 todo Finding 5「attach inputs 裁剪」的 WIP），位于审计范围 16f5a76..HEAD 之外，未纳入审查亦未改动。

- [x] 审核发现（低，安全提示，既有行为）：install.ps1:143 / install.sh:165 的 .sha256 侧车拉取为匿名（未随 `-Headers $headers` / `-H "Authorization"`），私有仓库/`--token` 路径侧车 404 →「无侧车无结论」→ 继续安装且零校验；提交 1018076 的「不匹配即硬失败」加固仅对公开/匿名路径生效，私有安装不受保护。属既有侧车设计、非本 diff 引入，但限制了该提交宣称的供应链属性（可随 token 拉取侧车令私有路径同享校验）。已修：`install.sh` 侧车 curl 按 `--token` 附 `Authorization: Bearer`；`install.ps1` 侧车请求复用 `$headers`；shell 语法/shellcheck 绿，新增 install_sh 回归第二腿断言 Bearer 传给 `.sha256`。b1943f9,test/luna_test_install.sh:88
- [x] 审核发现（低，测试缺口）：install.sh:169 / install.ps1:146 的 sha256 不匹配硬失败分支无自动化测试（test/ 无 install 脚本 harness）；parseEnv 跨行引号（r[9]/r[10]）与 serve.ps nil-read（test_ps_handles_stale_socket）修复均有有效确定性回归，唯安装脚本加固分支未测。已补：新增 ctest 组 install_sh（test/luna_test_install.sh），假 uname/curl/sha256sum/unzip 无网络驱动，两腿断言——侧车不匹配→die「sha256 mismatch」、--token 时侧车拉取携带 Bearer 头；install_sh 组绿，全量 ctest 17/17。test/luna_test_install.sh,test/CMakeLists.txt
- [x] 审核发现（低，错误处理缺失，既有模式）：serve.lua:300-303 的 nil-read 守卫只修 /proc/<pid>/cmdline 一处；同类未加保护的 `fh:read("a")` 仍存于 magic.lua:368（read 失败 src=nil，:370 `src:gsub` 抛错）、plugins.lua:40、modules.lua:36、rocks.lua:352、luna.lua:195、fs/init.lua:29。均读常规文件（nil-read 概率远低于过期 pid），既有、出 diff 范围，属同类缺陷残留。已修：magic.lua:368 加 src nil 守卫、rocks.lua:352 加 text nil 守卫（写 stderr+exit 1）、luna.lua:195 加 src nil 守卫（恢复 _G.arg 返 1）、fs/init.lua:29 加 data nil 守卫（error level 2）；plugins.lua:40/modules.lua:36 的 readfile 调用方已判 nil（契约安全）、http/init.lua:172 的 res:send 内部 `body or ""` 兜底（安全），均无需改。新增回归：magic %load read→nil（test_load_reports_read_failure）、fs.readFileSync read→nil（test_fs_read_reports_read_failure）。magic/modules/serve 组绿。lua/luna/magic.lua:370,lua/luna/rocks.lua:354,lua/luna.lua:197,lua/modules/fs/init.lua:31,test/luna_test_magic.c:388,test/luna_test_modules.c
- [x] 审核发现：prepend_module_dir 对超长 LUA_PATH 直接丢弃 module-dir 前置（而非旧行为截断但仍保留前置），致 luna 自身模块 argparse 不可达，-e/脚本/REPL 全模式报「module 'argparse' not found」无法启动，属回归；对应测试仅断言警告+非崩溃未捕获此启动失败。已修：prepend 改由 luaL_Buffer 动态拼接，超长 LUA_PATH 仍保留 module-dir 前置（不再截断/丢弃）；test_long_lua_path_not_truncated 改为断言 40KiB 路径下 #package.path 增长且 require("argparse") 可达。cli 组绿。src/luna_main.c:112,test/luna_test_cli.c:548
- [x] 审核发现：l_sock_write 超限拒绝用 luaL_error 的 %zu，而 Lua luaO_pushvfstring 仅支持 s/c/d/I/f/p/U/%，%zu 被原样输出、实际字节数缺失，错误信息误导。已修：改用 %I（lua_Integer，pushvfstring 支持），错误信息带真实字节数。src/luna_loop.c:1689
- [x] 审核发现：test_long_lua_path_not_truncated 的 -e 'assert(#package.path > 40000)' 因 bootstrap 失败（argparse 缺失）永不执行；即便执行，package.path 恰为 40000 字节，断言 >40000 恒假，未真正验证路径完整性。已修：测试改为断言路径增长（>40000）且 require("argparse") 可达、退出码 0。test/luna_test_cli.c:548
- [x] 审核发现：stream _want_read 让出条件只判 loop 模块已加载且有 setImmediate，未判 loop 是否在运行；loop 已加载但未运行时（纯同步消费/loop 未跑），同步源 64 次拉取后搁浅，数据需等 loop 转动（maybeDrain）才交付，旧行为同步完成。已修：新增 loop.running()（run/maybeDrain/turn 置 g_running），_want_read 仅当 loop.running() 为真才 setImmediate 让出；未运行时保持同步拉取。新增回归 test_stream_sync_source_without_running_loop（未运行→同步收口）与 test_running_reflects_an_active_run。modules/loop 组绿。lua/modules/stream/init.lua:249,src/luna_loop.c:88,test/luna_test_modules.c:3280,test/luna_test_loop.c:147
- [x] 审核发现：exec_pack 将 kernel.exec 的 raise（目标 stdout 断管致 io.write 抛错）当作普通失败，触发 line 回退重执行，使带副作用的表达式（如 foo()）被求值两次。已修：exec_pack 对 raise 返回带 raised=true 的失败帧，dispatch 仅在非 raised 时回退重执行。新增回归 test_raising_exec_does_not_rerun_line（断言单次求值）。serve 组绿。lua/luna/serve.lua:137,test/luna_test_serve.c:243
- [x] 审核发现：check_rock_component 字符集 [A-Za-z0-9._-] 允许前导 - 的 name/version，且 cmd_update 的锁名不经校验直入 luarocks argv（cmd_install --from-lock 的 via 亦然），敌意锁可注入 luarocks CLI 旗标（如 --tree=）。已修：check_rock_component 拒前导 -；cmd_update 与 cmd_install 的锁名/CLI 名先校验再入 argv。新增回归 test_update_refuses_flag_injected_lock_name。rocks 组绿。lua/luna/rocks.lua:54,test/luna_test_rocks.c:311
- [x] 审核发现：CMake 加固的 $<$<CONFIG:Release>:-O2 -D_FORTIFY_SOURCE=2> 追加于配置默认 -O3 之后，Release 实际降为 -O2（MinSizeRel 的 -Os 亦然），静默改变各配置优化档；且 if(UNIX AND NOT MSVC) 对 macOS/BSD/musl 亦加 -D_FORTIFY_SOURCE=2，与注释「glibc-like」不符。已修：硬化块去掉 -O 覆写（保留各配置默认优化档），_FORTIFY_SOURCE=2 改为 CheckCSourceCompiles 探测 __GLIBC__ 后仅对 glibc 系启用；-fstack-protector-strong 仍全 UNIX 非 MSVC 加。CMakeLists.txt:338
- [x] 审核发现：prepend_module_dir 的 lua-path 分支在 `luaL_addstring(&buffer, "/?/init.lua;")`（已以 `;` 结尾）之后又无条件 `luaL_addchar(&buffer, ';')`，拼接结果在 module-dir 模板与用户 LUA_PATH 之间产生 `;;`；cpath 分支因 LUNA_SO_EXT 不带 `;` 而恰好单 `;` 正确。该 `;;` 使 package.path 夹入一个空模板（vendored Lua 5.5.1 searchpath 对空模板 `fopen("")` 失败、运行时无害），但 `;;` 是 Lua 文档化的「以默认路径替换」标记（仅 env-var 初始化时由 setpath 展开，运行期 setfield 写入不展开），与旧行为单 `;` 及本重构注释「keep the module dir prepended」不符，属未预期畸形路径；setup_module_paths 对 side 与 LUNA_MODULES_DIR 各前置一次，lua 路径实际含两个 `;;`，若任何工具/未来实现按 `;;` 展开将改变搜索序。建议删 lua 分支 addstring 尾部 `;`（保留 addchar 作分隔）。已修：删 lua 分支 addstring 尾部 `;`，单 `;` 分隔，package.path 无 `;;`。cli 组绿。src/luna_main.c:137,test/luna_test_cli.c:548,3d37d94
- [x] 审核发现：g_running 为单一布尔，l_run/l_maybe_drain/l_turn 均在 uv_run 前置 1、返回后置 0；若回调内再调用 turn/maybeDrain/run（嵌套 uv_run），内层返回把 g_running 置 0 而外层 uv_run 仍在运行，running() 此后误报 false——stream _want_read 的 setImmediate 让出门控失效、改回同步拉取（大同步源可饿死循环）。l_turn 的 turning 只防 turn-in-turn，不防 run 内调 turn；改用深度计数 g_running_depth++/-- 更稳。触发需用户在回调内驱动循环，属边界情形，低危。已修：g_running 改深度计数 g_running_depth++/--，running() 报 >0；新增回归 test_running_stays_true_across_nested_drive（回调内 loop.turn 嵌套，断言外层 running 恒 true）。loop 组绿。src/luna_loop.c:89,test/luna_test_loop.c:165,3d37d94
- [x] 审核发现：test_update_refuses_flag_injected_lock_name 用 name="--tree=/tmp/evil"，其含 `=` 与 `/`，已被既有字符集 `^[A-Za-z0-9._%-]+$` 拒绝——即使不加新前导 `-` 守卫该测试亦通过，新守卫（拒 `-v`/`--tree` 等纯旗标名）未被该用例隔离验证；建议另用如 `-v` 或 `--tree`（无 `=`/`/`、落在字符集内）作输入以真正覆盖新分支。已修：用例改用 name="--tree"（字符集内、无 `=`/`/`），真正触发前导 `-` 守卫。rocks 组绿。test/luna_test_rocks.c:311,3d37d94
- [x] 审核发现：嵌套驱动仍会覆写 g_interrupted——外层 run 的 on_prepare 在 ^C 时置 g_interrupted=1 并 uv_stop（src/luna_loop.c:229-230），同轮 check 阶段已排队的 immediate 回调若调 turn/run/maybeDrain，内层入口 `g_interrupted = 0`（src/luna_loop.c:421,358,392）清掉该标志，外层 uv_run 因 stop 返回后 l_run 读到 0 不抛「interrupted」、脚本退出 0 而非 130；触发需 ^C 恰落在含嵌套驱动的同轮迭代，属边界情形、低危，与本次 g_running 修复同类（嵌套驱动覆写外层态），仅修 running() 未修 g_interrupted。已修：g_interrupted 改深度计数 g_interrupted_depth++/--，仅无外层驱动时清零，嵌套驱动不再覆写外层 pending 中断；新增回归 test_interrupt_survives_nested_drive（^C 落 on_prepare、同轮 immediate 嵌套 turn，断言嵌套调用成功且外层 run 仍抛「interrupted」）。loop 组 116/116 绿。src/luna_loop.c:90,test/luna_test_loop.c:185

- [x] 审核发现：中断后回调内嵌套驱动会让内层 uv_run 清除 libuv 的 stop 标志，外层默认循环继续等待未关闭的句柄，导致 Ctrl-C 延迟或永久无法返回；新增回归测试的 10 秒定时器也掩盖了该问题。已修：内层 run/maybeDrain/turn 的 uv_run 返回后，若检测到外层 pending 中断且当前非最外层驱动，立即补发 uv_stop 恢复外层停止请求；回归用例改用 200ms 定时器并断言 fired=false（原先 10000ms 定时器实测 loop 组 52s，改后 10s 内完成且 116/116 绿）。src/luna_loop.c:365,test/luna_test_loop.c:185

## 审核发现

- [x] 审核发现：回归测试依赖 200ms 定时器在首次 loop.run 前尚未到期，若进程在创建定时器后被调度暂停超过 200ms，libuv 会先执行到期定时器再处理中断，导致正确实现也因 fired=true 间歇失败。已修：200ms 定时器移入 immediate 回调内创建，外层首次 run 前不存在可到期句柄。loop 组绿：1/1 passed。test/luna_test_loop.c:196
- [x] 审核发现：第三轮复审证据「侧边栏 72 条链接逐条对盘」计数与实况不符——config.mts 侧边栏实际为 en 38 + zh 39 = 77 条链接条目（去重后 71 条，en 35 / zh 36）。已修：审计证据改为 77/71；脚本复算输出 en total 38 unique 35 / zh total 39 unique 36 / combined total 77 unique 71。docs/审计-文档一致性.md:86
- [x] 审核发现：lua/modules/toml/init.lua:1 的「TOML 1.0」注释与实现（tomlc17 v1.1）及文档「strict TOML v1.1」三方不符。已修：代码注释改「TOML 1.1」，审计裁定同步为已修正。lua/modules/toml/init.lua:1,docs/审计-文档一致性.md:91
- [x] 审核发现：custom.css 的 `.vp-doc table{display:block;overflow-x:auto}` 与 VitePress 内置规则完全重复（内置实际位于 vp-doc.css:225-230）。已修：删除冗余覆写；`rg "word-break|max-width: ?960|display: ?block|overflow-x|\.vp-doc table" custom.css` 无匹配。docs/.vitepress/theme/custom.css:206
- [x] 审核发现：custom.css 用 `word-break: break-word`（非标准/废弃值）。已修：改为标准 `overflow-wrap: break-word`；`rg word-break custom.css` 无匹配。docs/.vitepress/theme/custom.css:210
- [x] 审核发现：`.vp-doc table { display:block }` 使 table 失去表格格式化上下文，thead/tbody 可能各自匿名表化并错位列宽。已修：删除 `display:block` 覆写，恢复 VitePress 内置表格行为；残留检查无 `.vp-doc table`/`display:block`。docs/.vitepress/theme/custom.css:206
- [x] 审核发现：`max-width:960px !important` 无条件覆盖 VitePress 响应式内容宽度。已修：删除三条 960px 强制宽度规则，恢复默认主题缩放；残留检查无 `max-width:960`/`!important`。docs/.vitepress/theme/custom.css:206
- [x] 审核发现：本轮 diff 未见问题（6c7a545e..HEAD）。已核：逐项复核 5 文件 diff（CSS 清理/审计计数/TOML 口径/回归时序/todo 证据），`env -u NO_COLOR ctest` 17/17 绿，`npm run docs:build` 绿。git diff 6c7a545e..HEAD;cd build&&env -u NO_COLOR ctest
- [x] 审核发现：本轮 diff 未见问题（fb1e23d6..HEAD）。已核：该区间仅 `5cfbea0` 提交且只新增上一条审核结论行，无代码/文档内容改动；`env -u NO_COLOR ctest` 17/17 绿，`npm run docs:build` 绿。git diff --stat fb1e23d6..HEAD;cd build&&env -u NO_COLOR ctest
- [x] 审核发现：本轮 diff 未见问题（5cfbea05..HEAD）。已核：该区间仅 `38c0851` 提交且只新增上一条审核结论行，无代码/文档内容改动；`git diff --stat 5cfbea05..HEAD` 为 1 文件 1 插入。git rev-list 5cfbea05..HEAD;git diff --stat 5cfbea05..HEAD
- [x] 审核发现：本轮 diff 未见问题（38c08517..HEAD）。已核：该区间仅 `b530c97` 提交且只新增上一条审核结论行，无代码/文档内容改动；`env -u NO_COLOR ctest` 17/17 绿。git rev-list 38c08517..HEAD;git diff --stat 38c08517..HEAD;cd build&&env -u NO_COLOR ctest
- [x] 审核发现：本轮 diff 未见问题（b530c976..HEAD）。已核：该区间仅 `becf415` 提交且只新增上一条审核结论行，无代码/文档内容改动；`git diff --stat b530c976..HEAD` 为 1 文件 1 插入。git rev-list b530c976..HEAD;git diff --stat b530c976..HEAD
- [x] 审核发现（audit-batch4 luna 节 2026-11-11，P1-1 真实，已修）：tls_pump_body 的 pending-write 分支只有 `r == pend_len` 与 `r < 0` 两臂，SSL_write 返 0（对端 close_notify 零返回，write(2) 返 0 或他版 OpenSSL 构建可产生）时 pend 滞留、writeref 永不交付，流卡死回调泄漏；同型缺陷亦在同文件 l_tsock_write 同步快路径。已修：两处折叠为 `r <= 0`，非 WANT_* 经 tls_fail 交付回调；OpenSSL 3.5.5 探针实测 len>0 时 r==0 不可达（close_notify 后重试返 -1/WANT_WRITE、SYSCALL 返 -1），折叠属跨版防御、零成本。回归 test_tls_pending_write_delivers_callback_on_peer_close：64MiB pend 撞「读 1 记录即 close_notify+关闭」的服务端，断言 write: 回调必达且 loop 排空（诚实注：锁契约非判别——SYSCALL 路径修复前亦交付，r==0 臂本机 OpenSSL 不可达故无判别测试可写）。loop 组 117/117 绿。src/luna_loop.c:3368,3759,test/luna_test_loop.c:1596,6a40b66
- [x] 审核发现（audit-batch4 luna 节，P0-1≡P1a，裁定误报不改码）：称 tcp_dial_next/on_retry_closed 跨多地址重试复用同一 uv_connect_t 在前请求未完成时属 UB；vendored libuv 1.44.2 三重反证——(1) deps/libuv/src/unix/tcp.c:288-289 在途守卫 `connect_req != NULL` 同步返 UV_EALREADY，即便真复用也只是错误返回非 UB；(2) 同步失败各路径（family 检查 uv-common.c:405、maybe_new_socket tcp.c:294、connect() 非 EINPROGRESS/ECONNREFUSED tcp.c:331）均先于 `connect_req = req`（tcp.c:341）返回，while 迭代复用时无在途请求；(3) 异步路径 stream.c:1295 先清 connect_req 再调 cb（tcp.c 场景 1302-1303），on_retry_closed 复用时前请求已完结，属文档化生命周期。审核者给的触发「首个地址立即拒绝」：Linux ECONNREFUSED 走 delayed_error（tcp.c:321-330）rc==0 异步交付、根本不进 while 同步迭代；ENETUNREACH 走同步失败路径、同样无在途。两调用点（on_resolved:1591/on_retry_closed:1554）互斥于在途请求
- [x] 审核发现（audit-batch4 luna 节，P1b≡P1-3/P1-4，裁定误报不改码）：称 serve.lua:192 pcall 三值解包使成功 magic 落入 not okm 误报 ERR；实际 magic.dispatch 契约是成功返 true（magic.lua:62）失败返 (nil, err)（magic.lua:53,57），pcall 成功得 (true, true)→okm=true→not okm 假→OK 帧，失败得 (true, nil, err)→ERR 带真消息，逻辑正确；审核者漏算 dispatch 首个 true 落在 okm（其假设「成功时 okm=nil」与契约不符）。既有回归 test_magic_runs_against_live_state 锁 %whos→OK|
- [x] 审核发现（audit-batch4 luna 节，其余项裁定）：P0-2（luna_loop.c:3750 tls_fail 后继续执行）、报告条目3（:3630 connectref 未赋值）、P1-2、P1d、P1e 均审核者自行撤回（报告内已注「无 bug/撤回」）；P1c（repl.lua:161 表达式探测对 `--` 结尾/含 return 块误判）与 P1-5（toml datetime_shape offset 校验不一致）审核者自列 P2 低危，超出本批 P0/P1 范围；P2 合并段逐条自查均「无 bug/可接受」，无需处理
