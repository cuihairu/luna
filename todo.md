# todo

2026-09-25 通读 README / docs / 源码后的缺口盘点。README 宣称的能力大多已落地,本清单只记
**文档承诺了但实现缺位**、**明显值得补**与**顺手打磨**三类,按优先级排序。
约定:每完成一项必须带测试,`cmake --build` + `ctest` 全绿后才 commit/push;不打 tag、不发版。

## P1 会话与魔法命令(文档承诺 vs 实现缺口)

- [ ] **`In[n]` 寄存器缺失**:README/docs 承诺 `In[n]`/`Out[n]` 会话,实际只有 `_G.Out`;
      输入历史仅存于 session 内部(`session.inputs`)。补 `_G.In[n]`(与 Out 同点维护),
      会话求值与魔法命令行都登记;`%reset` 把 Out/In 与 `_`/`__` 一并复位
      (当前 `%reset` 漏清 `_`/`__`,残留上一次结果)。
- [ ] **`%time`/`%timeit` 只吃表达式**:硬包装 `"return " .. arg`,语句形式
      (`%time for i=1,1e6 do end`)报语法错——REPL 本体有 wrapped 探针,magic 没有。
      复用同一探针:能作为表达式则照旧回显 Out,否则按语句执行、只报时间。
- [ ] **`%plugins` 不报遮蔽**:文档说同名后者记入 `plugins.overridden`,但 `%plugins`
      只列 loaded/failed。补 overridden 清单输出。

## P2 错误提示与补全

- [ ] **"did you mean" 建议**:Lua 5.5 运行时错误自带 `(global 'x')` 线索,但 REPL 原样
      打印。错误输出前解析线索名,`rawget(_G, name)` 为空时给出编辑距离最近的 1–2 个
      全局名候选(`luna.introspect.suggest`),打错字当场可见。
- [ ] **Tab 补全不认 require 目标**:`complete.lua` 注释自认 path completion 是"留给插件"
      的例子,最常用的 `require "…` 反而没人补。内置一个 source:`require "pre` /
      `require("pre` 列出逐级上溯各层 `luna_modules/` 的包名(lfs 缺失时静默降级)与
      `package.loaded` 已加载名;点号续配(`require "json.`)走已加载表的字段。
- [ ] **luna_modules 包内子模块**:`require("dep.sub")`/`require("dep/sub")` 的 Node 语义
      是解析到 `<上溯>/luna_modules/dep/{sub.lua, sub/init.lua}`;当前只试字面
      `luna_modules/dep.sub`,项目内依赖的子模块解析不到(只有全局 staged 树碰巧命中)。
      在 `resolve_bare` 加最长前缀包匹配:先照旧试整名,再从最长点段前缀回退找包目录,
      余段按包内文件解析(不走包 `main`,与 Node 一致)。

## P3 标准库与打磨

- [ ] **fs 便捷层补齐**:`appendFileSync` / `readdirSync`(排除 `.`/`..`)/
      `mkdirSync(recursive)`(mkdir -p 语义),Node 命名,保持薄层 + 测试 + 文档。
- [ ] **`%hist` 打磨**:支持范围参数(`%hist 2-5`);遍历改 `ipairs` 保证顺序稳定。
- [ ] **文档站同步**:cli-repl(In 寄存器、did-you-mean、%plugins 遮蔽行)、
      modules(子模块解析规则、fs 新便捷层)。
- [ ] **覆盖率**:每个新特性必须带测试(新增分支全覆盖);每组改动后 ctest 全组绿。

## 明确不做(本轮)

- 未闭合引号跨行续行的语义修补(`s = "abc` 回车后无解)——`^C` 丢弃整块已可用,改语义
  动 `kernel.check` 契约,收益低;
- 包管理(luna.rocks)与 loop 面扩展——已有专文与测试,不在本轮范围;
- 打 tag / 发版(硬约束禁止)。
