# TypeAnything

> **打拼音，落地任何语言、任何圈层、任何风格。**
> Type Chinese pinyin → press Enter → AI rewrites it as English / 留学生式中英夹杂 / 金融式说话 / 学术体 / 港片台词 / Klingon — anything you can describe.

[![IME](https://img.shields.io/badge/Windows-IME-blueviolet)](https://learn.microsoft.com/en-us/windows/win32/tsf/text-services-framework)
[![Built on](https://img.shields.io/badge/Built%20on-Weasel%20%2B%20librime-orange)](https://github.com/rime/weasel)
[![License](https://img.shields.io/badge/License-MIT-blue)](LICENSE)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen)]()

---

## 这不仅是翻译输入法。更是**风格化输入法**。

你打中文：

```
我们最近的项目数据不太好看，老板让我下周给个明确的方案
```

切不同 target，落地的是**完全不同的口吻**：

| 切换目标 | 落地结果 |
|---|---|
| `English` | Our recent project metrics aren't looking great. The boss wants a clear plan from me by next week. |
| `留学生式说话` | 我们 recent 的 project data 有点 ugly，老板 push 我下周给个 clear 的 plan。 |
| `金融式说话` | 这个 deal 最近的 KPI 不及预期，management 要求 next week 之前 deliver 一份 actionable 的 strategy memo。 |
| `互联网黑话` | 这个项目最近的数据有点压力，需要在下周前对齐一套有抓手、能闭环的赋能打法。 |
| `学术大佬式英语` | The project's recent metrics exhibit suboptimal performance; the supervisor expects a clearly articulated remediation strategy by next week. |
| `古汉语风格` | 近日吾辈所司之事，数据未尽人意。家主令吾翌周献清晰之策。 |
| `东北话` | 咱这项目最近数据贼难看，老板让我下周必须憋出个明明白白的方案来。 |
| `港片黑帮` | 我哋最近条 case 啲数啱啱睇唔过眼，大佬话下个礼拜要有个交代。 |

不止翻译。**你说啥风格，AI 就给你啥风格。**

---

## 它是什么

Windows OS 级输入法。你打拼音 → 空格选词 → Enter → 中文当场被 AI 改写为目标语言或风格 → 在**任意 Windows 应用**（微信 / Word / Chrome / VSCode / 飞书 / Discord / Slack）原地替换。

切换 target 不重启：托盘鱼图标右键 → **切换语言** → 弹框 → 输入任何描述。

---

## 风格清单（举例，不限）

### 🌐 语种 — 直接翻译

```
English / 日本語 / 한국어 / Français / Deutsch
Español / Italiano / Português / Русский / العربية
Tiếng Việt / ไทย / Türkçe / 粵語
... 任何 AI 能识别的语言
```

### 🎭 圈层风格 — 加入特定群体的说话方式

| 圈层 | 典型词汇 / 句式 |
|---|---|
| **金融式说话** | deal / EXIT / IRR / runway / cap table / Q3 / actionable / commit |
| **留学生式说话** | paper / final / stress / dorm / TA / lecture，连接词中文 |
| **互联网黑话** | 赋能 / 抓手 / 闭环 / 降本增效 / 打法 / 链路 / 心智 / 颗粒度 |
| **程序员式说话** | PR / MR / repo / deploy / merge / log / 上线 / 跑通 |
| **学术大佬式** | paper / cite / significant / sample size / methodology |
| **HR 式** | 人力成本 / 绩效 / 对齐 / 复盘 / 赋能 / 人才梯队 |
| **销售式** | 客户 / 转化 / 留资 / 成单 / 跟单 / 价值 |
| **二次元** | の / 大佬 / 我去 / 下次一定 / 滑稽 / 草 |
| **东北话** / **港式中文** / **台湾腔** / **老北京话** | 口语化方言 |

### 🎬 场景/文体 — 指定语气、年代、媒介

```
学术英语 / 商务日语 / 古汉语风格 / 文言文体
网络流行语 / 知乎体 / 小红书种草体 / 公众号文章体
B站弹幕体 / 抖音口播体 / 朋友圈鸡汤体 / 营销号体
```

### 🪐 虚构 / 自定义 — AI 自由发挥

```
像鲁迅一样的英语 / 像周杰伦歌词 / 港片黑帮台词
莎士比亚十四行诗 / 武侠小说体 / 玄幻爽文体
Klingon battle prose / 火星文 / Spanish chilango / Cockney slang
```

**你能用一句话描述的口吻，TypeAnything 就能给你那个口吻。**

---

## 为什么做它

跨语言/跨圈层协作的真正瓶颈不在「会不会说英文」「认不认识黑话」，在**打字速度 + 切换成本**——你能用拼音 250 字/分钟想出来的句子，硬要套进金融狗中英夹杂模板就剩 60 字/分钟。市面解法的通病：

| 通病 | TypeAnything 改进 |
|------|--------------------|
| 翻译插件只做语种切换（中→英） | **AI 改写**：语种 / 圈层 / 时代 / 文体 / 自定义任意切 |
| 浏览器插件方案只在 web 内有效 | **OS 级 TSF**，微信/Word/任何原生 app 都生效 |
| 「划词翻译」要先打中文再选 → 中断思路 | **打字过程内联**，Enter 当场替换，零额外动作 |
| sidecar / hook 方案微信里出现幽灵光标 | **直接 fork Weasel TSF**，框架级集成无 hook 冲突 |
| 谷歌输入法 + 翻译只支持 1-2 语言 | **任意自然语言描述**，1 秒切换风格 |
| 翻译质量靠默认 prompt（直译生硬） | **5 条 rule 专业翻译 prompt** + 风格描述 pass-through |
| 阻塞 LLM 调用导致 UI 卡死 | **后台 worker + 版本号校验**，用户继续打字不阻塞 |
| 鬼图标 / 维护中托盘气泡 / 12 项菜单噪音 | **托盘 4 项极简** + 神仙鱼品牌图标 |
| 安装要装 7 个组件、改 5 个注册表 | **一条命令**完成 binary 替换 + schema 部署 + TSF 注册 |
| 上锁文件无法替换需手动重启 | **MoveFileEx pending-on-reboot** 自动兜底 |

---

## 工作原理

```
┌─ 用户打字 ──────────────────────────────────────────┐
│  nihao  + [Space]                                  │
│   ↓                                                 │
│  Weasel TSF DLL → librime engine → 「你好」         │
│   ↓ commit_notifier 触发                            │
│  TypeAnything Rime processor 累积「你好」           │
└─────────────────────────────────────────────────────┘
                      ↓ [Enter]
┌─ 后台异步 ──────────────────────────────────────────┐
│  spawn worker thread (Chinese 仍可见 ~1s)           │
│   ↓                                                 │
│  WinHttpPost → AI provider chat/completions API     │
│   payload = professional translator prompt          │
│           + 你的目标 (任意自然语言描述)              │
│           + 「你好」                                 │
│   （默认连 DeepSeek，可在 schema yaml 改 OpenAI/...）│
│   target_lang ← %APPDATA%\Rime\typeanything_       │
│                  lang.txt (托盘菜单写入)             │
└─────────────────────────────────────────────────────┘
                      ↓ ~1s 后 LLM 返回
┌─ 静默替换 ──────────────────────────────────────────┐
│  SendInput VK_BACK × 「你好」.length                │
│   ↓                                                 │
│  SetClipboardUtf8("Hello")                          │
│   ↓                                                 │
│  SendPaste (Ctrl+V)  ← 绕开 IME 拦截                │
└─────────────────────────────────────────────────────┘
                      ↓
              「Hello」 落地原应用
```

---

## 安装

### 前置条件

- Windows 10 / 11
- [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) — Win10 20H1+ / Win11 默认已装，老版本 Win10 需手动装一次
- AI API key — 默认走 [DeepSeek](https://platform.deepseek.com/)（性价比 + 中文质量好），可换 OpenAI / Anthropic / 任意 OpenAI-compatible API（装完后在「模型配置」面板填）

> Weasel 不用先装。`ta-installer.exe` 自带 Weasel 0.17.4 全套二进制 + librime + TypeAnything 插件，一次装完。

### 一键安装（推荐）

1. 下载最新 release 的 `ta-installer.exe`：
   <https://github.com/A-cat-with-carrots/TypeAnything/releases/latest>
2. 双击 `ta-installer.exe` — UAC 提权
3. 选安装目录（默认 `C:\Program Files\Rime\weasel-0.17.4\`）→ 「开始安装」
4. 等进度条到 100% → 「配置模型」按钮直接弹模型配置面板，填 API key 保存
5. Win+Space 切到 **TypeAnything**

**装完不需要注销重登 / 重启电脑** — 新打开的应用立即生效，已开的应用需关掉重开。

### 卸载

控制面板 → 程序和功能 → 找到「TypeAnything」→ 卸载。或直接跑安装目录里的 `uninstall.exe`。用户字典与个人配置（`%APPDATA%\Rime`）会保留，方便日后重装时延用。

### 从源码自行构建

```powershell
choco install -y visualstudio2022buildtools python
# Boost 1.84 prebuilt 装到 C:\local\boost_1_84_0
Invoke-Expression (Invoke-Webrequest 'https://xmake.io/psget.text' -UseBasicParsing).Content

git clone https://github.com/A-cat-with-carrots/TypeAnything.git
cd TypeAnything\third_party\weasel\librime
git submodule update --init --recursive
.\build.bat librime         # → dist\lib\rime.dll

cd ..
.\_build_weasel_xmake.ps1   # → Weasel binary 套件

cd ..\..\tools\ta-settings
.\_build.ps1                # → ta-settings.exe（设置面板）

cd ..\ta-installer
.\_stage_embed.ps1          # 把所有 binary 资源 stage 进 installer
.\_build.ps1                # → ta-installer.exe（带 embedded 资源）
```

`ta-installer.exe` 内部干这些：
1. UAC 提权（embedded manifest `requireAdministrator`）
2. Kill 现有 Weasel 进程 + `MoveFileEx` pending-on-reboot 兜底
3. 写 binaries 到选定安装目录（备份原版到 `*.bak`）+ 替换 `system32\weasel.dll`
4. 写 `typeanything.schema.yaml` + `typeanything.dict.yaml` + `default.custom.yaml` + `weasel.custom.yaml` 到 `%APPDATA%\Rime\`
5. patch TSF 注册表 IME 描述为 `TypeAnything`
6. `WeaselDeployer.exe /deploy` 编译 schema（轮询 `typeanything.prism.bin` ≤ 60s）
7. 启 `WeaselServer.exe` 上托盘
8. 写 HKLM\Run 自启动 + Add/Remove Programs entry + 拷贝自身为 `uninstall.exe`

---

## 用法

### 基本流程

```
1. Win+Space 切到 TypeAnything
2. 任意输入框打拼音：women xia zhou yao kaihui
3. Space 选词 → 「我们下周要开会」
4. Enter → 中文消失 → 落地为目标风格
```

### 托盘菜单（右键鱼图标 / TSF 输入法图标）

```
切换风格  (L)   ← 打开「切换风格」WebView2 面板
模型配置  (M)   ← 打开「模型配置」WebView2 面板
检查更新  (U)   ← 打开 GitHub Releases 页
```

两项核心交互都由 `ta-settings.exe`（独立 WebView2 应用）承载，**不再走 PowerShell / WinForms**。深色 Fluent 设计沿用 [type.hrdai.cn](https://type.hrdai.cn) 品牌：teal `#15212a` 底 / 金 `#d4b27a` accent / Noto Serif SC 中文衬线 / Inter 英文 sans。

### 切换风格

右键 → **切换风格** 弹出面板，按 4 大类列出 chip：

- **🌐 语种** — English / 日本語 / 한국어 / Français / Deutsch / Español / 粵語 / Türkçe / ...
- **🎭 圈层风格** — 金融式说话 / 留学生式说话 / 互联网黑话 / 程序员式说话 / 学术大佬式 / HR 式 / 销售式 / 二次元 / 东北话 / 港式中文 / 台湾腔 / 老北京话
- **🎬 场景 / 文体** — 学术英语 / 商务日语 / 古汉语风格 / 网络流行语 / 知乎体 / 小红书种草体 / 公众号文章体 / B 站弹幕体 / 抖音口播体 / 营销号体
- **🪐 虚构 / 自定义** — 像鲁迅一样的英语 / 像周杰伦歌词 / 港片黑帮台词 / Klingon battle prose / Spanish chilango / 火星文 / ...

也可在输入框直接键入任意描述（默认 placeholder `English`，留空即默认）。保存后写到 `%APPDATA%\Rime\typeanything_lang.txt`，下次 Enter 触发翻译时 processor 重读文件，AI 按新描述翻译。

### 模型配置（接入自己的模型）

右键 → **模型配置** 弹出表单，5 个 provider 一键预设 + 4 个字段：

| 字段 | 默认值 | 说明 |
|---|---|---|
| **API Key** | 装完后在此填 | 你自己的 API key（mask 显示，可勾「显示」眼睛按钮）。**首次安装完进度页 → 「配置模型」按钮**直接弹此面板 |
| **Model** | `deepseek-chat` | 模型名（`gpt-4o` / `moonshot-v1-8k` / `glm-4-flash` / `qwen2.5:7b` / 任意） |
| **Host** | `api.deepseek.com` | 服务 host（`api.openai.com` / `api.moonshot.cn` / `open.bigmodel.cn` / `localhost:11434`） |
| **Path** | `/v1/chat/completions` | OpenAI Chat Completions 兼容协议 |

预设按钮一键填充：**DeepSeek** / **OpenAI** / **Moonshot** / **智谱** / **Ollama**（本地）。

保存后：
- 写到 `%APPDATA%\Rime\typeanything.schema.yaml`（仅本机，没有上传）
- 自动 kill + 重启 WeaselServer 让 processor 重读新配置
- 秒级生效

**隐私保证**：你的输入只发到你填的 endpoint，没有中转代理 / shared backend / vendor lock-in。换 provider 改一行就好。

### 拼音 → 汉字 自学习

Rime user_dict 默认开启，每次选词增加频次到 `%APPDATA%\Rime\luna_pinyin.userdb`。schema 里加强参数：

```yaml
translator:
  enable_user_dict:  true   # 频次持久化
  enable_encoder:    true   # 多字 composition 自动编码为新词组
  enable_sentence:   true   # 整句候选浮上来
  enable_completion: true   # 简写 (xz → 现在) 也能匹配
  initial_quality:   1.2    # user_dict 权重高于 base dict
```

用得越多，候选顺序越贴合你常用词。

### 标点直输（无候选窗）

schema 覆盖 punctuator preset，对齐 Microsoft IME 行为：
- 中文模式 (ascii_mode=false)：`,` → `，` / `.` → `。` / `(` → `（` / `\` → `、` / 引号自动配对
- 英文模式 (ascii_mode=true)：全 ASCII 半角直输

不再弹「、 \ \」三选一候选窗。

---

## 文件结构

```
TypeAnything/
├── README.md                                    本文档
├── LICENSE                                      MIT
├── fish.ico                                    神仙鱼品牌图标
├── tools/
│   ├── ta-installer/                           ★ 单 exe 安装器（含全部资源 embed）
│   │   ├── main.cpp                            UAC + 安装 / 卸载 worker
│   │   ├── installer.rc                        IDR_* 资源：Weasel 套件 / ta-settings.exe / schema yaml / UI
│   │   ├── resource.h
│   │   ├── manifest.xml                        requireAdministrator
│   │   ├── ui/                                 WebView2 安装向导：选目录 → 进度 → 完成
│   │   └── _stage_embed.ps1 + _build.ps1
│   └── ta-settings/                            ★ 托盘菜单「切换风格」+「模型配置」WebView2 面板
│       ├── main.cpp                            webview 宿主 + JS↔C++ bridge
│       ├── app.rc                              fish.ico embed → 窗口 / 任务栏图标
│       ├── ui/
│       │   ├── index.html                      SPA: ?page=lang | ?page=model
│       │   ├── style.css                       hrdai dark editorial (teal+金)
│       │   ├── app.js                          chips/preset 逻辑 + bridge 调用
│       │   └── fish.png                        title bar logo
│       ├── vendor/
│       │   ├── webview.h                       zserge/webview v0.10 single-header MIT
│       │   └── webview2/                       Microsoft.Web.WebView2 SDK
│       ├── xmake.lua + _build.ps1              build script
│       └── fish.ico
└── third_party/
    └── weasel/                                  fork 自 rime/weasel
        ├── WeaselServer/
        │   └── WeaselServerApp.cpp              ★ ShellExecute ta-settings.exe
        ├── WeaselTSF/
        │   ├── LanguageBar.cpp                  ★ ShellExecute ta-settings.exe
        │   └── WeaselTSF.rc                     ★ 3 项菜单
        ├── WeaselUI/
        │   └── WeaselPanel.cpp                  ★ 候选窗 status icon 关掉
        ├── resource/                            ★ 6 个 ICO 神仙鱼 + 中/A 模式徽章
        ├── include/
        │   └── resource.h                       ★ ID_WEASELTRAY_SWITCH_LANG + MODEL_CONFIG
        └── librime/plugins/typeanything/        ★ 我们的 Rime 插件
            ├── src/
            │   ├── typeanything_processor.cc    ★ ResolveTargetLang 自由格式 + 异步翻译
            │   ├── typeanything_processor.h
            │   └── typeanything_module.cc
            ├── schema/
            │   └── typeanything.schema.yaml     ★ luna_pinyin + abbrev + 标点直输
            └── CMakeLists.txt
```

★ = TypeAnything 改动 / 新增。

---

## 自定义

### 改 LLM provider

默认连 DeepSeek（中文质量好 + 价格低）。每个用户用**自己的 API key 直连**，没有 shared backend / 中转代理 — 你的输入只发到你填的那个 API endpoint。

`typeanything.schema.yaml`：

```yaml
typeanything:
  api_key: "sk-..."          # 你自己的 API key
  model: deepseek-chat       # → "gpt-4o" / "moonshot-v1-8k" / 任意
  host: api.deepseek.com     # → "api.openai.com" / 任意 OpenAI-compatible host
  path: /v1/chat/completions # OpenAI Chat Completions 兼容协议
  temperature: 0.3
```

**支持任意 OpenAI Chat Completions 兼容 API**：DeepSeek / OpenAI / Moonshot / 阶跃 / 智谱 / 任何 self-hosted vLLM / Ollama 等。Anthropic `/v1/messages` 协议不同，需改 processor 的请求 builder。

---

## 已知限制

- **macOS / Linux 暂不支持** — 需用 squirrel (macOS) / fcitx-rime (Linux) 重新 fork
- **新打开的应用立即生效，已开的应用需关掉重开** — TSF DLL 被每个文本输入进程持锁；installer 走 `MoveFileEx` pending-on-reboot 兜底，**整机重启不需要**
- **WebView2 runtime 依赖** — 设置面板用 Edge WebView2。Win10 20H1+ / Win11 默认装；老 Win10 (1909/1903) 需手动装一次
- **切换风格只影响下次 Enter** — processor 在 Enter 触发时才重读 `typeanything_lang.txt`，已落地的文本不回译
- **首次启动 schema 编译 ~3-5s** — librime 首次编译 `luna_pinyin.table.bin` (13MB)，仅一次性

---

## 版本历史

### v0.6.5（当前）
- **冷装修复**：installer cold-register TSF + bundle luna_pinyin 全套数据 + 安装失败真报错（不再 silent 100%）+ 写开始菜单 `TypeAnything.lnk → WeaselServer.exe`
- **WeaselServer 跑 MEDIUM IL**（`CreateProcessWithTokenW` + explorer token）让 IME 在普通应用（Notepad / 微信 / Chrome / Word）正常工作，pipe ACL 不再被拒
- **设置面板单实例 + tab 切换 + 强制置顶**：托盘点「切换风格」/「模型配置」复用现有窗口，`AttachThreadInput` 技巧绕过 SetForegroundWindow 限制
- **每 provider 独立 API key**：`%APPDATA%\Rime\typeanything.keyring.json` 存 DeepSeek/Moonshot/智谱/OpenAI/Ollama/custom 各自 key，切 preset 不再串
- **自定义 preset**：清空 placeholder + 字段，用户从零填
- **当前 provider 标识**：`.preset.current` 金底 +「当前」角标 = runtime 实际在用的那个
- **候选框样式调优**：style/layout/* 字段（而非顶级 style/*）才真正驱动 layout — font 11pt YaHei UI、margin 8/6、hilite_padding 4、candidate_spacing 24、金色 mark
- **WeaselServer 版本从 registry 读**：`HKLM\...\Uninstall\TypeAnything\DisplayVersion` 单一来源，发版只改 installer 一处
- **检查更新弹窗 MB_TOPMOST + MB_SETFOREGROUND**：不再藏到后台
- **`taskkill /T` flag 去掉**：避免 ta-settings（WeaselServer 子进程）被一起 kill

### v0.6.2
- **纯净模式** — 标题栏右侧 toggle 开关，开启后写 `off` sentinel 到 lang 文件，UI 整体置灰锁定，Enter 直通宿主（不再被 IME 吞掉），实际效果 = 纯拼音中文输入法。模型配置页不显示该开关。
- **UI 收尾打磨**
  - chip 点击后输入框光标置末尾、无蓝色高亮（`setSelectionRange(n,n)` 替代 `select()`）
  - 用户手动编辑或聚焦时清除 chip 的 active 高亮
  - 保存 toast 简化为「已保存」，不附带值
  - 保存后不再自动关窗 — 用户可继续改
  - 「off」sentinel 在输入框显示为「无」
  - 模型配置页加 endpoint live preview（`POST  https://host{path}`）+ 2-column form-grid 排版，窗口 660×640

### v0.5
- **WebView2 设置面板** — 托盘的「切换风格」+「模型配置」从 PowerShell InputBox / WinForms 改为独立 `ta-settings.exe` 嵌 WebView2 + HTML/CSS
  - hrdai dark editorial 设计（teal `#15212a` + 金 `#d4b27a`，Noto Serif SC 衬线 + Inter sans）
  - 神仙鱼 ICO 嵌入资源 → 窗口 / 任务栏图标
  - chip / preset hover 金描边、active 金底深字
  - JS ↔ C++ bridge：read/write schema yaml & lang.txt，保存自动重启 WeaselServer
  - WebView2 user-data folder 强制走 `%LOCALAPPDATA%\TypeAnything\WebView2`（避开 Program Files 只读）
  - 标点直输（schema yaml 覆盖 punctuator preset，对齐 Microsoft IME）
  - 取消所有 PowerShell spawn — 不再受 PS 冷启动 / 沙箱进程拦截影响

### v0.4
- **任意自然语言目标** — 切换语言改 PowerShell InputBox，user 输入任意 AI 能理解的描述
  - 4 大类：语种 / 圈层风格 / 场景文体 / 虚构自定义
  - **金融式说话 / 留学生式说话 / 互联网黑话** 等圈层风格是核心卖点
- **模型配置 GUI** — 第一版用 WinForms（已在 v0.5 替换为 WebView2）
- **拼音→汉字 自学习强化** — Rime user_dict 增强（encoder/sentence/completion/initial_quality）
- 图标 0 margin 紧贴
- 托盘菜单减到 3 项
- WinSparkle 自动检查彻底删除
- 删字 bug fix
- install 脚本 5s 后自动 exit

### v0.3
- 30 语言托盘热切（English / 日本語 / 한국어 / 粵語 + 26 种）
- 神仙鱼品牌图标全套
- 托盘菜单 4 项极简
- 5 条 rule 专业翻译 prompt
- 关闭候选窗 status icon
- 检查更新跳转 GitHub Releases 页面
- install 脚本：MoveFileEx pending-on-reboot + 轮询 schema 编译

---

## 贡献

重点方向：
- **macOS port** — 用 squirrel 框架，复用我们的 typeanything plugin
- **Linux port** — 用 ibus-rime 或 fcitx-rime
- **流式翻译** — LLM streaming API → 边翻边显示
- **错误 toast** — API 失败时托盘气泡提示
- **风格 preset 库** — 内建几十种圈层风格 prompt template
- **离线 fallback** — 本地小 model 兜底

PR 前请阅 `third_party/weasel/librime/plugins/typeanything/src/typeanything_processor.cc` 了解核心翻译/替换链路。

---

## 致谢

- [rime/weasel](https://github.com/rime/weasel) — Windows TSF 框架，我们 fork 自此
- [rime/librime](https://github.com/rime/librime) — Rime 输入法引擎
- [DeepSeek](https://platform.deepseek.com/) — 默认 LLM provider（性价比高、中文好），用户可在 schema yaml 切到任意 OpenAI-compatible API
- 神仙鱼 logo 来自 [hrdai.com](https://hrdai.com) 品牌系统

---

## License

MIT — 详见 [LICENSE](LICENSE)。

Weasel 上游代码（`third_party/weasel/`）保持其原 license（GPL v3 / BSD-3-Clause）；本仓库新增代码以 MIT 发布。
