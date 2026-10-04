# examples

luna 官方示例,八个可直接跑的小程序——每个示例一个目录,入口统一 `main.lua`,
运行形如 `luna examples/<name>/main.lua`(以仓库根为工作目录时)。

| 示例 | 演示 | 运行 |
| --- | --- | --- |
| hello-cli | argparse 选项/旗标/`-h` 用法页 | `luna examples/hello-cli/main.lua -- -n luna a b`(脚本旗标须在 `--` 之后,`luna` 入口先吃旗标) |
| http-server | `http.serve` 可编程服务:路由、`res:json`、query、500 契约 | `luna examples/http-server/main.lua 8000`,另终端 `curl 'localhost:8000/api/echo?q=hi'` |
| tcp-server | `net.serve` 行协议服务:handler 每连接一调、错误不倒服 | `luna examples/tcp-server/main.lua 9000`,另终端 `echo time \| nc 127.0.0.1 9000` |
| file-tool | 同步 `fs` 读 + 表格输出,wc 形状的工具 | `luna examples/file-tool/main.lua README.md todo.md`(仓根) |
| web-scraper | `http.request` 抓页 + 模式匹配抽 `<title>` 与链接(仅 http://) | `luna examples/web-scraper/main.lua http://localhost:8000/` |
| build-tool | 分阶段数据构建:data/*.csv → dist/*.json,计时与单段运行 | 在 `examples/build-tool/` 下 `luna main.lua` |
| game-script | 纯数据表的游戏逻辑:房间/物品/动词表 + json 存档 | `printf 'look\nnorth\ntake key\nsouth\nup\nsave\nquit\n' \| luna examples/game-script/main.lua` |
| automation | `require("loop").fs` 的 `fs.watch` 目录监听 + `loop.run()` | 先 `mkdir -p /tmp/watchme`,`luna examples/automation/main.lua /tmp/watchme` |

要点:

- http-server / tcp-server / automation 是常驻进程,`^C` 停(退出码 130);
  web-scraper 对着本机起一个 `luna serve <目录>` 即可离线走查。
- game-script / build-tool / file-tool / hello-cli 是一次性进程,stdin 管道喂
  命令即可走查(game-script 的输入就是它的动词表)。
- 起新项目用脚手架:`luna new plugin|package|script <name>`(见
  [示例与脚手架](https://cuihairu.github.io/luna/guide/examples))。
