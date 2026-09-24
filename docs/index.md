---
layout: home

hero:
  name: luna
  text: IPython 式的 Lua 交互环境
  tagline: 以 Node.js 运行时的方式生长 —— 模块解析、目录插件、现代标准库,而 REPL 始终是入口。
  actions:
    - theme: brand
      text: 快速上手
      link: /guide/getting-started
    - theme: alt
      text: 架构设计
      link: /architecture

features:
  - icon: 🖥️
    title: REPL 即入口
    details: 多行续行、Tab 补全、历史召回、实时语法高亮、IPython 风格的 %time / %timeit / %whos 魔法命令,开箱即用。
  - icon: 📦
    title: Node 式模块
    details: 相对 require、沿 luna_modules/ 逐级上溯的裸名解析、package.json 式清单,与 package.loaded 缓存语义。
  - icon: 🔌
    title: 目录插件
    details: ./plugins 就近发现,plugin.json 清单,魔法命令 / 补全源 / 高亮规则 / 模块注入四个扩展点,失败隔离不致命。
  - icon: 🌲
    title: 现代标准库
    details: fs / net / http / json / zlib / crypto 均绑定成熟 C 库,随二进制一体分发,无需额外安装。
---
