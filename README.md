<p align="center">
  <img src="assets/logo.svg" alt="luna logo" width="140"/>
</p>

<h1 align="center">luna</h1>

<p align="center">通用的 Lua 集成环境</p>

## 定位

luna 把一批通用工具集成进同一个二进制:交互式 REPL、Node 式的模块解析、现代标准库、目录插件、事件循环。每件工具都能单独用,也可以互相咬合;直接用、挑着用,或者把它们当零件拼成自己的工作流,都行。

Lua 生态里趁手的通用工具不多,每开一个新项目,周边设施都要先搭一遍。luna 想把这一层提前备好,把时间还给业务本身。

IPython 式的 REPL 是其中一件工具,不是全部。

## 三十秒

```bash
luna                     # 交互控制台(像 node)
luna hello.lua           # 跑一个脚本(像 lua)
luna serve ./docs 8000   # 秒起一个静态文件服务(像 python -m http.server)
```

接口服务一行也够,`^C` 即停:

```bash
luna -e 'require("http").serve(function(req, res)
  res:json({ hello = "luna", path = req.path }) end)'
```

TCP echo 同样一行(收一行回一行,`^C` 即停),另开终端 `echo hi | nc 127.0.0.1 9000` 即见 `echo: hi`:

```bash
luna -e 'require("net").serve("*", 9000, function(c)
  c:send("echo: " .. (c:receive("*l") or "") .. "\n") end)'
```

REPL 里 `6 * 7` 回答 `Out[1]: 42`,`%timeit` 随手测性能,`%whos` 看当前全部全局;细节见[CLI 与 REPL](https://cuihairu.github.io/luna/guide/cli-repl)。

## 获取

**一键安装**(装最新每日构建进 PATH,装完即验 `luna --version`,重跑即升级):

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/cuihairu/luna/main/install.sh | sh
```

```powershell
# Windows (PowerShell)
irm https://raw.githubusercontent.com/cuihairu/luna/main/install.ps1 | iex
```

Actions 产物的下载需要 GitHub 凭据:脚本会自动探测 `gh` CLI 登录态,也可传 `GITHUB_TOKEN`(`install.sh` 另收 `--token`);拿到匿名直链时用 `LUNA_INSTALL_MIRROR` / `LUNA_MIRROR` 直接指过去,全程不出网关。当前产物矩阵:`linux-x86_64`、`linux-aarch64`、`macos-aarch64`(macOS 与 Windows 项为试运行,见下)。

- **每日构建**:Actions 的 [Daily Build](https://github.com/cuihairu/luna/actions/workflows/daily-build.yml) 每天定时构建+全量测试,artifact 按平台命名(`luna-nightly-<os>-<arch>`,内含二进制与 `luna_modules/` 模块侧车——二进制从自身同级目录解析 Lua 策略层,保留 14 天);Windows 项为首飞试运行(源码的 Windows 移植未完,尚无产物),macOS 项构建与产物已通、运行期测试按平台移植清单收敛中(见 todo),两者均红不拖垮其余平台;CI 产物分发,无 tag 无 Release。
- **源码构建**:依赖只有 CMake ≥ 3.16 与 C 编译器,其余全部在 `deps/` 内:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build       # 全部测试组应全绿
```

macOS 需要 `brew install pkg-config autoconf cmake` 并 `export MACOSX_DEPLOYMENT_TARGET="10.6"`;可选 `libssl-dev` 启用 crypto 模块。详见[快速上手](https://cuihairu.github.io/luna/guide/getting-started)。

## 工具集

| 能力 | 说明 | 文档 |
| --- | --- | --- |
| REPL | replxx 行编辑、scintillua/LPeg 实时高亮、^C 中断、历史召回、`In[n]`/`Out[n]` | [CLI 与 REPL](https://cuihairu.github.io/luna/guide/cli-repl) |
| 魔法命令 | `%time` `%timeit` `%hist` `%whos` `%reset` `%plugins` `%help` …,可插件注册 | [CLI 与 REPL](https://cuihairu.github.io/luna/guide/cli-repl) |
| 事件循环 | `loop`:定时器、TCP/Unix/TLS、异步 fs、信号、子进程;脚本尾部自动排水 | [事件循环](https://cuihairu.github.io/luna/guide/loop) |
| 一行起服 | `luna serve` 静态目录;`http.serve()` 静态/可编程;`net.serve()` TCP echo | [标准库 · http](https://cuihairu.github.io/luna/stdlib/http) |
| 模块 | 相对 require、`luna_modules/` 逐级上溯、`package.json` 式清单 `main`、`package.loaded` 缓存语义 | [模块系统](https://cuihairu.github.io/luna/guide/modules) |
| 标准库 | json/fs/net/http/csv/ini/toml/yaml/xml/zlib/crypto 绑定成熟 C 库或 LPeg;path/util/events/stream 纯 Lua 按 Node 语义;随二进制一体分发 | [标准库总览](https://cuihairu.github.io/luna/stdlib/) |
| 插件 | `./plugins` → `~/.luna/plugins` → `$LUNA_PLUGIN_PATH`,manifest + 入口,失败隔离 | [插件开发](https://cuihairu.github.io/luna/guide/plugins) |
| 架构 | C 内核薄层 + 嵌入 Lua 策略层 + 扩展点;选型对比与事件循环 rationale | [架构设计](https://cuihairu.github.io/luna/architecture) |

## 文档

- 文档站:**https://cuihairu.github.io/luna** — 指南(快速上手 / CLI 与 REPL / 模块系统 / 插件开发 / 包管理 / 事件循环)、标准库逐模块参照、配置与环境变量、构建与自测、FAQ、架构设计;
- 源码在 [`docs/`](docs/),VitePress:`cd docs && pnpm install && pnpm run docs:dev` 本地预览。

## 开发

- 测试:ctest 十六组(luna / repl / cli / complete / introspect / highlight / magic / modules / plugins / serve / loop / rocks / line / linedit / main / covsum),改完跑 `ctest --test-dir build`;覆盖率用独立 Profiling 插桩树,见[构建与自测](https://cuihairu.github.io/luna/other/build);
- 推送 main 后 CI 自动构建并发布文档站到 Pages(部署不计为发布)。
