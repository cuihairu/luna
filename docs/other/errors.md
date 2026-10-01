# 错误模型与退出码

Node 的错误是带 `code` 字段的 Error 对象;luna 刻意简化:**错误就是字符串**(Lua 惯例),但**什么时候抛、什么时候返回、往哪条流去**有统一契约。

## 同步面:两类错误,两种出场

贯穿标准库的二分([标准库总览](/stdlib/index)的贯穿契约):

| | 数据错误 | 参数错误 |
| --- | --- | --- |
| 例 | `json.decode("{oops")` | `json.decode(42)` |
| 出场 | `nil, err`(消息带行列) | **抛**(`luaL_error`) |
| 处置 | 显式接第二返回值,或 `pcall` | 是调用方的 bug,让它崩 |

格式类模块(json/csv/ini/toml/yaml/xml)遵守第一行。`fs` 拆两半:便利层(读文件、列目录等 Node 式 API)按 Node 风格**抛**(消息含原因),lfs 透传面保持 lfs 自身的 `nil, err`;`net` 同步面透传 luasocket 的 `nil, err` 风格。哪个模块哪种风格,以各模块页的契约节为准——规律是:**包装层学 Node,透传层守上游**。

## 异步面:回调首参,字符串直出

`loop` 各族(`loop.fs`/`loop.net`/`loop.dns`/`loop.process`…)沿用 Node 的"回调首参"风格,但错误值是**字符串**而非 Error 对象——libuv 的 `uv_strerror` 直出(`no such file or directory`、`connection refused`、`address already in use`):

```lua
loop.fs.stat("/nope", function(err, st)
    if err then print(err) end   -- "no such file or directory"
end)
```

判定一律 `err ~= nil`,不需要类型分派。

**回调隔离**:回调里抛错只打到 stderr,**循环继续**——和 REPL 隔离单次求值错误同款(Node 的 uncaughtException 不退出的行为)。代价是错误不回传:回调归回调所有,抛错而忘记 `close()` 会让 `run()` 一直等下去。

**同步抛与异步交付的分界**:会等待的操作(连接、读文件、解析域名)错误走回调;瞬间完成的操作(绑定/监听、`loop.os` 系统信息)错误直接抛——绑定失败没有"之后"可等。

## 未捕获错误:进程级

- 脚本主块以错误收场 → 消息写 stderr(带 traceback),进程退出;
- `loop` 脚本尾部自动排水同样如此:**主块出错不排水**,和 Node 的未捕获异常直接退出一致;
- REPL 与 attach 会话里单次求值的错误只结束那一次求值(attach 的报错经 pcall 隔离,不打倒目标进程)。

## 打错字提示

运行时错误若标注 `(global 'x')` 且 `x` 确实未定义,错误下方给出编辑距离最近的全局名建议——`hint: did you mean 'print'?`(候选歧义时报两个)。REPL 与脚本模式都有。

## 退出码

| 码 | 含义 |
| --- | --- |
| `0` | 正常结束(含脚本尾部排水完成) |
| `1` | 脚本/`-e` 求值以错误收场;文件打不开;attach 目标失联 |
| `130` | `^C` 中断(128 + SIGINT)——脚本、`-e`、事件循环三种场景同一口径;`luna serve` 收到 `^C` 亦然 |

判据是错误消息里的 `interrupted` 字样(`luna -e` 未完整代码、REPL 求值错误等普通错误不受影响)。shell 侧可以像对待任何 Unix 工具一样按退出码分支。

## attach 通道上的错误

`luna --attach` 的应答以 `\30` 封帧:`OK` 帧走 stdout、`ERR` 帧(完整 traceback)走 stderr——管道里拿到的是干净的结果流;目标崩溃时客户端得到 `attach: target went away`。见 [CLI 与 REPL](/guide/cli-repl#attach-接入一个运行中的-luna)。
