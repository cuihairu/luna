# stdlib · path

Path handling. Pure Lua, same API as Node's `path`; the current platform is POSIX (`path.sep` = `/`, `path.delimiter` = `:`, with `path.posix` being the same object).

## API

| Call | Description |
| --- | --- |
| `path.normalize(s)` | Collapses `.`/`..` and repeated separators |
| `path.join(...)` | Joins and normalizes |
| `path.resolve(...)` | Resolves right-to-left to an absolute path, filling in cwd where missing |
| `path.relative(from, to)` | The relative path from `from` to `to` |
| `path.dirname(s)` / `path.basename(s, ext?)` / `path.extname(s)` | The trio |
| `path.isAbsolute(s)` | Absolute check |
| `path.parse(s)` / `path.format(o)` | Decompose / recompose `{root, dir, base, ext, name}` |
| `path.sep` / `path.delimiter` / `path.posix` | Constants and the POSIX face |
| `path.toNamespacedPath(s)` | Windows namespace path (`\\?\` prefix; the posix face returns it unchanged) |
| `path.win32` | The Windows-semantics face: the same full set of functions, platform-independent text semantics (drive letters/UNC/reserved names; `sep` is `\\`) |

## Usage

```lua
local path = require "path"
print(path.join("docs", "guide", "cli-repl.md"))
print(path.normalize("docs//guide/../guide/./cli-repl.md"))
print(path.extname("archive.tar.gz"), path.basename("a/b/c.lua", ".lua"))
```

```text
docs/guide/cli-repl.md
docs/guide/cli-repl.md
.gz	c
```

```lua
local path = require "path"
print(path.resolve("docs", "..", "README.md"))
print(path.relative("/usr/local", "/usr/share/lua"))
print(path.isAbsolute("/etc/hosts"), path.isAbsolute("etc/hosts"))
```

```text
<cwd>/README.md
../share/lua
true	false
```

`parse`/`format` are inverses:

```lua
local path = require "path"
local p = path.parse("/home/cui/work/report.final.pdf")
print(p.dir, p.name, p.ext)
print(path.format({ dir = p.dir, base = "report.pdf" }))
```

```text
/home/cui/work	report.final	.pdf
/home/cui/work/report.pdf
```

## Boundary rules

- Same as Node: `normalize` collapses interior `..`/`.` and repeated separators (pure text, no symlink resolution), and `resolve` does the same plus an absolute prefix; `resolve` does not preserve a trailing slash (Node and `normalize` diverge on this point).
- Windows paths (`C:\`, UNC, the `\\?\` namespace) go through the `path.win32` face — the same full API set as posix but parsed with Windows text semantics (platform-independent: usable on Linux too); the default face is still posix.
