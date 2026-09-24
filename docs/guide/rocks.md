# 包管理

luna 的包管理接在 **LuaRocks** 上——Lua 官方生态(lua.org 主仓库收录)的事实标准仓库,相当于 Lua 界的 npm。luna 不自研 registry 客户端,只做一层薄包装:把 `luarocks` 的常用子命令变成 `luna` 自己的命令,并把依赖**锁进项目树**,保证离线可复现。

## 四个命令

| 命令 | 作用 |
| --- | --- |
| `luna install <rock> ...` | 安装 rock 到项目树,随后写 `luna.lock` |
| `luna install --from-lock` | 按 `luna.lock` 精确复现整棵树(sha 校验) |
| `luna search <词>` | 在 luarocks.org 查询 |
| `luna list` | 列出项目树里已装的 rock |
| `luna update [rock]` | 重装到最新版并重新锁 |

```console
$ luna install inspect
$ luna list
$ luna install --from-lock
```

不需要系统里装 luarocks:LuaRocks 的源码(v3.13.0)作为子模块 vendor 进 luna,`luna install` 在 luna 自己的 Lua 虚拟机里**进程内**运行它,curl/unzip 等外部工具按需调用。

## 项目树布局

从当前目录起逐级上溯找 `.luna/rocks`(与 `luna_modules` 的裸名上溯同一套习惯),找不到时在当前目录新建:

```
myproject/
├── luna.lock            ← 锁文件,与 .luna/ 平级(类比 package.json)
└── .luna/
    ├── rocks/           ← LuaRocks --tree:share/lua/5.5/*.lua、lib/lua/5.5/*.so
    └── cache/           ← 下载的 .src.rock 缓存,离线复现的数据来源
```

启动时(`REPL`/脚本/`-e` 均一样)若上溯链上有 `.luna/rocks`,它的 `share/lua/5.5` 与 `lib/lua/5.5` 会**前置**进 `package.path`/`package.cpath`,所以装完直接 `require`:

```lua
local inspect = require("inspect")   -- 来自 .luna/rocks
```

## luna.lock

每次 `install`/`update` 之后,整棵树(依赖闭包已随之而来)被快照成 `luna.lock`:

```json
{
  "version": 1,
  "lua": "5.5",
  "rocks": [
    {
      "name": "inspect",
      "version": "3.1.3-0",
      "rockspec": "dac6f2b2…(sha256)",
      "sha256": "8f47c1d1…(sha256,可省)"
    }
  ]
}
```

两个哈希各有分工:

- **`rockspec`(必有)**——树上该 rock 的 rockspec 文件的 sha256。rockspec 里写着源码 URL 与精确版本,所以它是"装的是什么"的权威指纹;
- **`sha256`(尽力而为)**——`.src.rock` 源码包的 sha256。并非所有 rock 都发布 `.src.rock`(纯 git 源的没有),这类只按 rockspec 锁。

`install --from-lock` 的复现顺序:

1. **离线优先**:`.luna/cache` 里有缓存源码包且 sha256 与锁一致 → 直接从缓存安装,全程不出网;
2. **在线兜底**:无缓存时按 `名字 + 精确版本` 安装;
3. **校验收口**:装完后树上 rockspec 的 sha256 必须与锁里的完全一致,不一致立即拒绝——锁保证的不是"装得上",是"装的就是那个"。

## 选型:为什么是 LuaRocks

| 方案 | 生态 | 工程量 | 与 luna 的贴合 | 结论 |
| --- | --- | --- | --- | --- |
| **包装 LuaRocks(采用)** | lua.org 官方主仓库收录,数千 rock 现成可用 | 薄包装:四个子命令 + 树/锁文件,约 400 行 Lua | rock 即 Lua 包,`package.path` 天然兼容 | ✅ 站在成熟生态上 |
| 自研 registry 客户端 | 从零建仓库与包格式 | 仓库、上传、签名、索引、冲突解析……全是长期负担 | 生态为零,每个包都要重写 | ❌ 违背"找成熟库、不自己写" |
| 移植 npm 客户端 | 复用 npm 的包**格式**,不是 npm 的包**内容** | 重写 resolver/semver/安装器,Lua 包仍要靠 LuaRocks 发布 | 两套仓库并存,npm 里没有 Lua 代码 | ❌ 拿到的壳,丢掉的货 |

一句话:包的**内容**在 luarocks.org,客户端只是取货方式——包装现成的,货才最多。

## 已知边界

- `install --from-lock` 与 `update` 需要网络(或 `.luna/cache` 已热);`list`、`require` 不需要;
- 命令是一次性 CLI 调用,luarocks 对缺失依赖的交互确认在非 TTY stdin 下自动继续;
- Lua 5.5 在 luarocks.org 的部分索引查询尚稀疏,`search` 结果偏少属上游索引问题,不影响 install(安装走通用查询)。
