---
layout: home

hero:
  name: luna
  image: /logo.svg
  text: Lua runtime for scripting, tooling, and lightweight services
  tagline: A batteries-included Lua runtime — REPL, stdlib, Node-style modules, plugins, package management, all in one binary.
  actions:
    - theme: brand
      text: Quick start
      link: /guide/getting-started
    - theme: alt
      text: Architecture
      link: /architecture
    - theme: alt
      text: GitHub
      link: https://github.com/cuihairu/luna

features:
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><rect x="3" y="4" width="18" height="16" rx="2"/><path d="m7 9 3 3-3 3"/><path d="M13 15h4"/></svg>'
    title: The REPL is the entry point
    details: Multi-line continuation, Tab completion, history recall, live syntax highlighting, plus IPython-style magic commands such as %time, %timeit and %whos.
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M12 3 3 7.5v9L12 21l9-4.5v-9L12 3z"/><path d="M3 7.5 12 12l9-4.5"/><path d="M12 12v9"/></svg>'
    title: Node-style modules
    details: Relative require, bare-name resolution climbing up the luna_modules/ chain, package.json-style manifests, with package.loaded caching semantics.
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M12 3 6 12h4l-3 6h10l-3-6h4L12 3z"/><path d="M12 18v3"/></svg>'
    title: A modern stdlib
    details: fs / net / http / csv / ini / toml / yaml / xml / zlib / crypto bound to mature C libraries or LPeg; path / util / events / stream / console / process / querystring / url / assert following Node semantics; json (dkjson) — all pure Lua, shipped in the one binary.
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M9 3v5"/><path d="M15 3v5"/><path d="M6 8h12v3a6 6 0 0 1-12 0V8z"/><path d="M12 17v4"/></svg>'
    title: Directory plugins
    details: ./plugins discovered in place, plugin.json manifests, four extension points — magic commands, completion sources, highlight rules, module injection — with failure isolation, never fatal.
---


## Architecture

<p align="center">
  <img src="/images/architecture-en.svg" alt="luna three-layer architecture: User Space / Luna Runtime / Native Runtime" width="960"/>
</p>


## In the terminal

A verbatim excerpt of a real session (pty capture; the In/Out registers, syntax highlighting and magic-command output are unmodified):

<p align="center">
  <img src="/terminal.svg" alt="luna terminal session: REPL evaluation, multi-line function definition, %timeit and json encoding" width="560"/>
</p>

<script setup lang="ts">
// 首次访问按浏览器语言选择文档语言（vitepress 无此能力，GitHub Pages 不支持 _redirects）
// 优先级：localStorage 手动选择 > navigator.language > 保持当前 locale
import { onBeforeMount } from 'vue'
const KEY = 'luna-lang-auto-picked'
const BASE = '/luna/'
const ROOT_PATH = BASE
const ZH_PATH = BASE + 'zh/'
onBeforeMount(() => {{
  if (typeof window === 'undefined') return
  const p = window.location.pathname
  if (p !== ROOT_PATH && p !== ROOT_PATH.slice(0, -1)) return
  const picked = localStorage.getItem(KEY)
  if (picked) {{ if (picked !== p) window.location.replace(picked); return }}
  const langs = (navigator.languages?.length ? navigator.languages : [navigator.language]).map(l => l.toLowerCase())
  if (langs.some(l => l.startsWith('zh'))) window.location.replace(ZH_PATH)
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
