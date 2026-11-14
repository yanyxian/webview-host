# 🚀 MathSlide (轻量级 HTML 演示器 / Lightweight HTML Presenter)

[![Platform](https://img.shields.io/badge/Platform-Windows-blue.svg)]()
[![C++](https://img.shields.io/badge/C++-Pure_C_Style-purple.svg)]()
[![Size](https://img.shields.io/badge/Size-79KB-success.svg)]()
[![License](https://img.shields.io/badge/License-MIT-green.svg)]()

**MathSlide** 是一个基于纯 C 风格、C++ 与原生 Win32 API 编写的极简、超轻量级本地 HTML 演示器。

它最初是为了在一个初中数学分享会上，完美、流畅、离线地展示基于 `Reveal.js`、`KaTeX` 和 `SVG` 制作的互动幻灯片而诞生的。为了彻底摆脱校园老旧电脑的卡顿浏览器和杀毒软件拦截，它被逼成了一台**极致的轻量化单文件机器**。

包含核心代码、内嵌图标、WebView2 静态加载器，**整个可执行文件（.exe）仅有 77 KB！**

---

## ✨ 核心特性 (Features)

- **📦 极致体积 (77KB)**：彻底抛弃 `<filesystem>` 和 `std::wstring`，纯 C 风格手写字符串与目录遍历。静态链接 WebView2 Loader，**零外部 DLL 依赖**，拷进 U 盘双击即用。
- **🛡️ 告别跨域 (No CORS)**：内置 `SetVirtualHostNameToFolderMapping` 虚拟主机映射，完美解决本地 `file://` 协议下的模块加载、Fetch 请求等跨域拦截问题。
- **🖥️ 高 DPI 像素级清晰**：内建 `PerMonitorV2` 感知，无论在 1080P 还是 4K 触摸屏上，网页文字与 SVG 渲染绝不模糊。
- **🔒 系统级缓存隔离**：自动在 `%TEMP%` 目录下按路径哈希生成专属缓存，不污染本地课件目录，支持多开不冲突。
- **🌓 智能多级降级路由**：支持 `StartUrl=index.html | {exe}.html | showlist` 形式的降级链。找不到配置的文件时，会自动生成本地目录浏览界面，**支持无限穿透子目录**。
- **⌨️ 防锁死快捷键**：全屏演示时，支持长按 `ESC` 1.5秒 强退，或按 `F1` 秒退。单按 `ESC` 会完美放行给网页（不干扰 Reveal.js 缩略图）。
- **⚙️ 全配置驱动**：无缝兼容 UTF-8 / ANSI / UTF-16 编码的 `config.ini`。

---

## 🛠️ 快速上手 (Usage)

1. 从 [Releases](<你的GitHub链接>/releases) 下载最新的 `MathSlide.zip` 并解压。
2. 将你的网页文件（如 `index.html` 及同目录资源）放在与 `MathSlide.exe` 同一文件夹下。
3. 双击 `MathSlide.exe`，即刻全屏演示！

### 命令行参数 (CLI)
```bash
MathSlide.exe                     # 默认加载当前目录下的 index.html 或 config.ini 配置
MathSlide.exe <文件.html>         # 强行加载指定的 HTML 文件
MathSlide.exe <文件夹>            # 强行加载指定文件夹内的 index.html
MathSlide.exe <路径> -f           # 无边框全屏模式启动
MathSlide.exe -c "dark.ini"       # 加载指定的配置文件
