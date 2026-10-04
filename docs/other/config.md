# 配置与环境变量

luna 没有配置文件:全部行为开关经**环境变量**与**命令行开关**控制,默认值开箱即用,每条都可单独覆盖。

## 环境变量

| 变量 | 作用 | 默认 |
| --- | --- | --- |
| `NO_COLOR` / `LUNA_NO_COLOR` | 关闭一切 ANSI 着色(输入行与回显同进同退,见[CLI 与 REPL](/guide/cli-repl)) | 未设置 |
| `LUNA_COLOR` | `1` 强制开色、`0` 强制关色;优先级低于上面的关闭变量 | 未设置 |
| `TERM` | 为 `dumb` 时自动降级为纯文本 | — |
| `HOME` | 决定历史文件 `~/.luna_history` 与用户插件目录 `~/.luna/plugins` 的位置;为空时历史不落盘、用户目录跳过 | — |
| `LUNA_PLUGIN_PATH` | 冒号分隔的额外插件目录,依次参与发现,优先级排在 `./plugins` 与 `~/.luna/plugins` 之后 | 未设置 |
| `LUNA_SOCK_DIR` | attach 监听 socket(`luna-<pid>.sock`,0600)的存放目录 | `/tmp` |
| `LUNA_VENDOR_DIR` | 内嵌 LuaRocks 的 vendor 目录覆盖(一般不需要动,构建期已定) | 构建期定值 |
| `LUNA_COVERAGE` | 仅 Profiling 覆盖率构建有效:置位后嵌入式策略模块按真实文件名加载以便 luacov 记数 | 未设置 |
| `PWD` | `path` 模块在取不到进程 cwd 时的回退 | shell 透传 |

`LUNA_*` 之间没有从属关系,逐条独立生效;`NO_COLOR` 同名于 [no-color.org](https://no-color.org)——**出现即关**,值为空也算置位;`LUNA_COLOR` 要非空才生效,`0` 关、其余值开。安装脚本自己消费的变量(`LUNA_INSTALL_MIRROR` / `LUNA_MIRROR`、`GITHUB_TOKEN`)是安装期一次性输入,不归 luna 二进制管,见[快速上手](/guide/getting-started)。

## 命令行开关

| 开关 | 作用 |
| --- | --- |
| `-i --interactive` | 跑完脚本后(或没有脚本时)落入交互控制台;脚本的 globals 仍在(对标 `python -i`) |
| `-e --eval 'code'` | 求值后退出;表达式结果按 `Out[n]` 回显,未完整代码报错(对标 `node -e`) |
| `-v --version` | 打印版本后退出(脚本安装器的装后自验就是它);不载插件、不开 attach socket |
| `--no-color` | 本次会话关闭着色(与 `NO_COLOR` 同效,范围仅本次) |
| `--no-plugins` | 跳过整个插件发现过程;排查"装了插件之后行为变了"先加这个复现 |
| `--no-serve` | 不建 attach 监听 socket(目标端关闭被接入能力;客户端自身本来就不建) |
| `--attach <pid>` | 接入另一个运行中的 luna 进程,见 [CLI 与 REPL](/guide/cli-repl) 的 attach 节 |
| `--help` | 用法与示例 |

子命令另有自己的参数面,不经此表:`luna serve [dir] [port]`(目录/端口不限顺序)见[标准库 · http](/stdlib/http);`luna ps` 是只读进程视图;`luna install/search/list/update` 透传内嵌 LuaRocks 自己的旗标(`--tree`、`--from-lock`…)见[包管理](/guide/rocks)。启动模式的完整清单见 [CLI 与 REPL](/guide/cli-repl)。

## 落盘文件

| 路径 | 内容 |
| --- | --- |
| `~/.luna_history` | 输入历史(带时间戳分隔),重启后 `↑` 召回;TTY 且 `HOME` 可用时才写 |
| `$LUNA_SOCK_DIR/luna-<pid>.sock` | attach 监听,0600,进程退出自动清理 |
| `<项目根>/.luna/rocks/` | `luna install` 的依赖树(LuaRocks `--tree`),向上逐级查找 |
| `<项目根>/luna.lock` | 依赖快照:逐 rock 版本 + 源包 sha256,`--from-lock` 据此复现 |
| `<项目根>/.luna/cache/` | 下载过的 `.src.rock` 源包缓存(离线复现用) |

后三行只在用 `luna install` 等包管理子命令时出现,且**只写在项目里**。除此之外 luna 不在用户机器上写任何全局状态:没有全局缓存目录,没有首次运行向导,没有遥测。
