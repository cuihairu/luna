---
layout: home

hero:
  name: luna
  image: /logo.svg
  text: Lua runtime for scripting, tooling, and lightweight services
  tagline: A batteries-included Lua runtime — REPL、标准库、Node 式模块、插件、包管理,集成在一个二进制里。
  actions:
    - theme: brand
      text: 快速上手
      link: /zh/guide/getting-started
    - theme: alt
      text: 架构设计
      link: /zh/architecture
    - theme: alt
      text: GitHub
      link: https://github.com/cuihairu/luna

features:
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><rect x="3" y="4" width="18" height="16" rx="2"/><path d="m7 9 3 3-3 3"/><path d="M13 15h4"/></svg>'
    title: REPL 即入口
    details: 多行续行、Tab 补全、历史召回、实时语法高亮,外加 %time、%timeit、%whos 一类 IPython 风格的魔法命令。
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M12 3 3 7.5v9L12 21l9-4.5v-9L12 3z"/><path d="M3 7.5 12 12l9-4.5"/><path d="M12 12v9"/></svg>'
    title: Node 式模块
    details: 相对 require、沿 luna_modules/ 逐级上溯的裸名解析、package.json 式清单,配 package.loaded 缓存语义。
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M12 3 6 12h4l-3 6h10l-3-6h4L12 3z"/><path d="M12 18v3"/></svg>'
    title: 现代标准库
    details: fs / net / http / csv / ini / toml / yaml / xml / zlib / crypto 绑定成熟 C 库或 LPeg,path / util / events / stream / console / process / querystring / url / assert 按 Node 语义,json(dkjson)——全部纯 Lua,随二进制一体分发。
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M9 3v5"/><path d="M15 3v5"/><path d="M6 8h12v3a6 6 0 0 1-12 0V8z"/><path d="M12 17v4"/></svg>'
    title: 目录插件
    details: ./plugins 就近发现,plugin.json 清单,魔法命令、补全源、高亮规则、模块注入四个扩展点,失败隔离不致命。
---


## 架构

<p align="center">
  <img src="/images/architecture-zh.svg" alt="luna 三层架构:用户空间 / Luna 运行时 / 原生运行时" width="960"/>
</p>


## 在终端里

一次真实会话的原样截取(pty 抓取,In/Out 寄存器、语法高亮与魔法命令输出均未加工):

<p align="center">
  <img src="/terminal.svg" alt="luna 终端会话:REPL 求值、多行函数定义、%timeit 与 json 编码" width="560"/>
</p>

<script setup lang="ts">
// 与英文首页对称：双向记录手动语言选择、双向跟随 localStorage，否则英文用户点中文会被弹回
// 优先级：localStorage 手动选择 > navigator.language > 保持当前 locale
import { onBeforeMount } from 'vue'
const KEY = 'luna-lang-auto-picked'
const BASE = '/luna/'
const ROOT_PATH = BASE
const ZH_PATH = BASE + 'zh/'
onBeforeMount(() => {{
  if (typeof window === 'undefined') return
  const p = window.location.pathname
  if (p !== ZH_PATH && p !== ZH_PATH.slice(0, -1)) return
  const picked = localStorage.getItem(KEY)
  if (picked) {{ if (picked !== p) window.location.replace(picked); return }}
  const langs = (navigator.languages?.length ? navigator.languages : [navigator.language]).map(l => l.toLowerCase())
  if (!langs.some(l => l.startsWith('zh'))) window.location.replace(ROOT_PATH)
}})
if (typeof window !== 'undefined') {{
  window.addEventListener('click', (e) => {{
    const a = (e.target as HTMLElement)?.closest?.('a') as HTMLAnchorElement | null
    const href = a?.getAttribute('href') || ''
    if (href.startsWith(ZH_PATH) || href.startsWith('zh/')) localStorage.setItem(KEY, ZH_PATH)
    else if (href === ROOT_PATH || href === ROOT_PATH.slice(0, -1)) localStorage.setItem(KEY, ROOT_PATH)
  }}, true)
}}
</script>
