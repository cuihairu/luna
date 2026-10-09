# Node 方向补齐:格式标准库与底层能力选型

这是一篇**规划文档**(先文档后实现,与[事件循环后端设计](/loop-backend-design)同一体例),回答两件事:

- **A. 格式标准库**:`xml` / `yaml` / `toml` / `csv` / `ini` 五个格式模块进内置 stdlib,与 `json` 同级——选哪个后端、API 长什么样、错误怎么报;
- **B. Node 底层能力**:`path` / `timers` / `child_process` / `os` / `util` / `events` / `stream` 逐项盘点——哪些已经在 luna 里(以哪个名字)、哪些是缺口、缺口怎么补、补在哪一层。

选型总原则沿用 [架构设计](/architecture) 的既有纪律:**绑定成熟 C 库**(成熟度 = 规范覆盖 + 实战年头 + 错误报告质量,不是"越 C 越好");**纯 Lua 用于逻辑/策略层**(Node 本尊的 path/events/stream 就是纯 JS,luna 对应放 Lua 层);**单二进制不变**(新 C 依赖一律静态链进 deps/,版本钉死)。批次拆解与验收标准在 `todo.md`(第二十轮立项);本文只管"选什么、为什么、API 长什么样"。

## 标准库一一对照(Node ↔ luna)

口径:Node **v24** 标准库全模块逐个对照;luna 侧以源码注册表为准(`register_c_modules` + `lua/modules/*` + REPL 全局),不凭印象。状态三档:**✅ 对应**(同名能力,API 形状贴 Node)、**◐ 部分**(有对应物但面收窄或形态不同)、**❌ 无对等**(登记缺口——是登记,不是否认;补不补见各批规划)。

| Node 模块 | luna 对应 | 状态 |
| --- | --- | --- |
| `assert` | 官方 `assert` / `error`(语言原语) | ◐ 无 deepEqual 等断言库 |
| `async_hooks` | — | ❌ 单线程回调面无对等钩子 |
| `buffer` | Lua 字符串即 8-bit 串;二进制打包走官方 `string.pack`/`unpack` | ◐ 语言原语覆盖,无独立 Buffer 类型 |
| `child_process` | `loop.process`:`run`(聚合)/`spawn`(流式,stdio 直交 `loop.net` sock)/`exec`/`execSync` | ✅ |
| `cluster` | — | ❌ 多进程可用 `loop.process` 自组 |
| `console` | `print`/`io.write` + REPL 的 `Out[n]` 会话记录 | ◐ |
| `crypto` | `crypto`(luaossl:digest/hmac/cipher/rand/kdf/pkey/x509 全家族) | ✅ 证书面比 Node 内建更深 |
| `dgram` | `loop.udp`(bind 常驻收包、send 按包回调) | ✅ |
| `diagnostics_channel` | — | ❌ |
| `dns` | `loop.dns`(lookup/reverse,线程池解析)+ 同步 `net.dns`(luasocket) | ✅ |
| `domain` | — | ❌(Node 本尊已废弃,不追) |
| `events` | `events`(on/once/off/prepend/listeners/setMaxListeners/'error' 契约) | ✅ 批次 6 |
| `fs` | 同步 `fs`(lfs + 便捷层)+ `loop.fs`(线程池,回调首参)+ `fs.watch` 目录观察 | ✅ |
| `http` | 同步 `http`(client + 一行 serve)+ `loop.http`(1.1 客户端跑在 loop.net 上,onHead/onData 流式,重定向/超时状态机;`http.listen` 服务端) | ✅ 1.1 |
| `http2` | — | ❌ |
| `https` | `loop.http` 走 `net.connectTls`(TLS 客户端) | ✅ |
| `inspector` | `luna --attach`(轮询点观测,非 CDP) | ◐ 形态不同 |
| `module` | 模块系统:`require` + `luna_modules/` 逐级上溯 + 清单 `main` + `luna install`(LuaRocks 包装,`luna.lock` 离线复现) | ✅ npm 的对应物在包管理面 |
| `net` | 同步 `net`(luasocket)+ `loop.net`(connect/listen/connectTls/listenTls,多地址回退) | ✅ |
| `os` | 官方 `os` 不遮蔽 + `loop.os`(hostname/type/arch/release/EOL/userInfo/availableParallelism/uptime/loadavg/mem/cpus/networkInterfaces/home/tmpdir) | ✅ 批次 8 |
| `path` | `path`(normalize/resolve/join/relative/parse/format,Node v24 实证钉版) | ✅ posix 面;`win32` ❌(平台面) |
| `perf_hooks` | `%time` 魔法 + `loop.os` 的 uptime/loadavg | ◐ 无独立高精度计时库面 |
| `process` | 全局 `arg`/`os.getenv` + `loop.process`(子进程)+ `loop.os`(系统信息) | ◐ 无单一大而全的 process 全局 |
| `punycode` / `querystring` | — | ❌(URL 面缺口,见 `url` 行) |
| `readline` | `luna_line`(replxx 桥:编辑/历史/高亮/补全)——REPL 内建,非公共库 | ◐ 形态不同 |
| `repl` | luna REPL 本体(多行续行/补全/高亮/魔法命令/`In[n]`·`Out[n]`) | ✅ |
| `stream` | `stream`(Readable/Writable/Duplex/Transform/pipe 背压;sock/chunks/http 适配器) | ✅ 批次 7,v1 面收窄(无 webstreams/异步迭代器/setEncoding/cork) |
| `string_decoder` | — | —(Lua 字符串原生字节串,无编码流切分问题) |
| `timers` | `loop.setTimeout/setInterval/setImmediate/clear*` + 脚本尾部自动排水 | ✅ 批次 8(非全局:需 `require "loop"`,REPL 集成推迟) |
| `tls` | `loop.net.connectTls/listenTls`(OpenSSL 状态机,证书校验默认开启) | ✅ |
| `trace_events` | — | ❌ |
| `tty` | 内核 `kernel.tty()` + REPL 的 tty 形态 | ◐ |
| `url` | — | ❌(`loop.http` 内部解析请求行/重定向,无公共 url 模块) |
| `util` | `util.inspect`(确定性键序/深度/循环)+ `util.format`(%s %d %f %i %j %%) | ◐ promisify/callbackify/types 无(Lua 无 promise 契约) |
| `v8` | `luna.introspect` + REPL 的 `?expr` 帮助 | ◐ 形态不同(无堆快照面) |
| `vm` | 官方 `load`(chunkname/env 参数) | ◐ 语言原语覆盖 |
| `wasi` | — | ❌ |
| `worker_threads` | — | ❌(并发模型与 Node 同形:单线程事件循环 + 线程池 IO;Lua 协程只做协作式) |
| `zlib` | `zlib`(deflate/gzip 一次性便捷层 + 流式 closure) | ✅ |

**Node 全局对象对照**:`process`→◐(`arg`/`os.getenv`/`loop.os`);`Buffer`→❌(`string` + `string.pack`);`console`→`print`;`URL`/`fetch`/`AbortController`→❌(`loop.http` 是回调式,无 promise 契约);全局 `setTimeout`→◐(`loop.setTimeout`,opt-in,见 timers 行);`TextEncoder`/`TextDecoder`→官方 `utf8` 库。

**反向账**:luna 有而 Node 标准库没有的——IPython 式 REPL(`In[n]`/`Out[n]`/魔法命令/实时高亮补全)、目录插件机制(四扩展点)、`luna.lock` 离线复现装包、`--attach` 跨进程观测、单文件二进制随带全部标准库与包管理器。本表只对齐 Node 标准库,不比 npm 生态。

**覆盖哲学**:❌ 行不是欠账清单,是选型记录——每一条都对应一条「不为行数写代码」的判定(见 `todo.md` 明确不做节)或尚未立项的方向;✅ 行的 API 形状以 Node v24 机器实证钉版(批次 6 的探针勘定见下文 path/util/events 节)。

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
| yaml | 值 | 串 | `yaml.decodeAll`(多文档,已实现) |
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
    StartElement = function(self, name, attrs) ... end,  -- 首参 self(lxp docall 约定)
    CharacterData = function(self, text) ... end,
    EndElement   = function(self, name) ... end,
}
p:parse(chunk) p:parse()             -- 增量喂,空参收尾(lxp 的 parse(s) 不终结流)

-- 错误:xml.decode("<a><b></a>")
--   → nil, "xml: mismatched tag at line 1, column 9"(行列来自 expat,经包装层规整)
```

- **DOM 形状的取舍**:fast-xml-parser 用"标签名做键"的形状,对 Lua 的数组/哈希混合表是歧义源(同名兄弟元素、属性与子元素撞名);`{tag, attrs, kids}` 三件套无歧义、可往返、遍历就是走 `kids`,编码器不用猜。属性值**恒为字符串**(XML 没有类型);
- **命名空间**:前缀原样保留(`<x:a>` 的 tag 是 `"x:a"`),不做解析展开——fast-xml-parser 同款口径,展开版列入"明确不做";
- **流式**:SAX 透传是免费的大文档面(decode 攒 DOM 的同一机制);`xml.sax` 不做第二套抽象,handler 表就是 lxp 的形状。

**实现勘定(2026-09-30)**:

- **handler 首参是 self**:lxp 的 docall 把 parser userdata 作首参推栈,`StartElement = function(self, name, attrs)`——上方 API 草案的 `function(name, attrs)` 漏了 self,以实现为准(草案其余部分已用例钉住);
- **decode 保留空白文本节点**:元素间的空白(如 `<r> <a/> </r>`)是 kids 里的普通字符串——标准 DOM 行为,美化 XML 的往返靠它;声明/注释/DOCTYPE 不进 DOM;
- **相邻文本片段在元素边界合并**:实体展开(`a&amp;b`)与 CDATA 边界会把文本分片送达,decode 在 StartElement/EndElement 处合并成单字符串——往返后 `&` 不会被双重转义;
- **错误列按 UTF-8 字符计**(与 yaml 同口径):`<r>张三<x></r>` 的 mismatched tag 报 column 11(字符),不是字节 15;
- **encode 的容错面**:`attrs`/`kids` 为 nil 按空处理;标量 kid(数字/布尔)走 tostring 当文本;属性值强制 tostring;属性序 = pairs 序(不承诺插入序,与 toml encode 同口径);
- **encode 环检测**:元素是自身祖先 → `nil, "xml: cyclic table reference"`(error level 0,消息不带 file:line 前缀);共享兄弟引用不是环,各自完整序列化;
- **expat 的 parse(s) 不终结流**:final 标志是 `s == NULL`——decode 必须 `p:parse(s)` 后再 `p:parse()` 收尾,未闭合标签的错误只在收尾时浮现;收尾失败时 close 会再抛一次,decode 用 pcall 兜住(坏输入不 raise)。

### YAML → libyaml(经 lyaml)

| 候选 | 结论 |
| --- | --- |
| **libyaml + lyaml** | **入选**:libyaml 是 YAML 规范的参考 C 实现(MIT,PyYAML/libyaml 生态共享);lyaml(gvvaughan,MIT,Lua 5.1–5.4,6.2.8)提供 C 侧快速 decode/emit;5.5 适配同上,vendored 小补丁预期(**实现勘定 2026-09-30**:钉 v6.2.9(最新 release;lukefile 自称支持到 5.5),5.5 下零补丁编译——lyaml.h 的 5.2–5.4 垫片不触发,但 luna 随附 Lua 的 luaconf.h 仍恢复 `lua_objlen`/`lua_strlen` 兼容宏,四个 C 翻译单元原样过;唯 luke 构建系统的 `VERSION` 宏由 CMake 定义) |
| 纯 Lua(tinyyaml / lua-yaml) | 淘汰:均为子集实现——锚点/别名、块标量缩进、多文档指示器各自缺角;YAML 的歧义输入多,静默错解比报错危险 |
| js-yaml 移植思路 | 淘汰:规范复杂度不以实现语言转移,理由同上 |

**API 草案**:

```lua
local yaml = require "yaml"

yaml.decode("name: luna\nreleases:\n  - 1\n  - 2\n")
--   → { name = "luna", releases = { 1, 2 } }
yaml.encode({ ok = true })           -- → "---\nok: true\n...\n"

yaml.decodeAll(doc)                  -- 多文档流:--- 分隔,返回数组
yaml.decode(s, { nullval = yaml.null })  -- null 保留哨兵(默认 → nil)
```

- **标量类型**:映射→表、序列→数组表、字符串/整数/浮点/布尔→Lua 同名类型;**YAML null → nil**(与 json/dkjson 默认一致);"键在但值为 null"的区分需求用 `opts.nullval = yaml.null`(哨兵表)开启——js-yaml 的 null 语义,但默认关闭,与 json 对齐(**实现勘定**:子替换只动**值**,null 键保持哨兵——静默删条目比留哨兵更糟;数组里的 null 同样落 nil,序列尾部收缩;null 文档在 `decodeAll` 里是 nil 槽);
- **锚点/别名**:decode 把别名解成**共享表引用**;encode 对共享引用报循环错误(同 dkjson 的 self-referential 口径);`opts.anchors = true` 的锚点发射列入后续批次,不进 v1(**实现勘定**:encode 的环检测走祖先集,共享的**兄弟**引用不是环,各自完整序列化;自引用锚点(`&a` 下 `*a`)decode 出的就是共享自引用表,null 替换的遍历以访问集防环);
- **错误**:libyaml parser 错误自带 `problem` + `problem_mark`(行列),包装层规整成共同口径 4(**实现勘定**:lyaml 的 Lua 层丢弃了 C 消息里 libyaml 自己的行列子句,坐标改用**最后一个成功解析事件**的起始 mark(1 基,列按 UTF-8 字符计)——常不在出错行,实测如此并已用例钉住,guide/modules.md 记账;未定义别名走 load_alias 的 `invalid reference: <name>`,mark 恰在别名处)。

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
| net | ✅ `loop.net.connect/listen/connectTls/listenTls`(异步)+ **多地址回退(2026-10-01 勘定,见下节)** | — | — | C(luna_loop.c) | 2026-10-01 ✅ |
| timers | ✅ `loop.setTimeout/setInterval/setImmediate/clear*`(opt-in)+ **脚本尾部自动排水(批次 8 已启用)** | 全局化未决(REPL 集成维持推迟) | 见下节 | C 入口 + Lua 收尾 | 8 ✅ |
| child_process | ✅ `loop.process.run`(聚合)/`process.spawn`(流式)+ `exec`/`execSync`(批次 8) | — | — | C(loop.c 内) | 8 ✅ |
| os | ✅ `loop.os`(hostname/type/arch/release/EOL/userInfo/availableParallelism/home/tmpdir/uptime/loadavg/mem/cpus/networkInterfaces) | — | — | C(loop.c 内) | 8 ✅ |
| path | ✅ 批次 6 已落地(`path`,纯 Lua) | — | — | Lua | 6 |
| util | ✅ 批次 6 已落地(`util`,纯 Lua;`luna.introspect` 仍是 REPL 内省,两者分工见下) | — | — | Lua | 6 |
| events | ✅ 批次 6 已落地(`events`,纯 Lua) | — | — | Lua | 6 |
| stream | ✅ 批次 7 已落地(`stream`,纯 Lua,基于 events) | — | — | Lua | 7 |

**"绑定 C 库优先"在这张表上的落法**:timers/child_process/os 的底座已经是 libuv(deps 已在)——B 面的唯一新增 C 代码是 loop.os 的四个 getter(~60 行,仍在既有 `os_funcs` 表里);path/util/events/stream **没有对应的成熟 C 库可绑**(Node 本尊全是纯 JS;libuv 无 path join/EventEmitter),它们落在策略层正合架构分层——"C 内核尽量薄,行为逻辑全是 Lua"。

### net:connect/connectTls 多地址回退(2026-10-01 勘定)

**实现勘定(2026-10-01)**:表中原记「Lua 策略层」的预估不成立——addrinfo 链与句柄生命周期都在 C 侧,回退落 `luna_loop.c` 直修:

- 两条拨号路径解析结果**留链**(挂 sock/tsock,首次成功/全败/关闭三处释放),connect 失败换下一条,全败才把**最后一条**错误交回调(Node `net.connect` 的 autoSelectFamily 语义;单地址路径行为与旧码一致);
- plain 侧:libuv 不许对失败句柄二次 connect——`uv_close` 后在 close 回调里重初始化句柄再拨;
- TLS 侧:拒绝经 poll 的 SO_ERROR **异步**浮出,poll 句柄必须随 fd 一起关掉、在 close 回调里重开新 fd 再拨;连接成功之后的错误(如握手失败)不重拨——回退只覆盖 connect 层,与 Node 口径一致;
- 回归用例 `test_dial_localhost_v4_only_listener_succeeds`:v4-only 监听 + `localhost` 拨号必须成功,plain/TLS 一条用例两腿同盖。`test_tls_client_dials_by_hostname` 的 `'::'` 双栈监听**保留不动**——它现在钉的是双栈监听语义,不再承担绕病职责。

### timers:事件循环现状核实与建议

**现状(源码核实)**:派发单引用的 README「事件循环推迟」已经演进——[architecture.md](/architecture) 的章节现在是「事件循环:第一批(显式选择,脚本 opt-in)」:`require "loop"` 落地已久,libuv 1.53.0 静态链接,已有 timer/fs(+watch)/net(+TLS)/udp/dns/signal/os/process 八个 C 面 + `loop.http` 纯 Lua 面;prepare 钩子承接 `^C` → `interrupted`(退出码 130)与 attach 轮询。**推迟的对象已经不是"要不要循环",而是"要不要全局化/进 REPL"**——README 概览表原有"事件循环推迟"的表述已随 2026-10-04 定位批更新为现状口径(定时器/TCP/Unix/TLS/异步 fs/信号/子进程 + 脚本尾部自动排水)。

**建议:启用「脚本尾部自动排水」,继续推迟「全局化与 REPL 集成」。**

1. **启用:脚本尾部自动排水**。Node 契约是"注册了 timer,进程就活着"——`luna script.lua` 现在是"主 chunk 跑完就走",`loop.setTimeout` 没配 `loop.run()` 的定时器被静默丢弃(footgun)。建议:`run_script` 主 chunk 正常结束后,若 loop 已被 require 且仍有活句柄,自动 `loop.run()` 排空——这正是 Node 的"事件循环跑到空退出";`unref` 语义天然支持"在但不留人",既有契约一个不改。实现落点 `luna_main.c` 的脚本路径尾部(探测 package.loaded.loop 的句柄计数,或 loop.c 出一个 `luna_loop_maybe_drain()`),`-e` 模式同口径,REPL 不启用。风险面:只影响"用了 loop 但没调 run()"的脚本——现状是句柄被丢,新行为是句柄被执行,方向只会更对;全部既有 loop 测试必须原样全绿(它们自己调 run,自动排水对它们是无操作)。
2. **继续推迟:全局 `setTimeout`**。全局化逼着 REPL 主循环进循环世界:行编辑的阻塞读、`^C` 的 130 契约、attach 的轮询点全在同步侧,为省一个 `require` 动这三张契约,收益配不上代价;「REPL 每次求值后 drain(nowait)」的形态已记入 todo 下轮方向,条件成熟(行编辑可超时读或唤醒线程可定时)再启。

**实现勘定(2026-10-01,批次 8 已落地)**:

- **形态**:loop.c 出 `loop.maybeDrain()`(活句柄才排水,否则空操作返回 false),`lua/luna.lua` 的 `run_script` 与 `run_eval` 在主块**正常结束**后调用;主块出错不排水(对齐 Node 未捕获异常即退);REPL 不调用。`-i script.lua` 先排水再进控制台;
- **排水中的 `^C` 照 130**:`maybeDrain` 与 `run()` 同款以 `interrupted` 抛错,`exit_code_for` 拿到 130——一张契约贯穿脚本体与排水段;
- **顺手根因修复一个真 bug**:keep-alive 的 prepare 钩子此前是 ref'd 句柄——只要还有任何用户句柄(unref 与否)它就撑着循环,`run()` 在"只剩 unref 句柄"时**永不返回**(实测 `-e` unref interval + run() 挂死),与"unref 不再绑住脚本寿命"的文档承诺相悖。修复:prepare 改 `uv_unref`——存活账本全归用户句柄自己,钩子照常在循环每一拍照跑(^C 翻译与 attach 轮询不受影响);副作用是把"unref 的一次性定时器在 run() 里照常触发"翻转为 Node 语义(循环不转,永不触发);
- 全局化与 REPL 集成维持推迟,启用条件不变。
- **REPL 集成落地(2026-10-10,唤醒线程定时面)**:上条的启用条件(「唤醒线程可定时」)已建成——`linedit.arm_timer(ms)` 给唤醒线程加一次性 deadline,到期注入的哨兵键在**空提示符**上把阻塞读打断成空行 tick(打字中的草稿只顺延,永不打断;判定跑在输入线程内、`replxx_get_state` 的文档口径场景,零竞态),REPL 侧以 `package.loaded.loop` 探测(不自动加载),tick 上 `loop.turn()` 跑一轮。全局 `setTimeout` 维持推迟:它要的是**不 require 就在**,那就得动 REPL 的加载语义,而定时面已经把"循环在会话里转起来"的收益交付了。

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

Node 语义按 **Node v24.21.0 机器实证**逐函数钉死(每个函数的边界行为对照进用例);~170 行纯 Lua,零依赖,**不用 C 的理由:Node 本尊纯 JS,libuv 无对应物**。

**实现勘定(2026-10-01,批次 6)**:
- **normalize/resolve 折叠连续斜杠**——本节初稿写的「normalize 保留双前导斜杠」与 v24 实测不符:`normalize("//a")` → `"/a"`、`resolve("//a","b")` → `"/a/b"`(旧版 Node 的双斜杠保留已不成立);normalize 与 resolve 在尾斜杠上分叉——normalize 保留(`"foo/"` → `"foo/"`),resolve 剥掉(`resolve("/a/b/")` → `"/a/b"`);
- **dirname 是文本操作**:斜杠原样保留(`"a//b"` → `"a/"`、`"//a//b"` → `"//a/"`);parse 的 dir 同样原样(`parse("//x/y").dir == "//x"`);
- **extname 的「全点前缀」规则**:`".."` 与 `".bashrc"` 无扩展名,`"a.."` → `"."`——最后一个点之前必须存在非点字符;
- **format 的拼接规则**:`dir` 非空则原样接 `dir .. "/" .. file`(`dir="/a/b/"` → `"/a/b//x"`,不吸收尾斜杠),仅 root 无 dir 时不加分隔符;`ext` 无前导点会补点(`{name="x", ext="lua"}` → `"x.lua"`,`{ext="lua"}` → `".lua"`);字段空串视为缺席(JS 真值口径);
- **relative 先 resolve 再段差**——同路径(含尾斜杠差异)返回 `""`;
- 参数类型错 raise(消息 `path.<fn>: ...`),cwd 锚定经 lfs(`luna` 随附恒在;脱离宿主时回退 `$PWD`/`"/"`,该两腿 lfs 在场不可达,覆盖账记结构性)。

### util(批次 6,纯 Lua)

- **`util.inspect(value, opts?)`**:值格式化——`depth`(默认 2,`math.huge` 全展开,负数顶层即塌缩)、循环引用 `[Circular *1]`(表登记)、长数组截断 `... N more items`(Node 的 breakLength 语义简化为 opts.maxArrayLength/opts.maxStringLength 的个数截断,折行不进 v1)、字符串带引号与转义、函数 `<function: shortsrc:line>`(经 debug.info;C 函数 tostring 兜底)、线程/udata 用 tostring 兜底、键 `["a b"]` 形式按 Lua 语法合法名规则(Lua 关键字加方括号);
- **`util.format(fmt, ...)`**:`%s`/`%d`/`%i`/`%f`/`%x`/`%X`/`%o`(走 util.inspect,Node 的 %o)/`%j`(JSON,后端 dkjson,同树可 require)/`%%`;无格式符时全参数空格连接(Node 同款);超参尾接;
- **与 `luna.introspect` 的分工**:introspect 是 REPL 的**内省**(函数签名、help、doc_for、suggest——查"这个函数怎么用");util.inspect 是**值格式化**(把任意值印成人话——查"这个数据长什么样")。不合并;REPL 的 `Out[n]` 表格回显已改用 util.inspect(2026-10-09 落地):表值走 inspect(确定性键序、与 stdlib 同款的深度/环/截断标记),标量与函数维持 introspect repr(带引号字符串、函数签名),`%eval` 同通道同渲染。
- **不做**:`promisify`/`callbackify`(Lua 无 promise,契约无锚)、`types`(isDeepStrictEqual 一族)。

**实现勘定(2026-10-01,批次 6,Node v24.21.0 机器实证)**:
- **`%d`/`%f` 不截断**(v24 实测 `format("%d", 3.7)` → `"3.7"`),只有 `%i` 向零截断(`-3.7` → `-3`);转换走 JS `Number()` 收窄——整合法数字串转数、其余一律 `"NaN"`(`"42abc"` → `"NaN"`,布尔按 1/0);`NaN`/`Infinity` 按原词拼写(非 Lua 的 `nan`/`inf`);
- **`%x`/`%X` 是本模块扩展**:Node v24 的 util.format **没有** `%x`/`%X`(实测留原样不消费参数)——todo 契约点名要求,故按十六进制实现(向零截整、负数 `-` 前缀),此为有意分叉;
- **`%c` 消费参数但不产出**(无终端样式层);未知转换符 `%` 原样保留且不消费(`format("%y", "x")` → `"%y x"`);
- **`%%` 只在发生格式化时折叠**:`format("100%%")` 单串无参走快路径原样返回 `"100%%"`,`format("50%% and %s", "x")` → `"50% and x"`;
- **`%j` 的 dkjson 分叉**:dkjson 比浏览器 JSON.stringify 严——函数值直接报错而非省略,失败回退 util.inspect(不抛);
- inspect 的 **Lua 语法适配**:表一律 `{}` 形态(JS 的 `[]`/`{}` 在 Lua 无对应)、键值 `k = v` 而非 `k: v`;**键排序保确定性**(序列段在前,其余数字升序、字符串字节序、他类按 tostring;pairs 序不定,无法对齐 Node 插入序——适配而非分叉);深度塌缩标记 `[Object]`/`[Array]`(按 `#t` 判);截断省略号用 ASCII `...`;共享(非环)引用各自完整渲染,只有祖先链上的重访标 `[Circular *N]`。

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

**实现勘定(2026-10-01,批次 6,Node v24.21.0 机器实证)**:
- `events.new()` 构造,`require("events").EventEmitter` 是模块表别名(`.defaultMaxListeners = 10` 可改,影响此后所有添加判定);
- **newListener 在添加前发出**(其回调里 `listenerCount` 不含新者),**removeListener 在移除后发出**;`removeAllListeners(ev)` 按 Node 源码同序 **LIFO 逐个**发 removeListener,无参调用全清不发;
- **超限警告**写 io.stderr,文本照抄 Node(含计数单复数与 `emitter:setMaxListeners()` 的 Lua 冒号拼写),每 (emitter, 事件名) 一次,监听移除或 `setMaxListeners` 后**重臂**可再发;`0`/负上限 = 不限;
- **emit('error') 无监听 → raise**:表负载原样 `error(payload)`(JS Error 对应物),标量包 `Unhandled error. (...)`(字符串带 `'...'` 引号,数字走 inspect),无负载报 `Unhandled 'error' event`;错误消息 `error(..., 0)` 不带 chunk 位置前缀(与 xml 环错误同口径);
- emit 按监听快照分发——回调内自移除本轮照常调用、once 内重挂的新监听下轮生效;`listeners()` 返回副本且 once 项以**原函数**入表(off 匹配的是原函数);`removeListener` 只移除首个匹配;
- **eventNames 不做**(依赖插入序,Lua pairs 序不定,返回序无法对齐 Node——登记而非实现);`emitter once` 与 `setMaxListeners` 均返回 self 可链式。

### stream(批次 7,纯 Lua,基于 events)

**v1 面收窄**(Node stream 全家桶很大,luna 只做被适配器真正消费的面):Readable(`_read` 源 → `push`,`'data'`/`'end'`/`'readable'`、pause/resume)、Writable(`_write` 汇 → `'drain'`/`'finish'`,`write`/`end`)、Duplex、Transform(`_transform`)、`readable:pipe(writable)`(含背压——write 返回 false 即停 push,等 drain 续推,`unpipe`)、`opts.highWaterMark`(缺省 16KiB,只做记账阈值不做字节精确)。事件与错误传播按 Node 语义(error → destroy → 'close'、pipe 上的错误联动)。

**适配器是 stream 的存在理由**(批次 7 内含 2–3 个,其余按需追加):`sock`(loop.net)→ Duplex(读回调转 push、write 转发)、`fs.readFileSync` 的内存块 → Readable、`loop.http` 的 onData → Readable。**明确不做**:webstreams(`ReadableStream` 家族)、异步迭代器(`for ... in s:lines()` 的 Lua 迭代器形态倒是顺手,记为可选)、`setEncoding`/`cork` 一族小面。

**实现勘定(2026-10-01,批次 7,Node v24 语义对照)**:
- 构造是工厂函数:`stream.readable/writable/duplex/transform(opts)`(`stream.Readable` 等大写形状是同一函数的别名);`end` 因关键字冲突拼作 `s:end_(chunk, cb)`,**sock 同款**的 `s["end"]` 形状也可用;
- **开流时机**:首个 `data` 监听(经覆写 `on`/`once`)真正落位后才转流动——events 的 `newListener` 在监听器落位**前**发出,在钩子里开流会把头几块数据丢给空气,这是与 Node(resumeScheduled 延一拍)实现路径不同、语义相同的处理;
- **拉式源的循环**:`_read` 在内部缓冲空且流动时被调,同步无产出即停、等下一次 `push` 触发;`push` 在 `_read` 里只入账不投递(防递归无底),投递由外层循环收口;`push(nil)` 即 EOF,EOF 后再 `push(chunk)` 发 `error` 事件;
- **背压双向**:消费侧 `write` 返回 false 即源 `pause`、目标 `drain` 续推;生产侧 `push` 返回 false 即源头收手,缓冲排空时 `_read` 被回调灌——`pushReadable` + 自持队列就是 loop.http onData 的适配形态("视余量"= 这个返回值加这个回灌点);
- **autoDestroy 两脸判据**:纯流单脸(`end`/`finish`)到位即 `destroy`→`close`;duplex/transform 要 `end` 与 `finish` **都**到齐才 `close`(Node 同款)——sock EOF 后还需 `end_()` 收写脸;transform 的 `finish` 先于读侧 `end`(flush 完成即 finish,缓冲排空才 end);
- **与 Node 裸 pipe 的有意分叉**:源 `error` 会 `unpipe` 后 `destroy(同错)` 到目标(Node 的裸 pipe 不动目标,那是 `pipeline()` 的契约——luna 把这个契约并进 `pipe`,一个调用管到底);目标 `error` 只 `unpipe`,源不炸;`opts.end == false` 关掉源 `end` → 目标 `end_()` 的联动;
- **记账简化**:字符串按字节、其余值按 1(objectMode 恒开的合并面),`highWaterMark` 缺省 16384,只做阈值不做字节精确;`destroy` 把未决 write 回调(**在途一笔 + 排队若干**)以 `"stream destroyed"` 结账,`destroy(err)` 无 error 监听时按 events 语义 raise;
- 适配器三件:`duplexFromSock(sock)`(常驻读回调转 push、write 转发、`_destroy` 关 sock、读错误走 error→destroy 联动)、`readableFromChunks(chunks)`(字符串或块表,一次一块按需推)、`pushReadable(opts)`(纯推式,`push` 公开);modules 组 8 用例 + loop 组 1 个真 sock 回显集成(写转发/EOF→end/autoDestroy→close)钉住。

### os 补齐(批次 8,C 在 loop.c 内)

`loop.os` 增补,与 Node `os` 的名字对齐(不建顶层 `os` 模块——Lua 全局 `os` 是官方标准库,遮蔽它违背"官方语义不动"的纪律):

| 增补 | 底座 | Node 对照 |
| --- | --- | --- |
| `loop.os.arch()` | `uv_os_uname()->machine`,映射 `x86_64→"x64"`、`aarch64→"arm64"`(Node 的名字,不是 uname 原文) | `os.arch()` |
| `loop.os.release()` | `uv_os_uname()->release` | `os.release()` |
| `loop.os.EOL` | 常量 `"\n"`(POSIX 平台面) | `os.EOL` |
| `loop.os.userInfo()` | `uv_os_get_passwd`:`{username, uid, gid, shell, homedir}` | `os.userInfo()` |
| `loop.os.availableParallelism()` | `uv_available_parallelism` | Node ≥18 同名 |

**实现勘定(2026-10-01,批次 8)**:`arch` 的映射收 `x86_64`/`amd64`→`"x64"`、`aarch64`/`arm64`→`"arm64"`,其余 uname machine 原样;`EOL` 是字段常量 `"\n"`(在 `luaL_newlib` 后 setfield,不是函数);`userInfo` 的键 `{username, uid, gid, shell, homedir}`——shell 无条目时**键缺席**(nil),uid/gid 是 number;五件全部同步直返、失败 `luaL_error`,与既有 os 面同款。REPL 里的 `os` 全局不受影响(官方标准库的地盘,不遮蔽)。

### child_process 糖(批次 8)

- `process.exec(cmd, opts?, cb)`:`run("sh", {"-c", cmd}, opts, cb)` 的糖(Node `exec` 的 shell 语义;stdout/stderr 聚合照旧);`execFile` 即 `run` 本尊,文档点名即可;
- `process.execSync(cmd) -> stdout`:`popen` 同步面(`LUA_USE_POSIX` 已开)——读尽 stdout,`pclose` 的 wait 状态拿退出码,非零(或死于信号)raise——Node `execSync` 的形状(io.popen 的 `close()` 三元组同源语义);
- **不做** `spawnSync` 的三流同步面(popen 单流 + `/dev/null` 重定向够用的场景走 execSync;真三流是 `process.spawn` 的场)。

**实现勘定(2026-10-01,批次 8)**:两件落在 **C 面 `process_funcs`**(loop.c 内),而非上表初记的"Lua 层"——糖要随 `loop` 模块同脸出现:CLI、嵌入式测试桩(`luaL_requiref` 直开 loop 的 harness)与 `lua/luna.lua` 三方都拿到同一张 `loop.process` 表,单独放 Lua 层会漏掉前两者;`exec` 在栈上拼出 `sh, {"-c", cmd}, opts, cb` 后**逐字转调 `l_process_run`**(聚合/交付/cwd 语义零分叉),`execSync` 走 `popen`/`pclose` + `WIFEXITED`/`WEXITSTATUS`,20000 字节级输出跨 4 KiB 读循环、`exit 3` 与 `kill -9 $$` 两条 raise 腿均有用例。
