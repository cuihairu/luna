# 介绍

**luna** 是一个 Lua 5.5 交互环境与脚本运行时,包含两部分愿望:

- **IPython 式的 REPL**:多行输入、实时语法高亮、Tab 补全、内省帮助、魔法命令、`In[n]`/`Out[n]` 会话记录;
- **Node.js 式的运行时**:贴近 Node 的模块解析规则、目录插件机制、随二进制分发的标准库。

Lua 本身把一切留给宿主:没有命令行参数解析、没有包管理、交互层只有一行 `lua -`。luna 把宿主补齐,而内核始终是**官方 Lua 5.5**——luna 不含自制解释器。

## 它长什么样

```
$ luna
luna 0.1.0 · Lua 5.5
In [1]: 6 * 7
Out[1]: 42
In [2]: def fib = fn  …          ← 输入未完整时自动续行
In [2]: | function fib(n)
In [2]: |   if n < 2 then return n end
In [2]: |   return fib(n-1) + fib(n-2)
In [2]: | end
In [3]: fib(10)
Out[3]: 55
In [4]: %time fib(20)
Wall time: 0.014 ms
```

输入行实时着色(基于 scintillua 词法器 + LPeg),未完成块自动续行,`^C` 中断当前块回到提示符,`?expr` 给出帮助。

## 三条设计底线

1. **成熟库铁律**:行编辑、词法、参数解析、JSON、zlib、TLS 全部取自维护良好的成熟库,见[架构设计](/architecture#选型对比)的逐项对比;
2. **内核/策略分层**:C 内核只提供状态与原语,REPL、补全、高亮、魔法命令全部是嵌入二进制的 Lua 源,插件可在运行期替换其中任意一环;
3. **REPL 始终是入口**:`luna` 裸启动进控制台,`luna script.lua` 与 `luna -e` 是它的脚本形态——node 也是这么做的。

## 下一步

- [快速上手](/guide/getting-started):构建与三种启动方式;
- [CLI 与 REPL](/guide/cli-repl):魔法命令与会话模型;
- [模块系统](/guide/modules):require 解析与清单;
- [插件开发](/guide/plugins):四个扩展点与失败隔离。
