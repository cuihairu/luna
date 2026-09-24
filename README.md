# luna

IPython 式的 Lua 交互环境,按 Node.js 运行时的方式生长:多行续行、实时语法高亮、Tab 补全、`In[n]`/`Out[n]` 会话、`%time` 风格魔法命令;模块解析沿 `luna_modules/` 逐级上溯,目录插件提供魔法命令 / 补全源 / 高亮规则 / 模块注入四个扩展点。内核是官方 Lua 5.5,标准库(json/fs/net/http/zlib/crypto)绑定成熟 C 库并随二进制分发。

文档站:https://cuihairu.github.io/luna (源码在 [`docs/`](docs/),VitePress)。

## quick start

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

## 概览

| 能力 | 说明 | 文档 |
| --- | --- | --- |
| REPL | replxx 行编辑、scintillua/LPeg 实时高亮、^C 中断、历史召回 | [CLI 与 REPL](https://cuihairu.github.io/luna/guide/cli-repl) |
| 魔法命令 | `%time` `%timeit` `%hist` `%whos` `%reset` `%plugins` `%help` …,可插件注册 | [CLI 与 REPL](https://cuihairu.github.io/luna/guide/cli-repl) |
| 模块 | 相对 require、`luna_modules/` 上溯、清单 `main`,内置 json/fs/net/http/zlib/crypto | [模块系统](https://cuihairu.github.io/luna/guide/modules) |
| 插件 | `./plugins` → `~/.luna/plugins` → `$LUNA_PLUGIN_PATH`,manifest + 入口,失败隔离 | [插件开发](https://cuihairu.github.io/luna/guide/plugins) |
| 架构 | C 内核薄层 + 嵌入 Lua 策略层 + 扩展点;选型对比与事件循环推迟的 rationale | [架构设计](https://cuihairu.github.io/luna/architecture) |

## 开发

- 测试:ctest 九组(repl / cli / complete / introspect / highlight / magic / modules / plugins / kernel),改完跑 `ctest --test-dir build`;
- 文档:`cd docs && pnpm install && pnpm run docs:dev`;推送 main 后 CI 自动构建并发布到 Pages(部署不计为发布)。
