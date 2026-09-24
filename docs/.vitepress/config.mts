import { defineConfig } from 'vitepress'

export default defineConfig({
  lang: 'zh-CN',
  title: 'luna',
  description:
    'IPython 式的 Lua 交互环境,按 Node.js 运行时的方式生长:模块、插件、标准库',
  themeConfig: {
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
          { text: '事件循环', link: '/guide/loop' }
        ]
      },
      {
        text: '设计',
        items: [
          { text: '架构设计', link: '/architecture' },
          { text: '事件循环后端', link: '/loop-backend-design' }
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
