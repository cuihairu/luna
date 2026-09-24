# CLI 与 REPL

REPL 是 luna 的入口形态,CLI 的三种模式都是它的变体。本页按"启动 → 会话 → 魔法命令 → 退出"过一遍交互面。

## 启动模式

| 命令 | 行为 | 对标 |
| --- | --- | --- |
| `luna` | 交互控制台 | `node` |
| `luna script.lua a b` | 跑脚本后退出;`a b` 成为 chunk 的 `...`,`arg[0]` 为脚本路径 | `lua script.lua` |
| `luna -i script.lua` | 跑脚本后落入控制台(脚本的 globals 仍在) | `python -i` |
| `luna -e 'code'` | 求值后退出;表达式按 `Out[n]` 回显,未完整代码报错 | `node -e` |

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
