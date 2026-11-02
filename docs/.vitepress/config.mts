import { defineConfig } from 'vitepress'

export default defineConfig({
  // project Pages serves under /luna/ — without this, every asset and
  // link 404s at https://cuihairu.github.io/luna/
  base: '/luna/',
  title: 'luna',
  srcExclude: ['review/**'],
  themeConfig: {
    logo: '/logo.svg',
    siteTitle: '🌙 luna',
    socialLinks: [
      { icon: 'github', link: 'https://github.com/cuihairu/luna' }
    ],
    outline: [2, 3],
    search: { provider: 'local' }
  },
  locales: {
    root: {
      label: 'English',
      lang: 'en-US',
      title: 'luna',
      description:
        'A batteries-included Lua runtime for scripting, tooling, and lightweight services: REPL, standard library, Node-style modules, plugins, and package management in one binary.',
      themeConfig: {
        nav: [
          { text: 'Guide', link: '/guide/introduction', activeMatch: '/guide/' },
          { text: 'Architecture', link: '/architecture', activeMatch: '/architecture' }
        ],
        sidebar: [
          {
            text: 'Guide',
            items: [
              { text: 'Introduction', link: '/guide/introduction' },
              { text: 'Getting started', link: '/guide/getting-started' },
              { text: 'CLI and REPL', link: '/guide/cli-repl' },
              { text: 'Module system', link: '/guide/modules' },
              { text: 'Plugin development', link: '/guide/plugins' },
              { text: 'Package management', link: '/guide/rocks' },
              { text: 'Event loop', link: '/guide/loop' },
              { text: 'Async tasks', link: '/guide/tasks' },
              { text: 'Examples and scaffolding', link: '/guide/examples' },
              { text: 'Sandbox runtime', link: '/guide/sandbox' },
              { text: 'The kernel global', link: '/guide/kernel' }
            ]
          },
          {
            text: 'Standard library',
            items: [
              { text: 'Overview (four-class split)', link: '/stdlib/' },
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
                { text: 'Package management (LuaRocks)', link: '/guide/rocks' },
                { text: 'Plugins', link: '/guide/plugins' },
                { text: 'Module system', link: '/guide/modules' }
              ] }
            ]
          },
          {
            text: 'Other',
            items: [
              { text: 'Configuration and environment', link: '/other/config' },
              { text: 'Error model and exit codes', link: '/other/errors' },
              { text: 'Building and self-testing', link: '/other/build' },
              { text: 'FAQ', link: '/other/faq' }
            ]
          },
          {
            text: 'Design',
            items: [
              { text: 'Architecture', link: '/architecture' },
              { text: 'Event-loop backend', link: '/loop-backend-design' },
              { text: 'Node alignment', link: '/node-parity' }
            ]
          }
        ],
        docFooter: { prev: 'Previous', next: 'Next' }
      }
    },
    zh: {
      label: '简体中文',
      lang: 'zh-CN',
      link: '/zh/',
      title: 'luna',
      description:
        '开箱即用的 Lua 通用运行时:REPL、标准库、Node 式模块、插件与包管理,集成在一个二进制里。',
      themeConfig: {
        nav: [
          { text: '指南', link: '/zh/guide/introduction', activeMatch: '/zh/guide/' },
          { text: '架构', link: '/zh/architecture', activeMatch: '/zh/architecture' }
        ],
        sidebar: [
          {
            text: '指南',
            items: [
              { text: '介绍', link: '/zh/guide/introduction' },
              { text: '快速上手', link: '/zh/guide/getting-started' },
              { text: 'CLI 与 REPL', link: '/zh/guide/cli-repl' },
              { text: '模块系统', link: '/zh/guide/modules' },
              { text: '插件开发', link: '/zh/guide/plugins' },
              { text: '包管理', link: '/zh/guide/rocks' },
              { text: '事件循环', link: '/zh/guide/loop' },
              { text: '异步任务', link: '/zh/guide/tasks' },
              { text: '示例与脚手架', link: '/zh/guide/examples' },
              { text: '沙箱运行时', link: '/zh/guide/sandbox' },
              { text: '全局对象 kernel', link: '/zh/guide/kernel' }
            ]
          },
          {
            text: '标准库',
            items: [
              { text: '总览(四类分法)', link: '/zh/stdlib/' },
              { text: 'Core', collapsed: false, items: [
                { text: 'json', link: '/zh/stdlib/json' },
                { text: 'fs', link: '/zh/stdlib/fs' },
                { text: 'path', link: '/zh/stdlib/path' },
                { text: 'util', link: '/zh/stdlib/util' },
                { text: 'events', link: '/zh/stdlib/events' },
                { text: 'stream', link: '/zh/stdlib/stream' },
                { text: 'net', link: '/zh/stdlib/net' },
                { text: 'http', link: '/zh/stdlib/http' },
                { text: 'crypto', link: '/zh/stdlib/crypto' }
              ] },
              { text: 'Data-Format', collapsed: false, items: [
                { text: 'csv', link: '/zh/stdlib/csv' },
                { text: 'ini', link: '/zh/stdlib/ini' },
                { text: 'toml', link: '/zh/stdlib/toml' },
                { text: 'yaml', link: '/zh/stdlib/yaml' },
                { text: 'xml', link: '/zh/stdlib/xml' },
                { text: 'zlib', link: '/zh/stdlib/zlib' }
              ] },
              { text: 'Dev tooling', collapsed: false, items: [
                { text: 'logging', link: '/zh/stdlib/logging' }
              ] },
              { text: 'Ecosystem', collapsed: false, items: [
                { text: '包管理(LuaRocks)', link: '/zh/guide/rocks' },
                { text: '插件', link: '/zh/guide/plugins' },
                { text: '模块系统', link: '/zh/guide/modules' }
              ] }
            ]
          },
          {
            text: '其他',
            items: [
              { text: '配置与环境变量', link: '/zh/other/config' },
              { text: '错误模型与退出码', link: '/zh/other/errors' },
              { text: '构建与自测', link: '/zh/other/build' },
              { text: 'FAQ', link: '/zh/other/faq' },
              { text: '文档一致性审计', link: '/zh/审计-文档一致性' }
            ]
          },
          {
            text: '设计',
            items: [
              { text: '架构设计', link: '/zh/architecture' },
              { text: '事件循环后端', link: '/zh/loop-backend-design' },
              { text: 'Node 方向选型', link: '/zh/node-parity' }
            ]
          }
        ],
        docFooter: { prev: '上一篇', next: '下一篇' }
      }
    }
  }
})
