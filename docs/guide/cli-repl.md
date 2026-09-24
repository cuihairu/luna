# CLI 与 REPL

REPL 是 luna 的入口形态,CLI 的三种模式都是它的变体。本页按"启动 → 会话 → 魔法命令 → 退出"过一遍交互面。

## 启动模式

| 命令 | 行为 | 对标 |
| --- | --- | --- |
| `luna` | 交互控制台 | `node` |
| `luna script.lua a b` | 跑脚本后退出;`a b` 成为 chunk 的 `...`,`arg[0]` 为脚本路径 | `lua script.lua` |
| `luna -i script.lua` | 跑脚本后落入控制台(脚本的 globals 仍在) | `python -i` |
| `luna -e 'code'` | 求值后退出;表达式按 `Out[n]` 回显,未完整代码报错 | `node -e` |
| `luna --attach <pid>` | 交互式连到另一个运行中的 luna 进程 | gdb attach / ipython `%connect_info` |

模式可组合通用开关 `--no-color` 与 `--no-plugins`;插件对**每种**启动模式都生效——脚本可能 `require` 插件注入的模块。

## 输入模型

- **多行续行**:以 `function`/`if`/`for`/`while`/`do`/`{` 等开头且括号/块未闭合时,提示符进入续行态,整块一次性求值;续行内 `^C` 丢弃整块。
- **`^C` 中断**:正在执行的死循环可被 `^C` 打断,会话不退出,错误消息含 `interrupted`;状态由 `kernel.clear_interrupt()` 复位。
- **历史**:输入持久化到 `~/.luna_history`(带时间戳分隔),重启后 `↑` 召回;replxx 的增量检索(`^R` 风格)可用。
- **Tab 补全**:链式表达式逐级解析——全局名、字段、metatable `__index`;已注册的插件补全源同样参与(异常源被跳过)。
- **帮助糖**:`?expr` 与 `expr?` 打印值的签名、文档与示例;`%whos` 列出当前全部全局。

## 语法高亮

输入行与回显都按 scintillua 的 Lua 词法器实时着色(LPeg 驱动)。着色只在 TTY 生效,管道/脚本模式下自动降级;`NO_COLOR` / `--no-color` 可显式关闭。词法器与配色方案都是可插拔的——见[插件开发](/guide/plugins)。

## 魔法命令

以 `%` 开头的行不进入 Lua 求值,直接由 REPL 拦截派发:

| 命令 | 作用 |
| --- | --- |
| `%help` | 列出全部魔法命令(含插件注册的) |
| `%time expr` | 执行一次并报 wall time |
| `%timeit expr` | 反复执行至 100ms/1000 次,报单次最优 |
| `%hist` | 本会话全部输入 |
| `%whos` | 当前全局变量一览 |
| `%reset` | 清空用户全局与 `Out` 寄存器(保留 stdlib/kernel 等运行时环境) |
| `%clear` | 清屏(仅 TTY) |
| `%plugins` | 已加载插件清单与失败报告 |
| `%exit` | 退出(同 `^D`) |

## 会话状态

- **`In[n]` / `Out[n]`**:每个非 nil 结果登记为 `Out[n]`,并同步写入 `_`(最近)与 `__`(次近);
- `%reset` 清理用户状态但保留运行时快照(`__LUNA_BASE_GLOBALS` 里记录的名字:各 stdlib、kernel、lpeg 等);
- 插件注入的模块经 `require` 可见,受 `package.loaded` 缓存约束——重载语义与 Lua 一致。

## attach:接入一个运行中的 luna

每个 luna 进程启动时都会在 `$LUNA_SOCK_DIR/luna-<pid>.sock`(默认 `/tmp`,0600)开一个 unix 域监听,`luna --attach <pid>` 连上去后进入 attach 控制台:**本地行编辑、远程求值**——命令在目标的实时 Lua 状态里执行(同一份 globals、同一份 `Out`、同一个死循环中的脚本),本地 REPL 的交互习惯全部可用:

```
$ luna --attach 12345
attached to 12345 — %detach or ^D to leave
attach> gx = 99999
nil
attach> gx
99999
attach> print("hello")        -- 输出捕获:回显到 attach 会话,同时镜像到目标终端
hello
nil
attach> %whos                 -- 魔法命令跑在目标状态上
attach> ?gx                   -- ?expr / expr? 帮助糖照常
number
attach> %detach
```

- **魔法命令与帮助糖**:`%whos`、`%hist`、`%reset` 等通过目标自己的 `luna.magic` 表远程执行(`%hist`/`%reset` 作用于 attach 会话自己的历史与寄存器);`?expr` / `expr?` 描述目标状态里的值。
- **输出捕获**:命令执行期间目标内核的输出被收集进应答帧回显给客户端,同时镜像到目标自己的终端——两边看到同一条流;单帧输出上限 64KB。
- **Tab 补全跑在远程**:候选来自**目标**的全局与 `package.loaded`(经 `\1complete` 元行协议),所以在 attach 里能补出只存在于目标状态的函数名。
- **`%exit` 在 attach 里不杀目标**:它只让客户端脱离,目标进程继续运行;真正退出目标请在目标终端或 `os.exit()`。

**目标端怎么被"敲到"**:目标进程有两类轮询点——REPL 空闲时,attach 客户端的一条 `SIGUSR1` 让阻塞的行编辑器返回空行,主循环顺势轮询一次;脚本(哪怕死循环)执行期间,C 内核的计数钩子每约 20 万条指令顺带轮询一次。所以无论目标在等输入还是在空转,attach 都能及时到达。

**边界**:

- 每次一条命令,顺序应答;应答以 `\30` 字节封帧(`OK` 成功 / `ERR` 出错 / `EXIT` 脱离),`OK` 帧走 stdout、`ERR` 帧(含完整 traceback)走 stderr——管道里拿到的是干净的结果流;
- 目标端默认开启;`--no-serve` 可显式关闭(不建 socket 文件),attach 客户端自身不会重复开监听;
- 求值全程 pcall 隔离——attach 会话里的报错不会打倒目标进程;目标崩溃时客户端得到 `attach: target went away`;
- 退出:`%detach` 或 `^D`;目标进程退出时 socket 文件自动清理。
