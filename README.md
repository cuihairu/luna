<p align="center">
  <img src="assets/logo.svg" alt="luna logo" width="140"/>
</p>

<h1 align="center">luna</h1>

<p align="center">通用的 Lua 集成环境</p>

## 定位

luna 把一批通用工具集成进同一个二进制:交互式 REPL、Node 式的模块解析、现代标准库、目录插件。每件工具都能单独用,也可以互相咬合;直接用、挑着用,或者把它们当零件拼成自己的工作流,都行。

Lua 生态里趁手的通用工具不多,每开一个新项目,周边设施都要先搭一遍。luna 想把这一层提前备好,把时间还给业务本身。

IPython 式的 REPL 是其中一件工具,不是全部。

## 快速原型:C++/Lua 场景

游戏行业的主流技术栈是 C++ 加 Lua:C++ 承担引擎与框架,Lua 承担脚本层。写原型时,真正花时间的往往不是业务逻辑,而是交互环境、模块组织、工具库这些周边,每个项目都得重新搭一遍。

luna 面向的就是这个场景:内核是官方 Lua 5.5,单二进制,克隆下来配好就能用。原型从第一天就能跑;已有的 C++/Lua 工程,也可以把它放在手边当日常的脚本工具。

## 工具集

| 能力 | 说明 | 文档 |
| --- | --- | --- |
| REPL | replxx 行编辑、scintillua/LPeg 实时高亮、^C 中断、历史召回 | [CLI 与 REPL](https://cuihairu.github.io/luna/guide/cli-repl) |
| 魔法命令 | `%time` `%timeit` `%hist` `%whos` `%reset` `%plugins` `%help` …,可插件注册 | [CLI 与 REPL](https://cuihairu.github.io/luna/guide/cli-repl) |
| 模块 | 相对 require、`luna_modules/` 逐级上溯、`package.json` 式清单 `main`、`package.loaded` 缓存语义 | [模块系统](https://cuihairu.github.io/luna/guide/modules) |
| 标准库 | json/fs/net/http/csv/ini/toml/yaml/xml/zlib/crypto 绑定成熟 C 库或 LPeg;path/util/events/stream 纯 Lua 按 Node 语义;随二进制一体分发 | [模块系统](https://cuihairu.github.io/luna/guide/modules) |
| 插件 | `./plugins` → `~/.luna/plugins` → `$LUNA_PLUGIN_PATH`,manifest + 入口,失败隔离 | [插件开发](https://cuihairu.github.io/luna/guide/plugins) |
| 架构 | C 内核薄层 + 嵌入 Lua 策略层 + 扩展点;选型对比与事件循环 rationale | [架构设计](https://cuihairu.github.io/luna/architecture) |

## 安装

```bash
# build(依赖已在 deps/,configure 自动拉取;可选 libssl-dev 启用 crypto)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build                      # 全部测试组全绿

# run
./build/luna                                # 交互控制台(像 node)
./build/luna script.lua a b                 # 跑脚本后退出(像 lua)
./build/luna -i script.lua                  # 跑脚本后落入控制台
./build/luna -e 'print(6 * 7)'              # 求值后退出(像 node -e)
```

Linux 需要 cmake ≥ 3.16 与 C 编译器;macOS 另需 `brew install pkg-config autoconf cmake` 并 `export MACOSX_DEPLOYMENT_TARGET="10.6"`。

## 文档与开发

- 文档站:https://cuihairu.github.io/luna(源码在 [`docs/`](docs/),VitePress,`cd docs && pnpm install && pnpm run docs:dev` 本地预览);
- 测试:ctest 十六组(repl / cli / complete / introspect / highlight / magic / modules / plugins / kernel / serve / loop / rocks / line / linedit / main / covsum),改完跑 `ctest --test-dir build`;
- 推送 main 后 CI 自动构建并发布文档站到 Pages(部署不计为发布)。
