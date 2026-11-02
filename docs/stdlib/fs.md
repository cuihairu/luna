# stdlib · fs

Filesystem. Backed by [luafilesystem](https://lunarmage.co.uk/projects/luafilesystem/) (lfs): **the full lfs API is reachable verbatim** (`attributes`/`dir`/`mkdir` and so on, passed through `__index`), with a Node-style synchronous convenience layer on top. Asynchronous file operations (read/write/directory/stat) live in `loop.fs` under [loop](/guide/loop), not on this page.

## API

| Call | Description |
| --- | --- |
| `fs.exists(path)` | True if it exists (file or directory) |
| `fs.isFile(path)` / `fs.isDirectory(path)` | Type checks |
| `fs.readFileSync(path)` | Reads the whole file as a string; raises if it cannot be opened (message carries the reason) |
| `fs.writeFileSync(path, data)` | Overwrites; returns `true` on success, raises if it cannot be opened |
| `fs.appendFileSync(path, data)` | Appends when the file exists |
| `fs.readdirSync(dir)` | Array of directory entry names, **already sorted**, without `.`/`..` (same as Node) |
| `fs.mkdirSync(path, recursive?)` | Creates a directory; with `recursive`, missing parents are created and an existing dir also counts as success (`mkdir -p` semantics) |
| `fs.attributes(path, what?)` | lfs verbatim: `mode`/`size`/`mtime`… |
| `fs.dir(path)` | lfs verbatim: an entry iterator (including `.`/`..`) |
| (everything else) | All `lfs` functions pass through `__index` |

## Usage

```lua
local fs = require "fs"
fs.writeFileSync("/tmp/luna-fs-demo.txt", "hello ")
fs.appendFileSync("/tmp/luna-fs-demo.txt", "luna")
print(fs.readFileSync("/tmp/luna-fs-demo.txt"))
print(fs.exists("/tmp/luna-fs-demo.txt"), fs.isFile("/tmp/luna-fs-demo.txt"))
```

```text
hello luna
true	true
```

```lua
local fs = require "fs"
fs.mkdirSync("/tmp/luna-fs-demo/a/b", true)   -- missing parents are created too
print(fs.isDirectory("/tmp/luna-fs-demo/a/b"))
print(table.concat(fs.readdirSync("/tmp/luna-fs-demo/a"), ","))
```

```text
true
b
```

The lfs-verbatim face is available as usual. `attributes` has two forms: without `what` it returns the whole attribute table (`mode`/`size`/`mtime`/`permissions`…), with `what` it returns the single value directly:

```lua
local fs = require "fs"
local a = fs.attributes("/tmp/luna-fs-demo.txt")
print(a.mode, a.size > 0)
print(fs.attributes("/tmp/luna-fs-demo.txt", "mode"))
```

```text
file	true
file
```

For a missing path `attributes` returns `nil, err` (lfs convention) — `fs.exists` takes exactly its first return value. The `fs.dir` iterator includes `.`/`..`, while `fs.readdirSync` already filters and sorts them; use the latter to "read a directory," the former only to "stream a large directory."

## Boundary rules

- Synchronous blocking calls — fine to use in scripts; inside the event loop, for large files use the `loop.fs` async face, don't block the loop.
- Failure semantics: the convenience layer's "cannot open / already exists" follows Node style and **raises** (message includes the reason); the lfs passthrough keeps lfs's own return-value style (`nil, err`).
