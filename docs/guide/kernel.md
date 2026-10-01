# 全局对象:kernel

`kernel` 是预注册的**全局**——不需要 `require`,`require "kernel"` 拿到的也是同一张表(Node 里 `process` 的对位:进程状态、输出漏斗、中断控制的单一入口)。它是整个运行时唯一用 C 写的策略底座;REPL、补全、插件都在它之上。

## API 一览

| 调用 | 语义 |
| --- | --- |
| `kernel.exec(src, chunkname?, ...args)` | 求值一段代码;`args` 成为 chunk 的 `...` |
| `kernel.check(chunk)` | 只解析不求值:`"ok" \| "incomplete" \| "error", msg` |
| `kernel.write(...)` | 各参数(须为字符串)依次写进输出漏斗 |
| `kernel.sink(fn?)` | 输出改道到 Lua 函数;`nil` 还原 stdout |
| `kernel.clear_interrupt()` | 复位挂起的 `^C` 标志 |
| `kernel.pid()` | 本进程 pid(attach socket 路径由它派生) |
| `kernel.wake(pid)` | 给目标进程发 `SIGUSR1`,唤醒其轮询点 |
| `kernel.chmod(path, "600")` | 八进制字符串设权限(lfs 不带 chmod 的补位) |
| `kernel.umask(mask?)` | 设 umask 并返回旧值;缺省 `0077` |
| `kernel.millis()` | 单调时钟毫秒(整数,`CLOCK_MONOTONIC`) |
| `kernel.tty()` | stdin 是否终端 |
| `kernel.colors()` | 着色是否可用(TTY + `TERM` 非 `dumb` + 无 NO_COLOR) |
| `kernel.version()` | `"luna 0.1.0"` |

## 执行与校验

- **`kernel.exec(src, chunkname?, ...args)`**:编译并执行 `src`。`chunkname` 进 traceback(约定 `"@" .. 路径` 表示真实文件,REPL 用 `"=(repl)"`、`-e` 用 `"eval"`);其余参数成为 chunk 的 `...`——`run_script` 就靠它把命令行参数递给脚本。返回 `ok, err` 风格:编译或运行错误都走第二个返回值,不抛。
- **`kernel.check(chunk)`**:只过编译器,把「还差下一行」(`"incomplete"`——以运算符/开括号/裸 `return` 等收尾,解析器明确要求续行)与「真错误」(`"error", msg`)分开。REPL 的多行续行判定就建立在它之上;想在输入层做自己的交互面,这是入口。

## 输出漏斗

所有内核输出汇经同一个 `emit()`:

- **`kernel.write(...)`**:字符串(非字符串直接抛参数错误)原样写漏斗——不加分隔符、不换行;
- **`kernel.sink(fn)`**:注册后每段输出改投 `fn(str)`(顺序保真,测试与插件的输出捕获即此);`kernel.sink(nil)` 还原 stdout;
- 内置 `print` **已接管**到同一漏斗(标准 print 的格式契约不变:tab 分隔、结尾换行)——所以 `kernel.sink` 一样捕得到 `print`,attach 的输出捕获、REPL 的会话输出都由此成立。

## 中断与进程

- **`kernel.clear_interrupt()`**:回提示符时调用——空转时到达的 `^C` 直接丢弃(bash 语义),不留给下一个 chunk;
- **`kernel.pid()` / `kernel.wake(pid)`**:attach 的一对原语——socket 文件名取 `pid`,客户端发 `SIGUSR1` 让目标的阻塞行编辑返回空行、主循环顺势轮询。

## 权限

- **`kernel.chmod(path, "600")`**:luafilesystem 不带 chmod,而 attach socket 必须从第一刻起就 owner-only——这是补位函数;mode 必须是八进制字符串;
- **`kernel.umask(mask?)`**:serve.start 在 bind 前收紧 umask、之后还原,靠的是「返回旧值」这一面。

## 时钟与环境

- **`kernel.millis()`**:单调钟,不受系统对时影响;`%timeit` 与 `loop.now()`(libuv 循环自己的钟)各有用途,别混用;
- **`kernel.tty()`** 看 stdin,**`kernel.colors()`** 看 stdout + 环境变量(`NO_COLOR`/`LUNA_NO_COLOR` 出现即关,[no-color.org](https://no-color.org) 契约;`LUNA_COLOR=1/0` 强开强关),REPL 的着色开关全部以它为准;
- **`kernel.version()`**:版本串,`luna --help` 的标题同源。

## 内部面

`kernel.count_hook()` 是计数钩子自身逻辑的普通可调用形态:一次 `^C` 检查加一次 attach 轮询。只有覆盖率构建(luacov 独占调试钩子槽,行事件转发到此)会显式调它;普通运行不会遇到,也不需要调。
