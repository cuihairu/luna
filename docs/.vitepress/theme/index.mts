// luna 主题:布局组件保持 VitePress 原装,只换设计令牌(custom.css)。
// 用 theme-without-fonts 入口:默认入口会经 Google Fonts 拉 Inter——
// 境内不可达、渲染被拖住,而中文正文也用不到它;字体栈在 custom.css 落。
import DefaultTheme from 'vitepress/theme-without-fonts'
import './custom.css'

export default DefaultTheme
