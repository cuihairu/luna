# 代码深审报告（2026-11-01 执行，2026-11-02 收口）

按 requesting-code-review 流程对 luna 全仓做代码深审：①安全静态扫描（硬编码密钥/shell 注入/SQL 拼接/eval/反序列化/路径穿越）；
②隐藏 bug 猎捕（错误处理缺失/竞态/越界/资源泄漏/off-by-one/并发）；③质量门（编译警告/测试基线对比）。
三路并行扫描（安全面 F 组、Lua 面 Lua 组、C 面 C 组），全部主张给 `file:line` 证据；
P0/P1 当场修（一问题一 commit，测试绿），P2/P3 记 todo（todo.md 同步登记）。

- 审计面：`src/*.c|h`（12 文件，约 7300 行）、`lua/luna*.lua`（37 文件）、`lua/modules/**`（25 文件）、
  `install.sh`/`install.ps1`、`.github/workflows/*`、`test/*.c`（16 文件）、`CMakeLists.txt`。
  `deps/` 属 vendored 第三方，按范围排除。
- 总账：**发现 17（P0×1、P1×4、P2×4、P3×8）｜已修 5（P0×1、P1×4）｜剩余 12（P2×4、P3×8）**。
- 修复验证：全量 ctest 16/16 绿（serve 39.5s 最重）；loop 组 112 例在 ASan（`detect_leaks=1`）下零报错零泄漏
  （修复前 110，+2 关闭-在-解析中压测）；
  全量编译零警告。

## P0

| ID | 类别 | 主张 | 源码证据 | 修复 |
| --- | --- | --- | --- | --- |
| F0 | 沙箱逃逸 | `--sandbox` 四层加固被预载的 `loop` 模块绕过：`require("loop")` 在沙箱内可达，`loop.process.execSync` 给 shell、`loop.fs.writeFile` 给任意写删、`loop.net` 给网络，沙箱宣称的隔离全破 | `src/luna_main.c:399`（`luaL_requiref` 预载 `loop`）→ `lua/luna/sandbox.lua:69-72`（baseline 快照含 `loop`）→ `:74-85`（require 守卫放行一切 baseline 名）；`src/luna_loop.c:4689-4693`（`loop.process` 面）、`:4629-4631`（exec=`sh -c`）、`:4645`（execSync=`popen`）、`:1034-1049`（fs 写删面） | 已修 f6fceb6：`loop`/`socket.core`/`socket.unix`/`mime.core`/`_openssl.*` 出基线并清 `package.loaded`；cli 10 例沙箱组锁面 |

## P1

| ID | 类别 | 主张 | 源码证据 | 修复 |
| --- | --- | --- | --- | --- |
| F1 | 沙箱逃逸 | 字节码加载防线被 `kernel.exec`/`kernel.check` 绕过：沙箱把 `_G.load` 收成 text-only，但同名全局 `kernel` 的加载器走 `luaL_loadbufferx(..., NULL)`（bt 双模），`kernel.exec(string.dump(f))` 直接执行字节码块 | `src/luna_main.c:393`（`kernel` 注册为全局）；`src/luna_kernel.c:434`（k_exec）、`:376`（k_check）——mode=NULL 即文本+二进制；`lua/luna/sandbox.lua:147-158` 只收 `_G.load`/`loadstring` | 已修 f6fceb6：`kernel.exec`/`kernel.check` 包装为 text-only（首字节 `\x1b` 拒绝）；`kernel.chmod` 一并拒绝；cli 沙箱组锁面 |
| L2 | 远程 DoS | `loop.http` 请求体无界：头限 64KiB 但体不设限，`Content-Length` 全信，`buf = buf .. chunk` 随攻击者流量无界增长直至 OOM；每块到达重扫 `buf:find("\r\n\r\n")` 并重解析整头，体大时 O(n²) | `lua/modules/loop/http.lua:616,619,645-648`（serve_conn 只挡头） | 已修 63b4e79：默认 16 MiB 封顶（`http.listen opts.max_body` 可调），头一次解析、体直落 `rest`，超限 413；loop 组锁面 |
| C1 | 崩溃 | `tsock_new` 在 `if (!t->ssl)` 之前调 `SSL_set_mode(t->ssl, ...)`：`SSL_new` 失败即空指针解引用（OpenSSL `SSL_set_mode` 直取 `s->mode`） | 修复前 `src/luna_loop.c:3088`（`SSL_new`）→ `:3094`（`SSL_set_mode`）→ `:3096`（NULL 检查在后，实际不可达——先崩在 :3094） | 已修 e8eb22f：检查上移到 `SSL_set_mode` 前；失败路径 `tsock_close` 统一关 fd/丢 pin；calloc 失败路径补关 accept fd，accept 调用方去重复关（现 :3113 `if (!t->ssl)` → :3122 `SSL_set_mode`） |
| C2 | UAF | 关闭-在-解析中：`uv_getaddrinfo` 在途时用户 close → 关闭回调立即丢 selfref pin → GC 回收结构体 → 解析完成回调 `on_resolved`/`on_tls_resolved` 读已释放内存。net.connect 与 connectTls 同根（TLS 侧 `poll_inited=0` 走 `on_tls_closed` 内联路径） | 修复前 `src/luna_loop.c:1409-1413`（on_sock_closed 无守卫丢 pin）、`:1523-1533`（on_resolved 读 `s->closed`）、`:3412-3419`（on_tls_closed 内联丢 pin）、`:3491-3493`（tsock_close 内联调 on_tls_closed）、`:3579-3589`（on_tls_resolved 读 `t->closed`） | 已修 a113410：`resolve_pending` 标志置位（:1637、:3603）；net 侧 `sock_pin_drop`（:1416）——关闭回调与解析完成**两者都到**才丢 pin（任意到达序安全）；TLS 侧 `on_tls_closed` 见 `resolve_pending` 即退（:3440），`on_tls_resolved` 清标志后补完延迟关闭（:3612-3618）；loop 组新增关闭-在-解析中压测 ×2（net 100 连发 + tls 50 连发，ASan 零报错） |

## P2（记 todo）

| ID | 类别 | 主张 | 源码证据 |
| --- | --- | --- | --- |
| F2 | 路径穿越 | `luna install --from-lock` 的 `name`/`version` 来自 `luna.lock` 直接拼路径，`../` 可穿越：`os.remove` 删任意路径、`sha256_file` 读任意文件（触发面：仓库内投毒 lock 文件） | `lua/luna/rocks.lua:259-262`（cache_file 拼接）、`:299`（os.remove）、`:80`（io.open）、`:308`（rockspec_sha）；`:286-289` shell 调用单引号包 argv，无命令注入，仅路径未消毒 |
| F3 | 路径穿越 | http.serve 静态面 Windows-only 穿越：只挡 `/` 分隔的 `..` 段，`%5C` 解码后 `\` 为 Windows 分隔符可绕过（POSIX 安全） | `lua/modules/http/init.lua:187`（urldecode）、`:196-203`（仅挡 `== ".."` 段）、`:204-207`（拼 root） |
| L1 | 死代码/错误逻辑 | `util.parseEnv` 多行引号值跳行是死代码：`:701` 的跨行跳过被 `:714` 无条件 `i = nl + 1` 覆盖，续行被重解析为 key=value（`'A="x\ny=z"\nB=1'` 多出 `y='z"'`） | `lua/modules/util/init.lua:701`（被覆盖）→ `:714`（无条件覆盖） |
| C3 | 资源泄漏 | `l_net_connect` 的 `malloc(resolver)` 失败走 `luaL_error` longjmp，但此时 `sock_new` 已 pin selfref/keepalive/connectref，三者永不释放，keepalive 计数滞留（仅 OOM 触发） | `src/luna_loop.c:1600-1603`（malloc 失败 return luaL_error，sock 已建 :1597） |

## P3（记 todo）

| ID | 类别 | 主张 | 源码证据 |
| --- | --- | --- | --- |
| L3 | nil 边界 | `serve.ps` 读 `/proc/<pid>/cmdline` 无 nil 检查，读失败（pid 回收/IO 错）抛 `attempt to index a nil value` | `lua/luna/serve.lua:285`（`fh:read("a")` 直接收 gsub，非 pcall 面） |
| L4 | 错误处理/泄漏 | attach 命令的 `kernel.exec` 未包 pcall：捕获 sink 的 `io.write` 中途抛错（目标 stdout 断管）越过 `kernel.sink(nil)`，sink 滞留，后续输出被吞进死命令的 acc 帧 | `lua/luna/serve.lua:181-185`（同形 `:171-172`） |
| L5 | 无界增长 | attach 会话 `inputs` 表永不裁剪，进程存活期随每条命令增长 | `lua/luna/serve.lua:136` |
| L6 | 紧循环 | stream `_want_read` 同步紧循环：常就绪同步源每推即耗时不让出，饿死定时器/attach 轮询/中断钩子 | `lua/modules/stream/init.lua:232-253` |
| L7 | 无界循环 | `%hist` 范围不封顶，`%hist 1-99999999999` 挂起会话 | `lua/luna/magic.lua:150` |
| C4 | 截断 | `l_sock_write` `uv_buf_init(copy, (unsigned)len)`：len>4GiB 时写长截断，超出部分静默不发送 | `src/luna_loop.c:1671` |
| C5 | 截断 | `prepend_module_dir` `snprintf(merged, 10240, ...)`：package.path 超长（用户 LUA_PATH）时截断破坏路径（非崩溃） | `src/luna_main.c:128-131` |
| Q1 | 质量门 | `_FORTIFY_SOURCE` 未开：编译面仅 `-fstack-protector-strong`，缺 `_FORTIFY_SOURCE=2`（需 -O2 且 glibc，Windows/clang 需条件化） | `CMakeLists.txt` 编译选项段 |

## 按设计/良性（不记缺陷）

- **F4** attach 通道本地 socket 求值任意代码：socket 0600（`lua/luna/serve.lua:62-68`）+ pid 数字校验（`:39-42`），同用户限定，设计如此。
- **F5** `loop.process.exec/execSync` shell 语义：Node 同款（caller 责任），文档已声明；沙箱缺口即 F0（已修）。
- **F6** 测试壳字符串拼接（`test/luna_test_serve.c:1054,1056` 等）：测试面自控路径。
- **F7** 测试 TLS 私钥（`test/luna_test_loop.c:1239` 自签名 fixture）与 `install.ps1` 文档占位 `ghp_xxx`：非真密钥。全仓 40+ 字符字面量/32+ hex 扫描零命中，无 SQL 面（全仓无 DB 用法），无反序列化执行面。

## 已证伪候选（猎捕过程中的假阳性，实证否决）

- `l_os_network_interfaces` 栈泄漏疑云：`lua_rawget` 实为 [-1,+1]（弹 key 压 value），栈每迭代完全平衡——独立最小复现实证（src/luna_loop.c:2807-2859）。
- `on_written` 在 close 后访问 sock：libuv 保证写回调先于关闭回调完成（uv_close 等待在途请求），pin 到 `on_sock_closed` 才丢，无 UAF。
- `uv_timer_init` 先于 `g_loop` 初始化：`luaopen_luna_loop`（src/luna_loop.c:4740-4744）先 init 后暴露，顺序安全。
- `luna_alloc` cap-used 下溢：used 恒 ≤ cap（检查序保证），无下溢路径。
- `g_wake_wfd` 信号竞态：fd 早于信号安装落位，竞窗内读 -1 即跳过，无崩溃面。

## 质量门

- 编译：全量零警告（gcc，`-Wall` 全开）。
- 测试基线：修复前 16/16 组绿（loop 110）；修复后 16/16 组绿（loop **112**，+2 关闭-在-解析中压测），serve 39.5s、rocks 21.5s、line 11.7s 全绿。
- ASan：loop 组 112 例 `detect_leaks=1` 零报错零泄漏（`-fsanitize=address -O1` 独立构建）。
- 静态面：`strcpy` ×2（src/luna_loop.c:4406,4417）均为 `malloc(strlen()+1)` 精确尺寸；`sscanf "%200[^\n]"` 入 `reason[224]` 有界（src/luna_toml.c:116）；无 `gets`/`sprintf`/`%n`；格式串全字面量。

## commit 清单（本轮码审）

| commit | 内容 | 验证 |
| --- | --- | --- |
| f6fceb6 | F0+F1：沙箱出基线 capability 模块 + 拒二进制块 + kernel.chmod 拒绝 + lfs 硬化 | cli 沙箱组 10 例；全量 ctest 绿 |
| 63b4e79 | L2：loop.http 请求体 16 MiB 封顶 + 头单次解析 | loop 组新增 test_http_server_body_cap；全量 ctest 绿 |
| e8eb22f | C1：SSL_new 空指针检查上移 + 失败路径 fd 契约统一 | loop 组全绿；ASan 零报错 |
| a113410 | C2：DNS 解析期 UAF（resolve_pending 延迟丢 pin，net+TLS 双腿） | loop 组 110→112（+2 关闭-在-解析中压测）；ASan 零报错零泄漏 |

todo.md 已同步登记剩余 12 项（P2×4、P3×8），每条带 file:line 与来源组别。
