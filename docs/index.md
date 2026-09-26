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
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><rect x="3" y="4" width="18" height="16" rx="2"/><path d="m7 9 3 3-3 3"/><path d="M13 15h4"/></svg>'
    title: REPL 即入口
    details: 多行续行、Tab 补全、历史召回、实时语法高亮、IPython 风格的 %time / %timeit / %whos 魔法命令,开箱即用。
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M12 3 3 7.5v9L12 21l9-4.5v-9L12 3z"/><path d="M3 7.5 12 12l9-4.5"/><path d="M12 12v9"/></svg>'
    title: Node 式模块
    details: 相对 require、沿 luna_modules/ 逐级上溯的裸名解析、package.json 式清单,与 package.loaded 缓存语义。
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M9 3v5"/><path d="M15 3v5"/><path d="M6 8h12v3a6 6 0 0 1-12 0V8z"/><path d="M12 17v4"/></svg>'
    title: 目录插件
    details: ./plugins 就近发现,plugin.json 清单,魔法命令 / 补全源 / 高亮规则 / 模块注入四个扩展点,失败隔离不致命。
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M12 3 6 12h4l-3 6h10l-3-6h4L12 3z"/><path d="M12 18v3"/></svg>'
    title: 现代标准库
    details: fs / net / http / json / zlib / crypto 均绑定成熟 C 库,随二进制一体分发,无需额外安装。
---
