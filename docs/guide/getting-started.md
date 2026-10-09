# 快速上手

## 获取 luna

三条路,按需选:

**一键安装**(推荐:装最新每日构建进 PATH,装完即验 `luna --version`,重跑即升级):

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/cuihairu/luna/main/install.sh | sh
```

```powershell
# Windows (PowerShell)
irm https://raw.githubusercontent.com/cuihairu/luna/main/install.ps1 | iex
```

产物来自**滚动 nightly Release**(固定 tag [`nightly`](https://github.com/cuihairu/luna/releases/tag/nightly),每次绿跑清旧传新重发):公共仓库资产公开可下,**匿名直拉,不需要任何凭据**;每个 zip 带 `.sha256` 侧车,脚本下载后自动校验(不匹配只告警——nightly 可能在下载间隙被重发,重跑一次即可)。`--token`/`GITHUB_TOKEN`/`LUNA_INSTALL_MIRROR`/`LUNA_MIRROR` 保留作私有 fork 与直链兜底。

**手动下载**:直接去 [nightly Release 页](https://github.com/cuihairu/luna/releases/tag/nightly)取 **`luna-nightly-<os>-<arch>.zip`**(`luna-nightly-linux-x86_64` / `luna-nightly-linux-aarch64` / `luna-nightly-macos-aarch64` / `luna-nightly-windows-x86_64`)+ `.sha256` 侧车,内含二进制与 `luna_modules/` 模块侧车(二进制从自身同级目录解析 Lua 策略层,整包拷走即用)。四条腿全矩阵转正(Windows 2026-10-08、macOS 2026-10-09):构建或测试红回归直接拉响夜间,无试运行豁免腿。Daily Build 工作流([daily-build.yml](https://github.com/cuihairu/luna/actions/workflows/daily-build.yml))是这些资产的来源,per-run artifact 只是本地副本,交付通道是 Release。

解压后验证:

```bash
./luna -e '6*7'      # 应答 Out[1]: 42
```

macOS 包未做签名,首次运行先 `xattr -cr luna` 清隔离属性。

**源码构建**:依赖只有 CMake ≥ 3.16 与 C 编译器;其余(Lua 5.5、LPeg、replxx、scintillua、luasocket、luafilesystem、lua-zlib、luaossl、dkjson、argparse、libuv、tomlc17、libyaml+lyaml、expat+lua-expat、lualogging 等)全部在 `deps/` 内,configure 时自动拉取。

```bash
# Linux / macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build          # 全部测试组应全绿
```

macOS 另需 `brew install pkg-config autoconf cmake` 并 `export MACOSX_DEPLOYMENT_TARGET="10.6"`。测试组清单与覆盖率插桩树的用法见[构建与自测](/other/build)。

可选:安装 OpenSSL 开发头文件(`libssl-dev`)后重新 configure,zlib 之外的 crypto 模块(`sha256`、`hmac`、随机字节等)随之启用;未安装时构建照常成功,`require("crypto")` 会给出指引性错误。

**Windows 源码移植未完**(nightly 矩阵有探针腿,逐层消化中),当前推荐 WSL2——见 [FAQ](/other/faq)。

## 三十秒上手

```bash
luna                          # ① 交互控制台(像 node)
luna hello.lua                # ② 跑一个脚本(像 lua)
luna serve ./docs 8000        # ③ 秒起一个静态文件服务(像 python -m http.server)
```

第 ③ 步不需要任何脚本:浏览器打开 `http://localhost:8000` 就是目录列表或 index.html。要接口服务,一行也够:

```bash
luna -e 'require("http").serve(function(req, res)
  res:json({ hello = "luna", path = req.path }) end)'
```

```bash
$ curl http://localhost:8000/api?name=cui
{"hello":"luna","path":"/api"}          # 键序以运行时为准
^C                                       # ^C 即停,退出码 130
```

TCP echo 同样一行(另开终端 `echo hi | nc 127.0.0.1 9000` 即见 `echo: hi`):

```bash
luna -e 'require("net").serve("*", 9000, function(c)
  c:send("echo: " .. (c:receive("*l") or "") .. "\n") end)'
```

细节见[标准库 · http](/stdlib/http)与[标准库 · net](/stdlib/net)。

## 启动方式

```bash
luna                     # 交互控制台(像 node)
luna script.lua a b      # 跑脚本后退出,a b 成为脚本的 `...`(像 lua)
luna -i script.lua       # 跑脚本后落入控制台
luna -e 'print(6 * 7)'   # 求值后退出(像 node -e),表达式按 Out[n] 回显
luna serve [dir] [port]  # 静态文件服务,^C 停止(见上「三十秒上手」)
```

通用开关:

| 开关 | 作用 |
| --- | --- |
| `--no-color` | 关闭输出 ANSI 着色 |
| `--no-plugins` | 跳过插件发现与加载 |
| `--no-serve` | 不建 attach 监听 socket |
| `--help` | 用法与示例 |

全部开关与环境变量见[配置与环境变量](/other/config)。

着色策略:TTY 且非 `dumb` 终端时自动开启;`NO_COLOR` 环境变量强制关闭,`LUNA_COLOR=1` 强制开启;管道下自动降级为纯文本——脚本输出永远不会被污染。

## 第一次 REPL 会话

```lua
In [1]: 6 * 7
Out[1]: 42

In [2]: os.time()                       ← Tab 补全 os.<TAB>
In [3]: ?string.format                  ← ? 帮助糖
string.format(fmt, ...) — formats per Lua printf rules

In [4]: %whos                           ← 当前全局一览
In [5]: %exit                           ← 或 ^D
```

`In[n]`/`Out[n]` 与 IPython 同构:每个非空结果自动登记进 `Out[n]`(同时存入 `_` 与 `__`),`%hist` 回看全部输入。

## 项目里使用

```
myapp/
├── luna_modules/
│   └── hello/            ← 一个包:清单 + 入口
│       ├── package.json  {"name":"hello","main":"init.lua"}
│       └── init.lua      return { greet = function() return "hi" end }
└── app/
    └── main.lua          require("hello").greet()
```

```bash
luna app/main.lua        # 裸名 hello 沿目录逐级上溯找到 luna_modules/
```

细节见[模块系统](/guide/modules);给项目加插件(如自定义魔法命令)见[插件开发](/guide/plugins)。
