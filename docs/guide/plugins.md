# 插件开发

luna 的插件是**一个目录**:一份 `plugin.json` 清单加一个入口脚本。加载器只做发现、排序、执行、报告;插件通过运行期扩展点把自己接进 REPL——内核不认识任何具体插件。

## 最小插件

```
plugins/hello/
├── plugin.json
└── init.lua
```

```json
{
  "name": "hello",
  "version": "0.1.0",
  "description": "greets on %hello",
  "main": "init.lua"
}
```

```lua
-- init.lua
require("luna.magic").register("hello", function()
    print("hello from plugin")
end, "%hello — say hi")

return { modules = { hellox = { greet = "hi" } } }
```

`return` 的表可带 `modules` 键:每项注入 `package.preload`,之后会话里任何 `require("hellox")` 都能拿到。普通值会被包成 loader;给函数则原样作为 loader 使用。

## 发现与优先级

就近优先,与 node_modules 同思路:

1. `./plugins`(项目);
2. `~/.luna/plugins`(用户);
3. `$LUNA_PLUGIN_PATH`(冒号分隔,依次)。

同一名字**先到先得**:项目目录可以遮蔽用户目录的同名插件,后者记入 `plugins.overridden`。清单不可读的目录以路径为键记入 `plugins.failed`。启动开关 `--no-plugins` 跳过整个发现过程。

## 四个扩展点

| 扩展点 | 注册方式 | 用途 |
| --- | --- | --- |
| REPL 命令 | `require("luna.magic").register(name, run, help)` | `%name` 魔法命令,`run(session, arg)` |
| Tab 补全 | `require("luna.complete").add_source(fn)` | `fn(line) -> 候选数组`,异常源被跳过 |
| 语法高亮 | `require("luna.highlight").set(fn)` | 换词法器/配色;`set_style(tag, ansi)` 只改色 |
| 模块注入 | `return { modules = { … } }` | 经 `package.preload` 成为可 require 的模块 |

`luna.highlight` 还有 `set_style(tag, code)`(微调颜色)与 `reset()`(回到默认);`luna.magic` 的命令表在 `magic.commands`,可被检查。

## 失败隔离

- 每个入口在 `pcall` 下执行:抛错(如 `error("boom")`)记入 `plugins.failed[name]`,**兄弟插件照常加载,luna 照常启动**;
- 补全源、高亮函数同样被 pcall 保护;
- 排查:`%plugins` 列出已加载与失败清单;失败详情在启动 stderr。

```text
In [1]: %plugins
loaded plugins:
  hello 0.1.0  greets on %hello
1 plugin(s) failed to load (see stderr)
```

## 边界约定

内核/插件边界由扩展点 API 定义:插件**只**通过 `magic.register`、`complete.add_source`、`highlight.set`、返回的 `modules` 表接入,不触碰 kernel 内部状态;加载器不解析插件内容。加载顺序即发现顺序(就近优先,同目录内按字典序),先加载者的名字与注册占先。
