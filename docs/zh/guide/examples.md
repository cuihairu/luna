# 示例与脚手架

起步资产两件:仓库里 `examples/` 的八个官方示例,和内置模板的 `luna new` 脚手架。示例随仓库分发,模板随二进制分发——新机器上不装任何东西就能从两者起步。

## 官方示例

每个示例一个目录,入口统一 `main.lua`;下面所有输出都是真实抓取。一次性进程跑完即退,常驻进程 `^C` 停(退出码 130)。

| 示例 | 演示 | 运行 |
| --- | --- | --- |
| [hello-cli](https://github.com/cuihairu/luna/tree/main/examples/hello-cli) | argparse 选项/旗标/`-h` 用法页 | `luna examples/hello-cli/main.lua -- -n luna a b` |
| [http-server](https://github.com/cuihairu/luna/tree/main/examples/http-server) | `http.serve` 可编程服务:路由、`res:json`、query、500 契约 | `luna examples/http-server/main.lua [端口]`,另终端 curl |
| [tcp-server](https://github.com/cuihairu/luna/tree/main/examples/tcp-server) | `net.serve` 行协议服务:handler 每连接一调、错误不倒服 | `luna examples/tcp-server/main.lua [端口]`,另终端 nc |
| [file-tool](https://github.com/cuihairu/luna/tree/main/examples/file-tool) | 同步 `fs` 读 + 表格输出,wc 形状 | `luna examples/file-tool/main.lua <file>…` |
| [web-scraper](https://github.com/cuihairu/luna/tree/main/examples/web-scraper) | `http.request` 抓页 + 模式匹配抽 `<title>` 与链接(仅 http://) | `luna examples/web-scraper/main.lua http://…` |
| [build-tool](https://github.com/cuihairu/luna/tree/main/examples/build-tool) | 分阶段数据构建:data/*.csv → dist/*.json,计时、单段运行 | 在 `examples/build-tool/` 下 `luna main.lua` |
| [game-script](https://github.com/cuihairu/luna/tree/main/examples/game-script) | 纯数据表的游戏逻辑:房间/物品/动词表 + json 存档 | 管道喂动词即可,见下 |
| [automation](https://github.com/cuihairu/luna/tree/main/examples/automation) | `fs.watch` 目录监听 + `loop.run()` 常驻 | `luna examples/automation/main.lua <目录>` |

hello-cli 的一次会话——脚本的旗标写在 `--` 之后(`luna` 入口先吃自己的旗标),`extra` 是位置参数:

```text
$ luna examples/hello-cli/main.lua -- -n luna a b
hello-cli: greeting issued
hello, luna, a b!
```

file-tool 的多文件汇总,`total` 行在两个文件以上出现:

```text
$ luna examples/file-tool/main.lua README.md todo.md
    105     541    7791  README.md
   1961    8939  140536  todo.md
   2066    9480  148327  total
```

build-tool 的三段流水,`compile` 一段跑完即产出 `dist/*.json`(`luna main.lua compile` 只跑一段):

```text
clean    ok  0.000s
compile  ok  0.001s
  orders.json      100 bytes
  users.json       107 bytes
report   ok  0.000s
```

game-script 的状态都在表里(`rooms`/`state`/`verbs` 三张表,引擎一个循环),stdin 喂动词、存档经 json 落盘,`load` 读回同一份状态:

```text
$ printf 'look\nnorth\ntake key\nsouth\nup\nsave\nquit\n' | luna examples/game-script/main.lua
luna game-script — 'help' lists verbs, 'quit' leaves.
You are in hall: a pillared hall. Dust, and one flickering lamp.
> You are in hall: a pillared hall. Dust, and one flickering lamp.
> You are in study: a cramped study. A desk, an open drawer.
here: key (a small brass key)
> taken: key
> You are in hall: a pillared hall. Dust, and one flickering lamp.
carrying: key
> You are in tower: the tower top. Wind, and the whole valley below.
carrying: key
> saved to luna-save.json
> bye.
$ cat luna-save.json
{"inv":["key"],"room":"tower"}
```

automation 盯一个目录,建立/改动/改名/删除逐事件交付(Linux 上是 inotify 直连),`^C` 以 130 收场:

```text
$ luna examples/automation/main.lua /tmp/watchme &
watching /tmp/watchme — touch files there; ^C stops (exit 130)
18:25:48  rename   new.txt
18:25:48  change   new.txt
^C
examples/automation/main.lua:30: interrupted
```

web-scraper 只有 http://(luasocket 客户端面无 TLS,TLS 客户端走 [`loop.http` 的 connectTls](/guide/loop#loopnettlsconnecttls-listentls));离线走查配 `luna serve <目录>` 即可——对 JSON API 抓取时 `title` 是 `(none)`、`links` 计零,页面失败连接被拒时退出码 1。

### 常驻三例的 ^C 契约

http-server / tcp-server / automation 都是"起服后等 ^C"的形状,背后是同一条循环契约:静默循环阻塞在 epoll 里时,libuv 内部重试 EINTR 不结束迭代,prepare 钩子看不到中断旗标([loop 页的 `^C` 条目](/guide/loop))——循环得自己转。三例各取一种口径:http-server 用 `http.serve` 内置的 100ms 心跳;tcp-server 的 `net.serve` 给 accept 挂 250ms 期限;automation 自己挂 100ms 心跳。示例即范本,照抄即可。

## luna new:内置脚手架

```text
luna new <template> <name>    从内置模板生成起步代码
luna new --list               列模板
```

| 模板 | 生成物 | 下一步 |
| --- | --- | --- |
| `plugin` | `<name>/plugin.json` + `<name>/init.lua`(注册 `%<name>` 魔法命令) | 目录挪进 `./plugins/`(或 `~/.luna/plugins`),`luna` 起来即生效 |
| `package` | `<name>/package.json` + `<name>/init.lua` | 目录挪进 `<项目>/luna_modules/`,任何位置 `require("<name>")` |
| `script` | `<name>.lua`(参数即 chunk 变参,`-h` 出用法) | `luna <name>.lua world` |

三条硬规则:**目标已存在即拒**(`refusing to overwrite`,不覆盖任何字节);名字只收字母/数字/`.`/`_`/`-` 且禁 `..`(名字要落路径和 JSON,先挡住逃逸);模板没有名字时报用法并列出模板清单(退出码 1)。

一次完整会话——生成、放进 `plugins/`、`%plugins` 验证、魔法命令开火:

```text
$ luna new plugin hello
created plugin 'hello':
  hello/plugin.json
  hello/init.lua
next: move hello/ into ./plugins/ (or ~/.luna/plugins), then run `luna` and try %hello
$ mkdir -p plugins && cp -r hello plugins/
$ luna
In [1]: %plugins
loaded plugins:
  hello 0.1.0  hello — a luna plugin
In [2]: %hello
hello from the hello plugin
```

package 模板落到 `luna_modules/` 后,解析规则见[模块系统](/guide/modules);plugin 模板的四个扩展点与失败隔离见[插件开发](/guide/plugins)。模板只是起步代码——生成之后,内容的所有权是你的。
