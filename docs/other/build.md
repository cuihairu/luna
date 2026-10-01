# 构建与自测

## 依赖

仅需 **CMake ≥ 3.16** 与 **C 编译器**(gcc/clang)。除 cmocka(FetchContent 拉取,仅测试树用)外,全部在 `deps/` 内:

- Lua 5.5
- LPeg
- replxx (行编辑)
- scintillua (高亮)
- luasocket
- lua-zlib
- luafilesystem
- luaossl (需系统 OpenSSL 开发头)
- dkjson
- argparse
- tomlc17 (TOML 1.0)
- lyaml + libyaml (YAML 1.1)
- lua-expat + expat (XML)
- libuv (事件循环后端)
- LuaRocks (内嵌源码,`luna install` 等子命令用)
- luacov (覆盖率,Profiling 树专用)
- cmocka (测试框架,FetchContent 拉取)

首次 configure 需要联网(拉取 git 子模块与 cmocka);之后离线可构建——**无需手动安装任何 LuaRocks 或系统 Lua 包**。

## 构建步骤

```bash
# Linux / macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build          # 全部测试组应全绿
```

**Windows 不在支持面**(replxx 桥未编 windows.cxx、attach 走 unix 域 socket),推荐 WSL2——见 [FAQ](/other/faq)。

### 可选:启用 crypto 模块

安装 OpenSSL 开发头文件后重新 configure:

```bash
# Debian/Ubuntu
apt-get install libssl-dev
# Fedora/RHEL
dnf install openssl-devel
# macOS (Homebrew)
brew install openssl
```

重新配置后 `require("crypto")` 可用(`sha256`、`hmac`、`rand.bytes` 等)。未安装时构建照常成功,运行时给出指引性错误。

### macOS 特殊说明

```bash
brew install pkg-config autoconf cmake
export MACOSX_DEPLOYMENT_TARGET="10.6"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

包内 `PLATFORM-NOTES.txt` 有同款说明。首次运行需 `xattr -cr luna` 清隔离属性。

## 测试组

`ctest --test-dir build` 跑十六组:

| 组 | 覆盖面 |
| --- | --- |
| `repl` | REPL 会话、多行续行、历史、着色 |
| `cli` | 启动模式、参数解析、`-e`/脚本/serve/attach |
| `complete` | Tab 补全、链式表达式、插件补全源 |
| `introspect` | `?expr`/`expr?`、`%whos`、值描述 |
| `highlight` | 词法器、配色、`luna.highlight` 扩展点 |
| `magic` | 内建魔法命令、插件注册命令、`%plugins` |
| `modules` | require 解析、luna_modules 上溯、包清单、`package.loaded` |
| `plugins` | 插件发现/优先级/失败隔离、四扩展点注册 |
| `luna` | 入口冒烟:无参启动、stdin EOF 干净退出 |
| `serve` | `http.serve` 静态/handler 双模式、`luna serve`、`^C` 契约、close 排水 |
| `loop` | 定时器、TCP/Unix/TLS、异步 fs、信号、子进程、HTTP 客户端/服务端 |
| `rocks` | 包管理(安装/卸载/搜索/依赖解析) |
| `line` | 行编辑器基础、光标、杀词 |
| `linedit` | replxx 绑定、增量检索、历史持久化 |
| `main` | 入口分发、信号安装、模块嵌入、启动流程 |
| `covsum` | 覆盖率汇总目标(`gcovr_summary`、`lua_coverage`) |

### 单组跑法

```bash
ctest --test-dir build -R serve     # 只跑 serve 组
ctest --test-dir build -E loop      # 跑除 loop 外所有组
```

### 串行与并行

`ctest` 默认**串行**;日常可 `ctest --test-dir build -j"$(nproc)"` 提速。覆盖率测量(`build-cov`)必须串行:

```bash
ctest --test-dir build-cov -j1      # 统计文件合并假定无并发写
```

## 覆盖率测量

两棵树各自全绿是提交前置条件;覆盖率用独立的 Profiling 插桩树,同一套测试原样跑一遍:

```bash
cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Profiling \
      -DCMAKE_C_FLAGS_PROFILING="-O0 -g --coverage" \
      -DCMAKE_CXX_FLAGS_PROFILING="-O0 -g --coverage" \
      -DCMAKE_EXE_LINKER_FLAGS_PROFILING="--coverage" \
      -DCMAKE_SHARED_LINKER_FLAGS_PROFILING="--coverage"
cmake --build build-cov -j
ctest --test-dir build-cov           # 串行
cmake --build build-cov --target gcovr_summary    # C 侧(src/)
cmake --build build-cov --target lua_coverage     # Lua 侧(lua/)
```

### C 侧

由 gcov/gcovr 计数。被 SIGTERM 杀掉的进程不写 `.gcda`,所以测试夹具都以正常退出收尾。

### Lua 侧

由内置的 luacov 接线计数(仅 Profiling 树自动生效):测试与二进制在 `LUNA_COVERAGE=1` 下把嵌入式策略模块按真实文件名加载,行级命中合并进 `build-cov/luacov.stats.out`,`lua_coverage` 目标渲染报告并把汇总表打到构建输出。普通构建与交互行为不受影响(报错里的 `'=(luna/x)'` 块名原样保留)。

### 合并 hook 分工

每个 Lua 态只有一个 debug 钩子槽位,覆盖率接管该槽位后按事件分发——行事件给 luacov 记数,行事件与 count 事件都转发一步 `kernel.count_hook()`(`^C` 中断检查与 attach 轮询)。转发不挑事件种类是刻意的:luacov 的逐行记账本身也消耗 count 预算,紧凑循环里 count 事件可能只落在钩子帧内而永远到不了用户代码,行事件每迭代必发,从它驱动不依赖指令预算的落点。

## 文档站本地预览

```bash
cd docs
pnpm install           # 首次
pnpm run docs:dev      # http://localhost:5173/luna/
pnpm run docs:build    # 产物在 .vitepress/dist/
```

VitePress 2.0-alpha,base `/luna/`,中文本地搜索。

## CI:Daily Build

`.github/workflows/daily-build.yml`:

- `schedule`: 每天 UTC 01:23
- `workflow_dispatch`: 手动触发
- 三平台并行构建(Linux x64 / Linux arm64 / macOS arm64)
- 产物上传为 GitHub Artifacts,固定名 `daily-build`
- Artifacts 里:三平台 zip + `BUILD_INFO.txt`(commit 与时间)+ `VERIFY.md`(验证步骤)
- **不打 tag、不发 Release、不推镜像**——纯产物分发

取件:Actions → Daily Build → 最新成功 run → Artifacts 区 → `daily-build`。

## 推送 main 后

CI 自动构建并发布文档站到 GitHub Pages(部署不计为发布)。