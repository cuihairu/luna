import { defineConfig } from 'vitepress'

export default defineConfig({
  lang: 'zh-CN',
  // project Pages serves under /luna/ — without this, every asset and
  // link 404s at https://cuihairu.github.io/luna/
  base: '/luna/',
  title: 'luna',
  description:
    'A batteries-included Lua runtime for scripting, tooling, and lightweight services:REPL、标准库、Node 式模块、插件与包管理,集成在一个二进制里',
  themeConfig: {
    logo: '/logo.svg',
    siteTitle: '🌙 luna',
    nav: [
      { text: '指南', link: '/guide/introduction' },
      { text: '架构', link: '/architecture' },
      { text: 'GitHub', link: 'https://github.com/cuihairu/luna' }
    ],
    sidebar: [
      {
        text: '指南',
        items: [
          { text: '介绍', link: '/guide/introduction' },
          { text: '快速上手', link: '/guide/getting-started' },
          { text: 'CLI 与 REPL', link: '/guide/cli-repl' },
          { text: '模块系统', link: '/guide/modules' },
          { text: '插件开发', link: '/guide/plugins' },
          { text: '包管理', link: '/guide/rocks' },
          { text: '事件循环', link: '/guide/loop' },
          { text: '全局对象 kernel', link: '/guide/kernel' }
        ]
      },
      {
        text: '标准库',
        items: [
          { text: '总览(四类分法)', link: '/stdlib/' },
          { text: 'Core', collapsed: false, items: [
            { text: 'json', link: '/stdlib/json' },
            { text: 'fs', link: '/stdlib/fs' },
            { text: 'path', link: '/stdlib/path' },
            { text: 'util', link: '/stdlib/util' },
            { text: 'events', link: '/stdlib/events' },
            { text: 'stream', link: '/stdlib/stream' },
            { text: 'net', link: '/stdlib/net' },
            { text: 'http', link: '/stdlib/http' },
            { text: 'crypto', link: '/stdlib/crypto' }
          ] },
          { text: 'Data-Format', collapsed: false, items: [
            { text: 'csv', link: '/stdlib/csv' },
            { text: 'ini', link: '/stdlib/ini' },
            { text: 'toml', link: '/stdlib/toml' },
            { text: 'yaml', link: '/stdlib/yaml' },
            { text: 'xml', link: '/stdlib/xml' },
            { text: 'zlib', link: '/stdlib/zlib' }
          ] },
          { text: 'Dev tooling', collapsed: false, items: [
            { text: 'logging', link: '/stdlib/logging' }
          ] },
          { text: 'Ecosystem', collapsed: false, items: [
            { text: '包管理(LuaRocks)', link: '/guide/rocks' },
            { text: '插件', link: '/guide/plugins' },
            { text: '模块系统', link: '/guide/modules' }
          ] }
        ]
      },
      {
        text: '其他',
        items: [
          { text: '配置与环境变量', link: '/other/config' },
          { text: '错误模型与退出码', link: '/other/errors' },
          { text: '构建与自测', link: '/other/build' },
          { text: 'FAQ', link: '/other/faq' }
        ]
      },
      {
        text: '设计',
        items: [
          { text: '架构设计', link: '/architecture' },
          { text: '事件循环后端', link: '/loop-backend-design' },
          { text: 'Node 方向选型', link: '/node-parity' }
        ]
      }
    ],
    socialLinks: [
      { icon: 'github', link: 'https://github.com/cuihairu/luna' }
    ],
    outline: [2, 3],
    search: { provider: 'local' },
    docFooter: {
      prev: '上一篇',
      next: '下一篇'
    }
  }
})
