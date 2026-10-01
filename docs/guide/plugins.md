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

## 完整例子:四个扩展点各一个

### 1) REPL 魔法命令 — `%utc` 打印当前 UTC 时间

```
plugins/utc/
├── plugin.json
└── init.lua
```

```json
{
  "name": "utc",
  "version": "0.1.0",
  "description": "%utc — print current UTC time",
  "main": "init.lua"
}
```

```lua
-- init.lua
local magic = require("luna.magic")
magic.register("utc", function()
    print(os.date("!%Y-%m-%dT%H:%M:%SZ"))
end, "%utc — print current UTC time in ISO 8601")
```

用法:

```
In [1]: %utc
2026-10-01T12:34:56Z
```

### 2) Tab 补全源 — 补全 `require("pkg.<TAB>")` 里的子模块名

```
plugins/pkg-complete/
├── plugin.json
└── init.lua
```

```json
{
  "name": "pkg-complete",
  "version": "0.1.0",
  "description": "complete require(\"pkg.<sub>\") submodules",
  "main": "init.lua"
}
```

```lua
-- init.lua
local complete = require("luna.complete")
local fs = require("fs")

-- 扫 luna_modules/pkg/ 的 .lua 文件与带 init.lua 的子目录
local function candidates(sub)
    local base = "luna_modules/pkg"
    local ok, entries = pcall(fs.readdirSync, base)
    if not ok then return {} end
    local out = {}
    for _, e in ipairs(entries) do
        local name
        if e:match("%.lua$") then
            name = (e:gsub("%.lua$", ""))
        elseif fs.isDirectory(base .. "/" .. e)
               and fs.exists(base .. "/" .. e .. "/init.lua") then
            name = e
        end
        if name and ("pkg." .. name):sub(1, #("pkg." .. sub)) == "pkg." .. sub then
            out[#out + 1] = "pkg." .. name
        end
    end
    table.sort(out)
    return out
end

complete.add_source(function(line)
    -- 行尾停在引号内才匹配:require("pkg.u  ← 注意没有闭合引号
    local sub = line:match('require%s*%(%s*["\']pkg%.([%w_]*)$')
        or line:match('require%s*["\']pkg%.([%w_]*)$')
    if not sub then return {} end
    return candidates(sub)
end)

return {}
```

两个要点:**匹配必须锚到行尾且不含闭合引号**——补全发生时行还没写完(`require("pkg.u`),模式里带 `["\']` 收尾就永远匹配不上;**候选是完整模块名**(`pkg.util`)——`require` 引号里的整段前缀(`pkg.u`)都在替换跨度里,只回 `util` 会把 `pkg.` 一起吃掉。

用法(在 `luna_modules/pkg/` 下有 `util.lua`、`core.lua` 与 `io/init.lua` 时):

```
In [1]: require("pkg.u<TAB>      ← 补出 pkg.util
In [1]: require("pkg.i<TAB>      ← 补出 pkg.io(目录包)
In [1]: require("pkg.<TAB>       ← 空前缀补出全部,table.sort 保序
```

### 3) 语法高亮 — 换一套配色方案(只改色,不换词法器)

```
plugins/dark-theme/
├── plugin.json
└── init.lua
```

```json
{
  "name": "dark-theme",
  "version": "0.1.0",
  "description": "dark syntax theme via set_style",
  "main": "init.lua"
}
```

```lua
-- init.lua
local hl = require("luna.highlight")
-- 只改 ANSI 色码,词法器不变
hl.set_style("keyword", "\27[38;5;203m")   -- 亮粉
hl.set_style("string",  "\27[38;5;114m")   -- 亮绿
hl.set_style("number",  "\27[38;5;215m")   -- 橙
hl.set_style("comment", "\27[38;5;242m")   -- 淡灰
hl.set_style("function","\27[38;5;75m")    -- 亮蓝
hl.set_style("operator","\27[38;5;250m")   -- 亮灰
return {}
```

> 也可用 `hl.set(fn)` 完全替换高亮器:`fn(text)` 返回**着色后的新文本**——返回非字符串或抛错,渲染就自动退回默认。想整套换着色逻辑时用它,只想换色板则 `set_style` 足够,`hl.reset()` 恢复默认高亮器。
>
> 作用面是**回显**:`Out[n]` 的值走 `highlight.render` 着色(上面的 `set_style` 就作用在这条链);输入行的实时着色是另一条链(replxx 的 16 色表 `color_map`,见 [CLI 与 REPL](/guide/cli-repl) 的高亮节),与这套 ANSI 样式表分开,`set_style` 改不到它。

### 4) 模块注入 — 注入一个带版本号的常量模块

```
plugins/app-version/
├── plugin.json
└── init.lua
```

```json
{
  "name": "app-version",
  "version": "1.2.3",
  "description": "inject app.version constant module",
  "main": "init.lua"
}
```

```lua
-- init.lua
-- 清单在插件自己旁边:按入口文件位置定位,不依赖工作目录
local src = debug.getinfo(1, "S").source:sub(2)   -- "@path" → "path"
local dir = src:match("(.*)/") or "."
local pkg = require("dkjson").decode(
    assert(require("fs").readFileSync(dir .. "/plugin.json"))
)

return {
    modules = {
        ["app.version"] = pkg.version
    }
}
```

用法:

```lua
In [1]: (require("app.version"))
Out[1]: '1.2.3'
```

外层括号把 `require` 截成单值——**新加载**的 `require` 会额外返回搜索器的 loader 数据,两值语义见[模块系统](/guide/modules)。

> 注入的值若是函数则作为 loader 使用(每次 `require` 都调用),若是普通值则包成恒返该值的 loader——行为与 `package.preload` 一致。

## 失败隔离

- 每个入口在 `pcall` 下执行:抛错(如 `error("boom")`)记入 `plugins.failed[name]`,**兄弟插件照常加载,luna 照常启动**;
- 补全源、高亮函数同样被 pcall 保护;
- 排查:`%plugins` 列出已加载与失败清单;失败详情在启动 stderr。

```text
In [1]: %plugins
loaded plugins:
  hello 0.1.0  greets on %hello
  utc 0.1.0  %utc — print current UTC time in ISO 8601
1 plugin(s) failed to load (see stderr)
```

## 边界约定

内核/插件边界由扩展点 API 定义:插件**只**通过 `magic.register`、`complete.add_source`、`highlight.set`、返回的 `modules` 表接入,不触碰 kernel 内部状态;加载器不解析插件内容。加载顺序即发现顺序(就近优先,同目录内按字典序),先加载者的名字与注册占先。