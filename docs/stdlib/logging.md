# stdlib · logging

[lualogging](https://github.com/lunarmodules/lualogging)(lunarmodules,1.8.x,MIT)的 `logging` 全家随二进制分发,`require` 即用:六级阈值 + log4j 式 appender。核心纯 Lua,上游原样透出、无包装;`logging.socket`/`logging.email` 踩在自带的 luasocket 上,`sql` 的数据库驱动与 `rsyslog`/`nginx` 的宿主属可选项,只在用到时才 require。luna 的 Lua 5.5 上游 CI 尚未覆盖(矩阵止于 5.4),已用上游全套回归在本机验证:三轮全绿、零补丁零改动。

## API

| 调用 | 说明 |
| --- | --- |
| `require "logging"` | 核心:`DEBUG/INFO/WARN/ERROR/FATAL/OFF` 六级常量,`logging.new(append, level)` 自定义 appender,`defaultLevel()`/`defaultLogger()` 管全局缺省 |
| `require "logging.console"` | 控制台 appender;`{ logLevel=, destination="stdout"\|"stderr", logPattern=, timestampPattern= }` |
| `require "logging.file"` | 追加写文件;`{ filename=, logLevel=, datePattern=, logPattern= }` |
| `require "logging.rolling_file"` | 按 `maxFileSize` 滚动、留 `maxBackupIndex` 份档;其余参数同 file |
| `require "logging.socket"` | 每条消息经 luasocket 连 `{ hostname=, port= }` 发往 TCP 对端 |
| logger 方法 | `:debug/:info/:warn/:error/:fatal(msg, ...)`,参数按 `string.format` 展开;`:setLevel()`、`:getPrint()` |

## 级别

`logLevel` 以下的记录静默丢弃,缺省 DEBUG;消息参数走 `string.format`:

```lua
local logging = require "logging"
local log = require "logging.console"({ logLevel = logging.INFO })
log:debug("this one is filtered out")
log:info("up and running")
log:error("boom: %s", "disk full")
```

```text
Sat Oct  3 11:54:35 2026 INFO up and running
Sat Oct  3 11:54:35 2026 ERROR boom: disk full
```

## 格式

`logPattern` 缺省 `%date %level %message\n`,`%date`/`%level`/`%message`/`%source`(调用点的文件名:行号与函数名)可自由组合,`timestampPattern` 走 `os.date`:

```lua
local logging = require "logging"
local log = require "logging.console"({
  logLevel = logging.DEBUG,
  logPattern = "%date [%level] %message\n",
})
log:warn("cache cold")
```

```text
Sat Oct  3 11:54:35 2026 [WARN] cache cold
```

## 文件

`logging.file` 追加写、按行缓冲;低于 `logLevel` 的同样不落盘——`app.log` 里只有那一条 WARN:

```lua
local logging = require "logging"
local log = require "logging.file"({ filename = "app.log", logLevel = logging.WARN })
log:info("not written (below WARN)")
log:warn("written to app.log")
```

```text
Sat Oct  3 11:54:35 2026 WARN written to app.log
```

## 滚动文件

`maxFileSize` 按字节计,每次写入前检查:超过即滚动——`server.log` 改名 `server.log.1`,旧档依次上推,最多留 `maxBackupIndex` 份,主文件重新开写:

```lua
local logging = require "logging"
local log = require "logging.rolling_file"({
  filename = "server.log", maxFileSize = 200, maxBackupIndex = 1,
  logLevel = logging.INFO,
})
for i = 1, 8 do log:info("request %d handled", i) end
```

8 条(每条约 49 字节)写完:前 5 条被 200 字节上限挤进 `server.log.1`,主文件从第 6 条重新计——

```text
server.log
Sat Oct  3 11:54:35 2026 INFO request 6 handled
Sat Oct  3 11:54:35 2026 INFO request 7 handled
Sat Oct  3 11:54:35 2026 INFO request 8 handled
```

格式化失败不丢消息:`string.format` 报错时该条以 `Error formatting log message: … | 栈回溯` 的形式照常交给 appender。`email`/`sql`/`rsyslog`/`nginx` 四个 appender 同在 `logging.` 名下,分别要 smtp 可达、数据库驱动、copas 与 ngx 宿主,属上游可选项;luna 随发保证可达的是 console/file/rolling_file/socket 四件。
