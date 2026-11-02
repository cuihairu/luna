# Plugin development

A luna plugin is **a directory**: a `plugin.json` manifest plus an entry script. The loader only discovers, orders, executes, and reports; plugins connect themselves to the REPL through runtime extension points — the kernel knows nothing about any particular plugin.

## Minimal plugin

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

The returned table may contain a `modules` key: each entry is injected into `package.preload`, so any later `require("hellox")` in the session can load it. Plain values are wrapped as loaders; functions are used as loaders as-is.

You do not have to write this skeleton by hand: `luna new plugin hello` generates it, including the `%hello` registration and manifest (see [Examples and scaffolding](/guide/examples)). This page explains the mechanics from scratch.

## Discovery and precedence

Nearest first, much like `node_modules`:

1. `./plugins` (project);
2. `~/.luna/plugins` (user);
3. `$LUNA_PLUGIN_PATH` (colon-separated, in order).

The same name is **first come, first served**: a project plugin shadows a user plugin with the same name, and the latter is recorded in `plugins.overridden`. Directories with unreadable manifests are recorded by path in `plugins.failed`. The `--no-plugins` startup switch skips discovery entirely.

## Four extension points

| Extension point | Registration | Purpose |
| --- | --- | --- |
| REPL command | `require("luna.magic").register(name, run, help)` | `%name` magic commands, with `run(session, arg)` |
| Tab completion | `require("luna.complete").add_source(fn)` | `fn(line) -> candidates`, with failing sources skipped |
| Syntax highlighting | `require("luna.highlight").set(fn)` | Replace the lexer/colors; `set_style(tag, ansi)` changes colors only |
| Module injection | `return { modules = { … } }` | Modules become requirable through `package.preload` |

`luna.highlight` also provides `set_style(tag, code)` (fine-tune a color) and `reset()` (restore defaults); `luna.magic`'s command table is available as `magic.commands` for inspection.

## Complete examples: one per extension point

### 1) REPL magic command — `%utc` prints the current UTC time

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

Usage:

```
In [1]: %utc
2026-10-01T12:34:56Z
```

### 2) Tab completion source — complete submodules in `require("pkg.<TAB>")`

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

-- Scan .lua files and subdirectories with init.lua under luna_modules/pkg/
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
    -- Match only when the line ends inside the quote: require("pkg.u  ← no closing quote
    local sub = line:match('require%s*%(%s*["\']pkg%.([%w_]*)$')
        or line:match('require%s*["\']pkg%.([%w_]*)$')
    if not sub then return {} end
    return candidates(sub)
end)

return {}
```

Two points matter: **the match must anchor to end-of-line and not include a closing quote** — completion runs while the line is still incomplete (`require("pkg.u`), so a pattern ending in `["\']` would never match; and **candidates must be full module names** (`pkg.util`) — the whole prefix inside the require quotes (`pkg.u`) is the replacement span, so returning only `util` would consume `pkg.` too.

Usage (with `util.lua`, `core.lua`, and `io/init.lua` under `luna_modules/pkg/`):

```
In [1]: require("pkg.u<TAB>      ← completes pkg.util
In [1]: require("pkg.i<TAB>      ← completes pkg.io (directory package)
In [1]: require("pkg.<TAB>       ← empty prefix completes all, ordered by table.sort
```

### 3) Syntax highlighting — replace the color scheme (colors only, same lexer)

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
-- Change ANSI colors only; the lexer stays the same
hl.set_style("keyword", "\27[38;5;203m")   -- bright pink
hl.set_style("string",  "\27[38;5;114m")   -- bright green
hl.set_style("number",  "\27[38;5;215m")   -- orange
hl.set_style("comment", "\27[38;5;242m")   -- light gray
hl.set_style("function","\27[38;5;75m")    -- bright blue
hl.set_style("operator","\27[38;5;250m")   -- light gray
return {}
```

> `hl.set(fn)` can replace the highlighter completely: `fn(text)` returns the **new colored text**, and a non-string return or error automatically falls back to the default renderer. Use it when changing the whole highlighting algorithm; `set_style` is enough for a palette. `hl.reset()` restores the default highlighter.
>
> This affects **result echoing**: `Out[n]` values are colored through `highlight.render` (where `set_style` applies). Live input-line highlighting is a separate chain (replxx's 16-color table, `color_map`; see the highlighting section of [CLI and REPL](/guide/cli-repl)) and is not changed by `set_style`.

### 4) Module injection — inject a constant module with a version

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
-- The manifest sits beside the plugin: locate it from the entry file, not the working directory
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

Usage:

```lua
In [1]: (require("app.version"))
Out[1]: '1.2.3'
```

The outer parentheses truncate `require` to one value — a **fresh** `require` also returns the searcher's loader data; see [Module system](/guide/modules) for the two-value semantics.

> If the injected value is a function it is used as the loader (called on every `require`); a plain value is wrapped in a loader that always returns it — matching `package.preload` behavior.

## Failure isolation

- Each entry runs under `pcall`: an error (such as `error("boom")`) is recorded in `plugins.failed[name]`, while **sibling plugins load normally and luna still starts**;
- Completion sources and highlight functions are protected by `pcall` too;
- Diagnose with `%plugins`, which lists loaded and failed plugins; failure details go to startup stderr.

```text
In [1]: %plugins
loaded plugins:
  hello 0.1.0  greets on %hello
  utc 0.1.0  %utc — print current UTC time in ISO 8601
1 plugin(s) failed to load (see stderr)
```

## Boundary rules

The extension-point API defines the kernel/plugin boundary: plugins integrate **only** through `magic.register`, `complete.add_source`, `highlight.set`, and the returned `modules` table, never by touching kernel internals; the loader does not parse plugin contents. Load order is discovery order (nearest first, lexicographic within a directory), and earlier plugins win names and registrations.
