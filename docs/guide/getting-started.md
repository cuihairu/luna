# 快速上手

## 构建

依赖:CMake ≥ 3.16 与 C 编译器;其余(Lua 5.5、LPeg、replxx、scintillua、luasocket、lua-zlib、luafilesystem、luaossl、dkjson、argparse)全部在 `deps/` 内,configure 时自动拉取。

```bash
# Linux / macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build          # 全部测试组应全绿
```

可选:安装 OpenSSL 开发头文件(`libssl-dev`)后重新 configure,zlib 之外的 crypto 模块(`sha256`、`hmac`、随机字节等)随之启用;未安装时构建照常成功,`require("crypto")` 会给出指引性错误。

Windows 用 `cmake --build build --config Debug`,同上。

## 三种启动方式

```bash
luna                     # 交互控制台(像 node)
luna script.lua a b      # 跑脚本后退出,a b 成为脚本的 `...`(像 lua)
luna -i script.lua       # 跑脚本后落入控制台
luna -e 'print(6 * 7)'   # 求值后退出(像 node -e),表达式按 Out[n] 回显
```

通用开关:

| 开关 | 作用 |
| --- | --- |
| `--no-color` | 关闭输出 ANSI 着色 |
| `--no-plugins` | 跳过插件发现与加载 |
| `--help` | 用法与示例 |

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
