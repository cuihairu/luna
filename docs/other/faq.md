# FAQ

## 安装与运行

**macOS 上双击/直接运行 luna 被拦截?**

包未做签名,先清隔离属性(包内 PLATFORM-NOTES.txt 有同款说明):

```bash
xattr -cr luna
```

**`require("crypto")` 报错?**

这是构建期开关,不是 bug:缺 OpenSSL 开发头文件时构建照常成功,crypto 面整体不可用,报错里直接给补构建指引。装 `libssl-dev`(macOS `brew install openssl@3`)后重新 configure 即可,见[构建与自测](/other/build)。

**Windows 怎么构建?**

同一套 CMake,`cmake --build build --config Debug`,ctest 同上。macOS 的 `MACOSX_DEPLOYMENT_TARGET` 那条只与 macOS 有关。

## 标准库行为

**为什么 `print(require("json").encode({ok=true}))` 每次输出键序可能不同?**

Lua 哈希表是 `pairs` 序,随进程变化。所有示例与测试都只用序确定的形态(单键、数组)或显式声明"键序以运行时为准"——业务里别对 JSON 对象键序做断言,也别拿它做快照比对。`http.serve` 的 handler 示例同理。

**`json.decode('{"a":null}')` 里 `a` 去哪了?**

JSON null 解码落 Lua `nil`,而 nil 意味着"键不存在",所以 `a` 取不到是**符合语义**的,不是丢数据。`yaml.null` 哨兵同理:只在你用 `opts.nullval = yaml.null` 显式要求保留时才出现,decode 默认不产出它。

**报错只有行号没有列(toml)?/ 行列指的地方不对(yaml)?**

都是后端语义,已钉死:tomlc17 只报行号(`<原因> at line N`);lyaml 丢弃了 libyaml 自己的 problem_mark,坐标是**最后一个成功解析事件的起点**,常常不在出错那一行。csv/ini/json/xml/json 侧是完整行列(列按 UTF-8 字符计)。跨格式时按各页契约条取坐标,别假设统一。

**`csv.lines` 喂了坏数据直接抛错?**

`lines` 是迭代器,nil 表示"读完",没有第二个返回值能带 err,坏输入只能 raise。喂不可信数据时自行 `pcall`,见 [csv](/stdlib/csv) 页尾的姿势。

**stream 的 `end` 监听没触发?**

`on("data")` 挂上的瞬间流动**同步**开跑:数据与 `end` 可能在你挂 `on("end")` 之前就发完了。先挂 `end` 再挂 `data`,见 [stream](/stdlib/stream) 的拉式源示例。

**`s:end(...)` 报语法错?**

`end` 是 Lua 关键字,拼作 `s:end_(...)` 或 `s["end"](s,...)`——注意后者要显式传 `s`。

## 模块与插件

**`return require("x")` 怎么透出去两个值?**

Lua 5.5 的 `require` 在**新加载**时返回两个值(模块本体 + loader 数据),命中缓存时只返回一个。只要模块本体时写 `return (require("x"))` 用括号截断,见[模块系统](/guide/modules)。

**插件装了没生效?**

先看三处:① `%plugins` 有没有它(失败的记入 failed,详情在启动 stderr);② 是否被同名先到的插件遮蔽(overridden 段——项目 `./plugins` 压过用户目录);③ 用 `--no-plugins` 启动一次,确认异常行为确实来自插件。

## 进程与退出

**`^C` 后退出码为什么是 130?**

这是 luna 的全局契约:128 + SIGINT,脚本、`-e`、`serve` 口径一致。`luna serve` 里 `^C` 即停就是这个码;测试里要"起服又收尾"别发信号,用 `srv:close()`。

**`luna serve` 端口被占?**

换个端口,或 handler/测试里传 `port = 0` 随机分配后读 `srv.port` 回来用,见[标准库 · http](/stdlib/http)。

**`--attach` 连不上目标?**

目标若带 `--no-serve` 启动则根本没建 socket;socket 在 `$LUNA_SOCK_DIR/luna-<pid>.sock`(默认 `/tmp`),0600 属主;目标崩溃时客户端得到 `attach: target went away`。attach 细节见 [CLI 与 REPL](/guide/cli-repl)。
