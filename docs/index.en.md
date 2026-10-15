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
      link: /architecture.en
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

[English](index.en.md) | [中文](index.md)

## In the terminal

A verbatim excerpt of a real session (pty capture; the In/Out registers, syntax highlighting and magic-command output are unmodified):

<p align="center">
  <img src="/terminal.svg" alt="luna terminal session: REPL evaluation, multi-line function definition, %timeit and json encoding" width="560"/>
</p>
