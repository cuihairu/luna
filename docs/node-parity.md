# Node 方向补齐:格式标准库与底层能力选型

这是一篇**规划文档**(先文档后实现,与[事件循环后端设计](/loop-backend-design)同一体例),回答两件事:

- **A. 格式标准库**:`xml` / `yaml` / `toml` / `csv` / `ini` 五个格式模块进内置 stdlib,与 `json` 同级——选哪个后端、API 长什么样、错误怎么报;
- **B. Node 底层能力**:`path` / `timers` / `child_process` / `os` / `util` / `events` / `stream` 逐项盘点——哪些已经在 luna 里(以哪个名字)、哪些是缺口、缺口怎么补、补在哪一层。

选型总原则沿用 [架构设计](/architecture) 的既有纪律:**绑定成熟 C 库**(成熟度 = 规范覆盖 + 实战年头 + 错误报告质量,不是"越 C 越好");**纯 Lua 用于逻辑/策略层**(Node 本尊的 path/events/stream 就是纯 JS,luna 对应放 Lua 层);**单二进制不变**(新 C 依赖一律静态链进 deps/,版本钉死)。批次拆解与验收标准在 `todo.md`(第二十轮立项);本文只管"选什么、为什么、API 长什么样"。

## A. 格式标准库

### 共同口径(与 json 对齐)

`json` 模块的事实契约(dkjson 后端,`lua/modules/json/init.lua`):

1. **decode/encode 失败不抛错,返回 `nil, err`**(err 为字符串)——调用方 `local v, err = m.decode(s)` 自行分派;dkjson 的 encode 对循环引用、decode 对非法文本都是这个形状;
2. **参数类型错才 raise**(传非字符串文本、非表值——程序员工误,`pcall` 接住该修代码);
3. **Node 命名别名只在 json 保留**(`parse`/`stringify`)——新格式在 Node 侧是 npm 包不是内置,不造别名,`decode`/`encode` 是唯一入口。

五个新模块全部沿用 1/2,并加两条:

4. **错误信息带行列**:`<fmt>: <原因> at line N, column M`(文本格式的错误坐标以行列为主;json 的第二返回值是字节偏移,是实现细节,新格式不学);
5. **分发方式与 json 同级**:`require "xml"` 等经 `luna_modules/` 解析——C 后端按 `zlib.core` 模式注册(`register_c_modules` 进 `package.loaded`,glob off),包装层放 `lua/modules/<name>/init.lua`(Node 风格便捷函数、错误口径统一都在这层做);纯 Lua/LPeg 后端按 dkjson 模式直接 staged。C 库的错误信息先在包装层规整成口径 4 的形状再返回。

| 能力 | decode | encode | 流式 |
| --- | --- | --- | --- |
| json(已有) | 值 | 串 | — |
| xml | DOM 表 | 串 | `xml.sax`(SAX 回调透传) |
| yaml | 值 | 串 | `yaml.decodeAll`(多文档;随批次定) |
| toml | 表 | 串 | —(单文档格式) |
| csv | 行数组 | 串 | `csv.lines`(逐行迭代器) |
| ini | 段表 | 串 | —(单文档格式) |

### 对照总表(选型一览)

| 格式 | Node 侧对应 | Lua 生态现有 | luna 选择 | 为什么 |
| --- | --- | --- | --- | --- |
| xml | fast-xml-parser / xml2js(npm,非内置) | lua-expat(lxp,绑 expat)、LuaXML、SLAXML、xml2lua(纯 Lua) | **expat + lua-expat 绑定** | XML 规范的实体/编码/命名空间边界是 C 库的领域;expat 是半个互联网的 XML 栈地基,MIT,错误带行列 |
| yaml | js-yaml / yaml(eemeli) | lyaml(绑 libyaml)、tinyyaml、lua-yaml(纯 Lua 子集) | **libyaml + lyaml 绑定** | YAML 规范复杂度出名(锚点/别名、多文档、块标量、缩进);纯 Lua 实现全是子集,会静默丢语义 |
| toml | @iarna/toml、toml(npm) | toml.lua、lua-toml(纯 Lua) | **tomlc17 绑定(见下方决策记录)** | TOML 规范小,纯 Lua 本可行,但多行字符串/Unicode 转义/整数宽度/日期时间这些边缘,toml-test 全过的 C 实现已担掉,自担不值 |
| csv | csv-parse(csv 包)、papaparse | (无事实标准;penlight 不在 deps) | **LPeg 语法,纯 Lua 层** | CSV 是行导向正则级格式,RFC 4180 一页纸;为零依赖引入 C 库方向反了(LPeg 已在 deps) |
| ini | ini(isaacs)、dotenv | (无事实标准) | **LPeg 语法,纯 Lua 层** | 同上;段/键/注释三件套,百行级语法 |

**为什么 C 绑定不选纯 Lua(对 xml/yaml/toml)**:三条共同理由——(1)**规范覆盖**:C 实现过的是官方测试套件(expat 的测试集、libyaml 随 PyYAML 生态的实战、tomlc17 过 214/466 的 toml-test),纯 Lua 实现的"支持 YAML"通常是支持到作者用过的子集,坏输入静默接受比报错更危险;(2)**错误报告质量**:带行列的错误位置是 C 库解析器结构里现成的,纯 Lua 实现要自己养;(3)**吞吐**:REPL 场景吞吐不是主因(architecture.md 选 dkjson 时已论证过),但脚本场景大 YAML/TOML 的 3–10 倍差距是白得的。

**为什么 csv/ini 反着选 LPeg**:这两格式的全部规范就是字段引用与转义、段/键/注释——LPeg 语法十几行,确定性、可测试、零新依赖;C 化的收益只剩吞吐,而 lpeg 本身就是 C 写的。

### XML → expat(经 lua-expat)

| 候选 | 结论 |
| --- | --- |
| **expat 2.x + lua-expat(lxp)** | **入选**:expat 是 XML 解析器里的事实地基(MIT,James Clark 起源、Expat 项目维护);lua-expat(Kepler 项目)是 Lua 侧最老牌绑定,1.5.2 支持 Lua 5.4(5.5 适配预计是 vendored 小补丁,与 luasocket 等同法,实现期核对);SAX 回调模型天然给流式;错误带 line/column |
| libxml2 | 淘汰:树 API + XPath + 验证 + 序列化全家桶,luna 的 stdlib 面只要 decode/encode——为一个面拖进最大的依赖树与 CVE 跟踪面;Lua 侧绑定碎片化(无事实标准);若未来要 XPath 再立项,不预付 |
| minixml(mxml) | 淘汰:单文件虽美,但命名空间/DTD/编码覆盖弱于 expat,Lua 绑定生态薄;轻量优势被"还要自己验规范覆盖"吃掉 |
| 纯 Lua(LuaXML / SLAXML / xml2lua) | 淘汰:实体展开、编码声明、属性规范化这些边界各自处理不一;没有一家过过系统性的规范测试集 |

**API 草案**:

```lua
local xml = require "xml"

-- decode:文本 → DOM 表。元素 = { tag, attrs, kids },文本节点是 kids 里的普通字符串
local doc = xml.decode([[<person id="7"><name>Ada</name></person>]])
-- doc == { tag = "person", attrs = { id = "7" },
--          kids = { { tag = "name", attrs = {}, kids = { "Ada" } } } }

-- encode:DOM 表 → 文本(属性值强制 tostring,文本节点做 &amp;/&lt;/&lt; 转义)
xml.encode(doc)                      -- 紧凑;opts.indent = 2 给美化输出

-- 流式:SAX 回调透传(lxp 的 handler 模型原样;decode 内部就是用它攒 DOM)
local p = xml.sax {
    StartElement = function(name, attrs) ... end,
    CharacterData = function(text) ... end,
    EndElement   = function(name) ... end,
}
p:parse(chunk) p:parse()             -- 增量喂,空参收尾

-- 错误:xml.decode("<a><b></a>")
--   → nil, "xml: mismatched tag at line 1, column 9"(行列来自 expat,经包装层规整)
```

- **DOM 形状的取舍**:fast-xml-parser 用"标签名做键"的形状,对 Lua 的数组/哈希混合表是歧义源(同名兄弟元素、属性与子元素撞名);`{tag, attrs, kids}` 三件套无歧义、可往返、遍历就是走 `kids`,编码器不用猜。属性值**恒为字符串**(XML 没有类型);
- **命名空间**:前缀原样保留(`<x:a>` 的 tag 是 `"x:a"`),不做解析展开——fast-xml-parser 同款口径,展开版列入"明确不做";
- **流式**:SAX 透传是免费的大文档面(decode 攒 DOM 的同一机制);`xml.sax` 不做第二套抽象,handler 表就是 lxp 的形状。

### YAML → libyaml(经 lyaml)

| 候选 | 结论 |
| --- | --- |
| **libyaml + lyaml** | **入选**:libyaml 是 YAML 规范的参考 C 实现(MIT,PyYAML/libyaml 生态共享);lyaml(gvvaughan,MIT,Lua 5.1–5.4,6.2.8)提供 C 侧快速 decode/emit;5.5 适配同上,vendored 小补丁预期 |
| 纯 Lua(tinyyaml / lua-yaml) | 淘汰:均为子集实现——锚点/别名、块标量缩进、多文档指示器各自缺角;YAML 的歧义输入多,静默错解比报错危险 |
| js-yaml 移植思路 | 淘汰:规范复杂度不以实现语言转移,理由同上 |

**API 草案**:

```lua
local yaml = require "yaml"

yaml.decode("name: luna\nreleases:\n  - 1\n  - 2\n")
--   → { name = "luna", releases = { 1, 2 } }
yaml.encode({ ok = true })           -- → "ok: true\n"

yaml.decodeAll(doc)                  -- 多文档流:--- 分隔,返回数组;随实现批次定去留
```

- **标量类型**:映射→表、序列→数组表、字符串/整数/浮点/布尔→Lua 同名类型;**YAML null → nil**(与 json/dkjson 默认一致);"键在但值为 null"的区分需求用 `opts.nullval = yaml.null`(哨兵表)开启——js-yaml 的 null 语义,但默认关闭,与 json 对齐;
- **锚点/别名**:decode 把别名解成**共享表引用**;encode 对共享引用报循环错误(同 dkjson 的 self-referential 口径);`opts.anchors = true` 的锚点发射列入后续批次,不进 v1;
- **错误**:libyaml parser 错误自带 `problem` + `problem_mark`(行列),包装层规整成共同口径 4。

### TOML → tomlc17(决策记录:派发单写 tomlc99,本选型改推 tomlc17)

**决策记录(2026-09-29)**:派发单指定的 tomlc99(cktan)上游 README 已挂 **"THIS LIBRARY IS OBSOLETE"** 横幅,指向同作者继任者 **tomlc17**——MIT、严格实现 **TOML v1.1**、通过官方 toml-test 验证套件、双文件 amalgamation(`tomlc17.c`/`tomlc17.h`)零外部依赖、C99 兼容(luna 的 C 标准是 C11,直接编)。选 **tomlc17**;若 vendored 编译出现阻碍,回退点是不再演进的 tomlc99(钉死的 vendor 一样能用,只是放弃上游修正),两者 C API 同族,绑定层可平移。

| 候选 | 结论 |
| --- | --- |
| **tomlc17** | **入选**:见上;parser-only——decode 全部现成,**encode 由 luna 自己写 Lua 面**(TOML 序列化规则简单:表/数组/字符串/数字/布尔/日期时间,~120 行;tomlc17 无 encoder 是接受的代价) |
| 纯 Lua(toml.lua / lua-toml) | 淘汰(诚实记录:这是五格式里 C 绑定理由最弱的一个):TOML 1.0 规范本身不大,纯 Lua 可行;淘汰理由是边缘——多行基本字符串/字面字符串、Unicode 转义、64 位整数边界、local 日期时间的组合,toml-test 的 466 个非法用例是现成的边界清单,C 实现已担掉;绑定层反而比格式层薄 |
| tomlc99 | 备选:同上决策记录 |

**API 草案**:

```lua
local toml = require "toml"

toml.decode [[
title = "luna"
[server]
host = "127.0.0.1"
port = 8321
tags = ["repl", "lua"]
]]
--   → { title = "luna", server = { host = "127.0.0.1", port = 8321, tags = {"repl","lua"} } }

toml.encode({ title = "luna" })      -- 标量键在前、表键在后(encode 层重排,不依赖调用方)
```

- **类型映射**:表/数组/字符串/整数/浮点/布尔直映;**datetime 映射为表** `{year, month, day, hour, minute, second, secfrac?, offset?}`(纯日期缺时分秒、纯时间缺年月日,`offset` 是分钟数)——Lua 无日期类型,这个形状 `os.time` 可直接吃(本地时间);encode 认同一形状回写(键集全属该组件集且带 `year` 或 `hour` 才认作 datetime,普通数据表带任一其它键即避开);
- **encode 的键序**:TOML 要求"先标量后表",encode 层负责重排(不依赖调用方);同组之内按 Lua 表序(pairs,不承诺插入序);数组内允许混合类型照实输出;
- **错误**:tomlc17 的错误串带行号(实现期已核对:三种串形统一规整为 `<fmt>: <原因> at line N`,**无列子句**——tomlc17 只报行号,与 csv/ini 的行列口径就这一点显式分叉,guide/modules.md 记账)。

### CSV / INI → LPeg 语法(零新依赖)

| 候选 | 结论 |
| --- | --- |
| **LPeg 语法 + 纯 Lua 包装** | **入选**:两格式的全部规范是字段引用/转义(csv)与段/键/注释(ini);LPeg 语法确定性可测;deps 已有 lpeg(1.1.0,`require "lpeg"` 已注册),**零新依赖** |
| 纯 C 小库(libcsv 等) | 淘汰:百行级 C 代码能干的事引一个新 vendor 方向反了;"绑定成熟 C 库"针对的是规范重的格式,不是格式本身 |
| 纯 Lua 手写循环 | 备选而非入选:带引号字段的 csv 转义状态机手写必错(引号内的分隔符/换行/双引号);LPeg 把这些写进语法,测试钉死 |

**csv API 草案**:

```lua
local csv = require "csv"

csv.decode('name,age\n"Ada,Lovelace",36\n')
--   → { { "name", "age" }, { "Ada,Lovelace", "36" } }
csv.encode({ {"a","b"}, {1,2} })    -- 需要引用的字段自动加引号

csv.decode(text, { headers = true })  -- 首行当表头:{ {name="Ada,Lovelace", age="36"} }
csv.lines(s)                          -- 迭代器(流式:大文件逐行,不整块建表)
-- opts.delimiter = ";"  /  decode 收 CRLF 与 LF,encode 出 "\r\n"(RFC 4180;opts.eol 可改 "\n")
```

- **值恒为字符串**:CSV 无类型(Node csv-parse 同默认);转换是调用方的事(`tonumber`);
- **错误**:未闭合引号等 → `nil, "csv: unterminated quote at line N, column M"`(LPeg 失败位置 → 行列换算,包装层持有一个 offset→line/col 的公共辅助,五个格式模块共用——实现在 `lua/luna/` 策略层或模块内联,随批次定);
- **流式**:`csv.lines` 的迭代器模型天然支持分块喂(`io.lines` / `fs.readFileSync` 后 `:gmatch` 同族)。

**ini API 草案**:

```lua
local ini = require "ini"

ini.decode("[server]\nhost = 127.0.0.1\nport = 8321\n")
--   → { server = { host = "127.0.0.1", port = "8321" } }
ini.encode({ server = { host = "127.0.0.1" } })
```

- **值恒为字符串**(ini 无类型;Node 的 ini 包同款),`opts.cast = true` 可开数字/布尔自动转换;
- 段前裸键进根表(`{ top = "1", server = {...} }`,npm ini 的形状);`#` 与 `;` 注释、`key = "value"` 引号形式、重复键后者覆盖;
- 错误口径同上(坏引号/缺 `]`)。

### 分发与构建(五个格式一起)

- **vendor 方式**:全部走 `deps/` 子模块钉版本(与 luafilesystem/libuv 同法,.gitmodules 登记);expat 与 libyaml 上游自带 CMake(`add_subdirectory(... EXCLUDE_FROM_ALL)`,参考 libuv 段);tomlc17 双文件直接进静态库;lua-expat/lyaml 取其绑定源(`lxp`/`lyaml` 的 luaopen)编译进 `luna_xml`/`luna_yaml` 静态库;
- **注册**:绑定层 `lxp`、`toml.core`、`yaml.core` 进 `register_c_modules`(luna_main.c,package.loaded glob off);包装层 `lua/modules/{xml,yaml,toml,csv,ini}/init.lua` 由既有 `LUNA_STDLIB_WRAPPERS` 的 glob 自动 staged——分发路径零新机制;
- **构建依赖口径**:三个 C 后端都是源码 vendor,**不引入系统库依赖**(与 zlib 需系统 ZLIB、crypto 需 OpenSSL 的口径不同——格式库全自带,离线可复现构建不受影响)。

## B. Node 底层能力逐项盘点

先给结论表(现状经源码核实,2026-09-29,`src/luna_loop.c` 4234 行 / `lua/modules/`):

| Node 能力 | luna 现状 | 缺口 | 补法 | 层 | 批次 |
| --- | --- | --- | --- | --- | --- |
| fs | ✅ 同步 `fs`(lfs + 便捷层)+ 异步 `loop.fs` | — | — | — | — |
| timers | ✅ `loop.setTimeout/setInterval/setImmediate/clear*`(opt-in) | 脚本尾部自动排水、全局化未决 | 见下节 | C 入口 + Lua 收尾 | 8 |
| child_process | ✅ `loop.process.run`(聚合)/`process.spawn`(流式) | `exec` 糖、同步面 | `process.exec` = `run("sh",{"-c",…})` 糖;`execSync` 走 `io.popen`(已启用) | Lua | 8 |
| os | ✅ `loop.os`(hostname/type/home/tmpdir/uptime/loadavg/mem/cpus/networkInterfaces) | `arch`/`release`/`EOL`/`userInfo` | libuv `uv_os_uname`/`uv_os_get_passwd` 接进 loop.os | C(loop.c 内) | 8 |
| path | ❌ 无 | 全部 | 纯 Lua 新模块(POSIX 语义) | Lua | 6 |
| util | ⚠️ `luna.introspect` 是 REPL 内省(签名/help/suggest),不是值格式化 | `util.inspect`/`util.format` | 纯 Lua 新模块 | Lua | 6 |
| events | ❌ 无 | EventEmitter | 纯 Lua 新模块 | Lua | 6 |
| stream | ❌ 无(`sock:read` 等回调流已有) | Readable/Writable/pipe 抽象 | 纯 Lua,基于 events | Lua | 7 |

**"绑定 C 库优先"在这张表上的落法**:timers/child_process/os 的底座已经是 libuv(deps 已在)——B 面的唯一新增 C 代码是 loop.os 的四个 getter(~60 行,仍在既有 `os_funcs` 表里);path/util/events/stream **没有对应的成熟 C 库可绑**(Node 本尊全是纯 JS;libuv 无 path join/EventEmitter),它们落在策略层正合架构分层——"C 内核尽量薄,行为逻辑全是 Lua"。

### timers:事件循环现状核实与建议

**现状(源码核实)**:派发单引用的 README「事件循环推迟」已经演进——[architecture.md](/architecture) 的章节现在是「事件循环:第一批(显式选择,脚本 opt-in)」:`require "loop"` 落地已久,libuv 1.53.0 静态链接,已有 timer/fs(+watch)/net(+TLS)/udp/dns/signal/os/process 八个 C 面 + `loop.http` 纯 Lua 面;prepare 钩子承接 `^C` → `interrupted`(退出码 130)与 attach 轮询。**推迟的对象已经不是"要不要循环",而是"要不要全局化/进 REPL"**——README 概览表那句"事件循环推迟的 rationale"指向上文,表述滞后但不失实。

**建议:启用「脚本尾部自动排水」,继续推迟「全局化与 REPL 集成」。**

1. **启用:脚本尾部自动排水**。Node 契约是"注册了 timer,进程就活着"——`luna script.lua` 现在是"主 chunk 跑完就走",`loop.setTimeout` 没配 `loop.run()` 的定时器被静默丢弃(footgun)。建议:`run_script` 主 chunk 正常结束后,若 loop 已被 require 且仍有活句柄,自动 `loop.run()` 排空——这正是 Node 的"事件循环跑到空退出";`unref` 语义天然支持"在但不留人",既有契约一个不改。实现落点 `luna_main.c` 的脚本路径尾部(探测 package.loaded.loop 的句柄计数,或 loop.c 出一个 `luna_loop_maybe_drain()`),`-e` 模式同口径,REPL 不启用。风险面:只影响"用了 loop 但没调 run()"的脚本——现状是句柄被丢,新行为是句柄被执行,方向只会更对;全部既有 loop 测试必须原样全绿(它们自己调 run,自动排水对它们是无操作)。
2. **继续推迟:全局 `setTimeout`**。全局化逼着 REPL 主循环进循环世界:行编辑的阻塞读、`^C` 的 130 契约、attach 的轮询点全在同步侧,为省一个 `require` 动这三张契约,收益配不上代价;「REPL 每次求值后 drain(nowait)」的形态已记入 todo 下轮方向,条件成熟(行编辑可超时读或唤醒线程可定时)再启。

### path(批次 6,纯 Lua)

Node `path.posix` 面(luna 的平台面是 Linux/macOS——replxx 构建已排除 windows.cxx,win32 面不做):

```lua
local path = require "path"
path.join("a", "b/", "../c")     -- "a/c"
path.resolve("a", "./b")          -- 绝对路径(cwd 锚定,lfs.currentdir)
path.normalize("//a/../b")        -- "/b"
path.basename("/a/b/luna.lua")    -- "luna.lua";带 ext 参去后缀
path.dirname("/a/b/luna.lua")     -- "/a/b"
path.extname("luna.tar.gz")       -- ".gz"
path.relative("/a/b", "/a/c/d")   -- "../c/d"
path.isAbsolute("/x")             -- true
path.parse("/a/b/luna.lua")       -- { dir="/a/b", base="luna.lua", name="luna", ext=".lua", root="/" }
path.format({ dir="/a/b", name="luna", ext=".lua" })
path.sep                          -- "/"
path.delimiter                    -- ":"
```

Node 语义按其文档钉死(resolve 的 cwd 锚定、normalize 保留双前导斜杠、trailing sep 的处理)——每个函数的 Node 行为对照进用例;~150 行纯 Lua,零依赖,**不用 C 的理由:Node 本尊纯 JS,libuv 无对应物**。

### util(批次 6,纯 Lua)

- **`util.inspect(value, opts?)`**:值格式化——`depth`(默认 2,`math.huge` 全展开)、循环引用 `[Circular *1]`(表登记)、长数组截断 `… N more items`(Node 的 breakLength 语义简化为 opts.maxArray/opts.maxString 的个数截断,折行不进 v1)、字符串带引号与转义、函数 `<function: shortsrc>`(经 debug.info)、线程/udata 用 tostring 兜底、键 `["a b"]` 形式按 Lua 语法合法名规则;
- **`util.format(fmt, ...)`**:`%s`/`%d`/`%i`/`%f`/`%x`/`%X`/`%o`(走 util.inspect,Node 的 %o)/`%j`(JSON,后端 dkjson,同树可 require)/`%%`;无格式符时全参数空格连接(Node 同款);超参尾接;
- **与 `luna.introspect` 的分工**:introspect 是 REPL 的**内省**(函数签名、help、doc_for、suggest——查"这个函数怎么用");util.inspect 是**值格式化**(把任意值印成人话——查"这个数据长什么样")。不合并;REPL 的 `Out[n]` 表格回显改用 inspect 是可选的后续打磨(记入 todo,不承诺批次)。
- **不做**:`promisify`/`callbackify`(Lua 无 promise,契约无锚)、`types`(isDeepStrictEqual 一族)。

### events(批次 6,纯 Lua)

```lua
local EventEmitter = require "events".EventEmitter
local em = EventEmitter.new()
em:on("tick", function(n) ... end)      -- on/addListener/once/prependListener/prependOnceListener
em:once("done", fin)
em:emit("tick", 1)                      -- → true/false(有无监听)
em:off("done", fin)                     -- off/removeListener;removeAllListeners(ev?)
em:listeners("tick")                    -- 数组
em:listenerCount("tick")
em:setMaxListeners(20)                  -- 缺省 10(Events.defaultMaxListeners),超限 stderr 警告一次
-- 'newListener'/'removeListener' 内建事件同 Node;emit('error') 无监听 → error() 抛出(Node 同款)
```

~150 行纯 Lua;**不用 C 的理由**同 path(无 C 库可绑,Node 纯 JS);它是 stream 的地基,先于 stream 一个批次。

### stream(批次 7,纯 Lua,基于 events)

**v1 面收窄**(Node stream 全家桶很大,luna 只做被适配器真正消费的面):Readable(`_read` 源 → `push`,`'data'`/`'end'`/`'readable'`、pause/resume)、Writable(`_write` 汇 → `'drain'`/`'finish'`,`write`/`end`)、Duplex、Transform(`_transform`)、`readable:pipe(writable)`(含背压——write 返回 false 即停 push,等 drain 续推,`unpipe`)、`opts.highWaterMark`(缺省 16KiB,只做记账阈值不做字节精确)。事件与错误传播按 Node 语义(error → destroy → 'close'、pipe 上的错误联动)。

**适配器是 stream 的存在理由**(批次 7 内含 2–3 个,其余按需追加):`sock`(loop.net)→ Duplex(读回调转 push、write 转发)、`fs.readFileSync` 的内存块 → Readable、`loop.http` 的 onData → Readable。**明确不做**:webstreams(`ReadableStream` 家族)、异步迭代器(`for ... in s:lines()` 的 Lua 迭代器形态倒是顺手,记为可选)、`setEncoding`/`cork` 一族小面。

### os 补齐(批次 8,C 在 loop.c 内)

`loop.os` 增补,与 Node `os` 的名字对齐(不建顶层 `os` 模块——Lua 全局 `os` 是官方标准库,遮蔽它违背"官方语义不动"的纪律):

| 增补 | 底座 | Node 对照 |
| --- | --- | --- |
| `loop.os.arch()` | `uv_os_uname()->machine`,映射 `x86_64→"x64"`、`aarch64→"arm64"`(Node 的名字,不是 uname 原文) | `os.arch()` |
| `loop.os.release()` | `uv_os_uname()->release` | `os.release()` |
| `loop.os.EOL` | 常量 `"\n"`(POSIX 平台面) | `os.EOL` |
| `loop.os.userInfo()` | `uv_os_get_passwd`:`{username, uid, gid, shell, homedir}` | `os.userInfo()` |
| `loop.os.availableParallelism()` | `uv_available_parallelism` | Node ≥18 同名 |

### child_process 糖(批次 8,Lua)

- `process.exec(cmd, opts?, cb)`:`run("sh", {"-c", cmd}, opts, cb)` 的糖(Node `exec` 的 shell 语义;stdout/stderr 聚合照旧);`execFile` 即 `run` 本尊,文档点名即可;
- `process.execSync(cmd) -> stdout`:~20 行 `io.popen` 糖(`LUA_USE_POSIX` 已开,popen 自带;`fh:close()` 的第二三元组拿退出码,非零 raise)——Node `execSync` 的形状;
- **不做** `spawnSync` 的三流同步面(popen 单流 + `/dev/null` 重定向够用的场景走 execSync;真三流是 `process.spawn` 的场)。
