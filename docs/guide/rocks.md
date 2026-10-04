# 包管理

luna 的包管理接在 **LuaRocks** 上——Lua 官方生态(lua.org 主仓库收录)的事实标准仓库,相当于 Lua 界的 npm。luna 不自研 registry 客户端,只做一层薄包装:把 `luarocks` 的常用子命令变成 `luna` 自己的命令,并把依赖**锁进项目树**,保证离线可复现。

## 一个完整循环(实录)

空目录里走一遍"装 → 用 → 锁 → 复现",输出为实录:

```console
$ mkdir rocks-demo && cd rocks-demo
$ luna install inspect
Installing https://luarocks.org/inspect-3.1.3-0.rockspec
inspect 3.1.3-0 depends on lua >= 5.1 (5.5-1 provided by VM: success)
No existing manifest. Attempting to rebuild...
inspect 3.1.3-0 is now installed in /tmp/luna-rocks-demo/.luna/rocks (license: MIT <http://opensource.org/licenses/MIT>)
luna: locked 1 rock -> /tmp/luna-rocks-demo/luna.lock
```

不需要 `git init` 或任何初始化:第一条 `install` 就地建出项目树并写下锁。代码里直接 `require`——启动时树里的路径已自动注入(`REPL`/脚本/`-e` 三种模式一样),不用设置任何东西:

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

把目录交给别人(或换一台机器),只需带走 `luna.lock`(可选再加 `.luna/cache`),`--from-lock` 精确重建:

```console
$ rm -rf .luna/rocks                 # 模拟新机器:装出来的树不存在
$ luna install --from-lock
inspect 3.1.3-0 depends on lua >= 5.1 (5.5-1 provided by VM: success)
luna: installed inspect 3.1.3-0
luna: lock reproduced (1 rocks)
```

日常就这四条命令:

| 命令 | 作用 |
| --- | --- |
| `luna install <rock> ...` | 安装 rock 到项目树(可一次多个),随后写 `luna.lock` |
| `luna install --from-lock` | 按 `luna.lock` 精确复现整棵树(sha 校验) |
| `luna search <词>` | 在 luarocks.org 查询 |
| `luna list` | 列出项目树里已装的 rock |
| `luna update [rock]` | 重装到最新版并重新锁;无参 = 锁内全部 |

不需要系统里装 luarocks:LuaRocks 的源码(v3.13.0)作为子模块 vendor 进 luna,`luna install` 在 luna 自己的 Lua 虚拟机里**进程内**运行它,curl/unzip 等外部工具按需调用。

## 版本怎么钉

`install` **不收版本参数**——它永远装最新版,然后把实际落地的精确版本写进 `luna.lock`。纪律模型和 `package-lock.json` 一致:**锁即钉**。日常升级靠 `luna update`(全部或单个),升级即重锁;想停在某个版本,不跑 `update` 就是了。

## 项目树布局

从当前目录起逐级上溯找 `.luna/rocks`(与 `luna_modules` 的裸名上溯同一套习惯),找不到时在当前目录新建:

```
myproject/
├── luna.lock            ← 锁文件,与 .luna/ 平级(类比 package.json)
└── .luna/
    ├── rocks/           ← LuaRocks --tree:share/lua/5.5/*.lua、lib/lua/5.5/*.so
    └── cache/           ← 下载的 .src.rock 缓存,离线复现的数据来源
```

启动时(`REPL`/脚本/`-e` 均一样)若上溯链上有 `.luna/rocks`,它的 `share/lua/5.5` 与 `lib/lua/5.5` 会**前置**进 `package.path`/`package.cpath`,所以装完直接 `require`(子目录里跑也一样,上溯找树)。

**git 里提交什么**(建议,luna 不强制):`luna.lock` 必进——它是"装的是什么"的权威记录;`.luna/rocks` 不进——等价于 `node_modules`,装出来的东西;`.luna/cache` 可进——进了之后 `--from-lock` 在无网环境也能完整复现(缓存 sha 与锁不符会被拒绝,不存在"脏缓存混进来")。

## luna.lock

每次 `install`/`update` 之后,整棵树(依赖闭包已随之而来)被快照成 `luna.lock`:

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

两个哈希各有分工:

- **`rockspec`(必有)**——树上该 rock 的 rockspec 文件的 sha256。rockspec 里写着源码 URL 与精确版本,所以它是"装的是什么"的权威指纹;
- **`sha256`(尽力而为)**——`.src.rock` 源码包的 sha256,写入时源码包同时存进 `.luna/cache`。并非所有 rock 都发布 `.src.rock`(纯 git 源的没有,如上例的 inspect),这类只按 rockspec 锁。

`install --from-lock` 的复现顺序:

1. **离线优先**:锁里有 `sha256` 且 `.luna/cache` 里的缓存源码包哈希一致 → 直接从缓存安装,全程不出网;
2. **在线兜底**:无缓存(或该 rock 本就无源码包)时按 `名字 + 精确版本` 安装;
3. **校验收口**:装完后树上 rockspec 的 sha256 必须与锁里的完全一致,不一致立即拒绝——锁保证的不是"装得上",是"装的就是那个"。

## 原生 rock

含 C 代码的 rock(lpeg、lua-cjson 一类)照样装:luarocks 现场编译,`.so` 落进树的 `lib/lua/5.5`,启动注入时同样进了 `package.cpath`,`require` 无差别。前提是机器上有编译工具链(gcc/make)——这一步是上游 luarocks 的活,luna 不代劳。

## 与 luna_modules/ 的分工

luna 有两条依赖通道,管的事不同:

| | `luna_modules/` | `.luna/rocks` |
| --- | --- | --- |
| 装什么 | 自己(或团队)手写的目录包 | luarocks.org 的生态包 |
| 版本管理 | 目录即版本(git 管) | `luna.lock` 管 |
| 解析规则 | Node 式裸名上溯,见[模块系统](/guide/modules) | `package.path` 前置注入 |
| 典型内容 | 项目内部的业务模块 | inspect、lpeg 这类第三方库 |

两者可共存,`require` 先看到谁由注入顺序决定(rocks 树前置在默认 path 之前)——重名时生态包赢,别给两者起同一个名。

## 选型:为什么是 LuaRocks

| 方案 | 生态 | 工程量 | 与 luna 的贴合 | 结论 |
| --- | --- | --- | --- | --- |
| **包装 LuaRocks(采用)** | lua.org 官方主仓库收录,数千 rock 现成可用 | 薄包装:四个子命令 + 树/锁文件,约 500 行 Lua | rock 即 Lua 包,`package.path` 天然兼容 | ✅ 站在成熟生态上 |
| 自研 registry 客户端 | 从零建仓库与包格式 | 仓库、上传、签名、索引、冲突解析……全是长期负担 | 生态为零,每个包都要重写 | ❌ 违背"找成熟库、不自己写" |
| 移植 npm 客户端 | 复用 npm 的包**格式**,不是 npm 的包**内容** | 重写 resolver/semver/安装器,Lua 包仍要靠 LuaRocks 发布 | 两套仓库并存,npm 里没有 Lua 代码 | ❌ 拿到的壳,丢掉的货 |

一句话:包的**内容**在 luarocks.org,客户端只是取货方式——包装现成的,货才最多。

## 已知边界

- `install --from-lock` 与 `update` 需要网络(或 `.luna/cache` 已热);`list`、`require` 不需要;
- 命令是一次性 CLI 调用,luarocks 对缺失依赖的交互确认在非 TTY stdin 下自动继续;
- Lua 5.5 在 luarocks.org 的部分索引查询尚稀疏,`search` 结果偏少属上游索引问题,不影响 install(安装走通用查询)。
