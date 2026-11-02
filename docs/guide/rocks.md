# Package management

luna's package management builds on **LuaRocks**, the de facto standard repository for the official Lua ecosystem (indexed by lua.org), effectively Lua's npm. luna does not implement its own registry client; it is a thin wrapper that turns common `luarocks` subcommands into `luna` commands and **locks dependencies into the project tree** for reproducible offline installs.

## A complete cycle (recorded session)

Walk through install → use → lock → reproduce in an empty directory. The output is from a real session:

```console
$ mkdir rocks-demo && cd rocks-demo
$ luna install inspect
Installing https://luarocks.org/inspect-3.1.3-0.rockspec
inspect 3.1.3-0 depends on lua >= 5.1 (5.5-1 provided by VM: success)
No existing manifest. Attempting to rebuild...
inspect 3.1.3-0 is now installed in /tmp/luna-rocks-demo/.luna/rocks (license: MIT <http://opensource.org/licenses/MIT>)
luna: locked 1 rock -> /tmp/luna-rocks-demo/luna.lock
```

No `git init` or other initialization is needed: the first `install` creates the project tree in place and writes the lock. Code can `require` immediately — the tree paths are injected automatically at startup in all three modes (`REPL`, script, and `-e`) with no configuration:

```console
$ luna -e 'local inspect = require("inspect"); print(inspect({a=1}))'
{
  a = 1
}
$ luna list
Rocks installed for Lua 5.5 in /tmp/luna-rocks-demo/.luna/rocks
---------------------------------------------------------------

inspect
   3.1.3-0 (installed) - /tmp/luna-rocks-demo/.luna/rocks/lib/luarocks/rocks-5.5
```

To hand the project to someone else (or move machines), carry `luna.lock` (optionally plus `.luna/cache`) and rebuild exactly with `--from-lock`:

```console
$ rm -rf .luna/rocks                 # simulate a new machine: the installed tree is gone
$ luna install --from-lock
inspect 3.1.3-0 depends on lua >= 5.1 (5.5-1 provided by VM: success)
luna: installed inspect 3.1.3-0
luna: lock reproduced (1 rocks)
```

Day to day, there are four commands:

| Command | Effect |
| --- | --- |
| `luna install <rock> ...` | Install one or more rocks into the project tree, then write `luna.lock` |
| `luna install --from-lock` | Reproduce the whole tree exactly from `luna.lock` (with sha verification) |
| `luna search <term>` | Query luarocks.org |
| `luna list` | List installed rocks in the project tree |
| `luna update [rock]` | Reinstall to the latest version and relock; without an argument, relock everything in the lock |

You do not need a system luarocks: LuaRocks source (v3.13.0) is vendored as a luna submodule, and `luna install` runs it **in-process** inside luna's own Lua VM, calling external tools such as curl/unzip when needed.

## How versions are pinned

`install` **does not take a version argument** — it always installs the latest version and writes the exact version that landed into `luna.lock`. The discipline matches `package-lock.json`: **the lock is the pin**. Upgrade with `luna update` (all or one), which relocks; to stay on a version, simply do not run `update`.

## Project tree layout

Starting from the current directory, luna walks upward looking for `.luna/rocks` (the same habit as bare-name resolution through `luna_modules`), and creates it in the current directory if absent:

```
myproject/
├── luna.lock            ← lock file, beside .luna/ (analogous to package.json)
└── .luna/
    ├── rocks/           ← LuaRocks --tree: share/lua/5.5/*.lua, lib/lua/5.5/*.so
    └── cache/           ← downloaded .src.rock cache, the source for offline reproduction
```

At startup (REPL/script/`-e` alike), if `.luna/rocks` exists in the upward search chain, its `share/lua/5.5` and `lib/lua/5.5` are **prepended** to `package.path`/`package.cpath`, so installed rocks are immediately requirable (including from subdirectories, because the tree is found by walking upward).

**What to commit** (recommended; luna does not enforce it): commit `luna.lock` — it is the authoritative record of what is installed; do not commit `.luna/rocks` (equivalent to `node_modules`); `.luna/cache` may be committed so `--from-lock` can reproduce fully offline (a cache sha that disagrees with the lock is rejected, so stale cache cannot slip in).

## luna.lock

After each `install`/`update`, the whole tree (with its dependency closure) is snapshotted into `luna.lock`:

```json
{
  "lua":"5.5",
  "rocks":[{
      "rockspec":"dac6f2b2…(sha256)",
      "version":"3.1.3-0",
      "name":"inspect"
    }],
  "version":1
}
```

The two hashes serve different purposes:

- **`rockspec` (required)** — the sha256 of that rock's rockspec file in the tree. The rockspec records the source URL and exact version, making it the authoritative fingerprint of what is installed;
- **`sha256` (best effort)** — the sha256 of the `.src.rock` source archive, stored in `.luna/cache` at the same time. Not every rock publishes a `.src.rock` (git-only rocks such as `inspect` above do not), so those are locked by rockspec only.

`install --from-lock` reproduces in this order:

1. **Offline first**: if the lock has a `sha256` and the cached archive matches, install from cache without any network access;
2. **Online fallback**: without a cache (or when the rock has no source archive), install by `name + exact version`;
3. **Verification**: after installation the rockspec sha256 in the tree must match the lock exactly, otherwise installation is rejected immediately. The lock guarantees not merely "it installs" but "it is exactly that artifact."

## Native rocks

Rocks containing C code (such as lpeg or lua-cjson) install the same way: luarocks compiles them locally, places the `.so` under the tree's `lib/lua/5.5`, and startup injection adds it to `package.cpath`, so `require` behaves identically. A compiler toolchain (gcc/make) must be present — that step belongs to upstream luarocks, and luna does not replace it.

## Division of labor with luna_modules/

luna has two dependency channels with different responsibilities:

| | `luna_modules/` | `.luna/rocks` |
| --- | --- | --- |
| What | Directory packages written by you (or your team) | Ecosystem packages from luarocks.org |
| Versioning | The directory is the version (managed by git) | Managed by `luna.lock` |
| Resolution | Node-style bare-name upward search; see [Module system](/guide/modules) | Prepend injection into `package.path` |
| Typical content | Project business modules | Third-party libraries such as inspect and lpeg |

They coexist; which one `require` sees first is determined by injection order (the rocks tree is prepended before the default path), so ecosystem packages win name collisions — do not use the same name for both.

## Why LuaRocks

| Option | Ecosystem | Effort | Fit with luna | Verdict |
| --- | --- | --- | --- | --- |
| **Wrap LuaRocks (chosen)** | Indexed by lua.org, thousands of rocks available | Thin wrapper: four subcommands plus tree/lock file, about 500 lines of Lua | A rock is a Lua package and is naturally compatible with `package.path` | ✅ Build on a mature ecosystem |
| Write a registry client | Build another repository and package format | Repository, uploads, signing, indexing, conflict resolution… all long-term burdens | No existing packages; every package must be rewritten | ❌ Violates "use mature libraries, do not reinvent" |
| Port the npm client | Reuse npm's package **format**, not its package **content** | Rewrite resolver/semver/installer while Lua packages still publish through LuaRocks | Two repositories coexist; npm has no Lua code | ❌ The shell without the goods |

In one sentence: package **content** lives on luarocks.org; the client is only the delivery mechanism — wrapping the existing one gives access to the most packages.

## Known boundaries

- `install --from-lock` and `update` need network access (or a warm `.luna/cache`); `list` and `require` do not;
- Commands are one-shot CLI calls, and luarocks' interactive confirmation for missing dependencies continues automatically on non-TTY stdin;
- Parts of the Lua 5.5 index on luarocks.org are still sparse, so `search` may return few results — an upstream indexing issue that does not affect installation (which uses the general query path).
