# TypeAnything

> **打拼音，落地任何语言、任何圈层、任何风格。**
> Type Chinese pinyin → press Enter → DeepSeek rewrites it as English / 留学生式中英夹杂 / 金融式说话 / 学术体 / 港片台词 / Klingon — anything you can describe.

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

不止翻译。**你说啥风格，DeepSeek 就给你啥风格。**

---

## 它是什么

Windows OS 级输入法。你打拼音 → 空格选词 → Enter → 中文当场被 DeepSeek 改写为目标语言或风格 → 在**任意 Windows 应用**（微信 / Word / Chrome / VSCode / 飞书 / Discord / Slack）原地替换。

切换 target 不重启：托盘鱼图标右键 → **切换语言** → 弹框 → 输入任何描述。

---

## 风格清单（举例，不限）

### 🌐 语种 — 直接翻译

```
English / 日本語 / 한국어 / Français / Deutsch
Español / Italiano / Português / Русский / العربية
Tiếng Việt / ไทย / Türkçe / 粵語
... 任何 DeepSeek 认识的语言
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

### 🪐 虚构 / 自定义 — DeepSeek 自由发挥

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
| 翻译插件只做语种切换（中→英） | **风格化**：圈层 / 时代 / 文体 / 自定义 任意切 |
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
│  WinHttpPost → api.deepseek.com/v1/chat/completions │
│   payload = professional translator prompt          │
│           + 你的目标 (任意自然语言描述)              │
│           + 「你好」                                 │
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
- Weasel 0.17.4 已装：[官方下载](https://rime.im/download/)（IME 框架）
- DeepSeek API key（[注册](https://platform.deepseek.com/) → 创建 key）

### 一键安装（推荐）

1. 下载最新 release 的 `TypeAnything-vX.Y.Z.zip`：
   <https://github.com/A-cat-with-carrots/TypeAnything/releases/latest>
2. 解压
3. 双击 `Install-TypeAnything.bat` — UAC 提权 → 弹框输入 API key → 弹框输入默认目标 → 自动部署
4. **注销重登或重启电脑** — 让所有 TSF 客户端进程加载新 weaselx64.dll
5. Win+Space 切到 **TypeAnything**

### 从源码自行部署

```powershell
# 管理员 PowerShell
git clone https://github.com/A-cat-with-carrots/TypeAnything.git
cd TypeAnything
.\install-typeanything-to-weasel.ps1 -ApiKey "sk-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
```

部署脚本干这些：
1. 优雅 `/q` 关 WeaselServer + `taskkill /F /T` + `MoveFileEx` pending-on-reboot 兜底
2. 备份 Weasel 原版 binary 到 `*.bak`
3. 替换 `rime.dll` / `weaselx64.dll` / `WeaselServer.exe` / `WeaselDeployer.exe` + `system32\weasel.dll`
4. 隐藏其他 schema（让 Deployer 「方案列表」只剩 TypeAnything）
5. 写 `typeanything.schema.yaml` + 注入 API key
6. 改 TSF 注册表 IME 描述为 `TypeAnything`
7. 重 deploy schema（轮询 `typeanything.prism.bin` 写入完成）+ 重启 server

---

## 用法

### 基本流程

```
1. Win+Space 切到 TypeAnything
2. 任意输入框打拼音：women xia zhou yao kaihui
3. Space 选词 → 「我们下周要开会」
4. Enter → 中文消失 → 落地为目标风格
```

### 切换目标风格

```
托盘神仙鱼图标右键 → 切换语言 → 弹框 → 输入任何描述
↓
%APPDATA%\Rime\typeanything_lang.txt 第一行更新
↓
下次 Enter 触发翻译时 processor 重读文件 → DeepSeek 按新描述翻译
```

弹框示例描述：
- `English` / `日本語`
- `金融式说话` / `留学生式说话` / `互联网黑话`
- `古汉语风格` / `小红书种草体`
- `像鲁迅一样的英语` / `Klingon battle prose`
- 任何你能想到的描述

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
├── install-typeanything-to-weasel.ps1         源码构建路径的部署脚本
├── Install-TypeAnything.bat                   一键安装包入口（UAC + GUI）
├── Install-TypeAnything.ps1                   GUI 包装
├── fish.ico                                     神仙鱼品牌图标
└── third_party/
    └── weasel/                                  fork 自 rime/weasel
        ├── WeaselServer/
        │   └── WeaselServerApp.cpp              ★ 切换语言 PS InputBox
        ├── WeaselTSF/
        │   ├── LanguageBar.cpp                  ★ 切换语言 PS InputBox（客户端进程）
        │   └── WeaselTSF.rc                     ★ 4 项菜单
        ├── WeaselUI/
        │   └── WeaselPanel.cpp                  ★ 候选窗 status icon 关掉
        ├── resource/                            ★ 6 个 ICO 神仙鱼 + 中/A 模式徽章
        ├── include/
        │   └── resource.h                       ★ 加 ID_WEASELTRAY_SWITCH_LANG
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

`typeanything.schema.yaml`：

```yaml
typeanything:
  api_key: "sk-..."
  model: deepseek-chat
  host: api.deepseek.com
  path: /v1/chat/completions
  temperature: 0.3
```

OpenAI / Anthropic 兼容协议直接改 host + path。Anthropic `/v1/messages` 协议不同需改 processor 请求 builder。

### 改翻译 prompt

`typeanything_processor.cc` 里 `kSystemPrompt` 字符串（5 条 rule 在那）。

### 改图标

```python
from PIL import Image
im = Image.open("your-logo.png").convert("RGBA")
im.save("fish.ico", format="ICO",
        sizes=[(16,16),(20,20),(24,24),(32,32),(40,40),(48,48),
               (64,64),(96,96),(128,128),(256,256)])
```

替换 `third_party/weasel/resource/weasel.ico` + `fish.ico` + 其他 5 个 ICO。

---

## 从源码构建

```powershell
choco install -y visualstudio2022buildtools
choco install -y python
# Boost 1.84 prebuilt: 装到 C:\local\boost_1_84_0
Invoke-Expression (Invoke-Webrequest 'https://xmake.io/psget.text' -UseBasicParsing).Content

cd third_party\weasel\librime
git submodule update --init --recursive
.\build.bat librime         # → dist\lib\rime.dll

cd ..
.\_build_weasel_xmake.ps1   # → build\windows\x64\release\*\*.exe + .dll

.\install-typeanything-to-weasel.ps1 -ApiKey "..."
```

---

## 已知限制

- **macOS / Linux 暂不支持** — 需用 squirrel (macOS) / fcitx-rime (Linux) 重新 fork
- **新 weaselx64.dll 真生效需注销重登** — 旧 dll 被每个文本输入进程持锁
- **Switch Language 改后已打的字不回译** — 只影响下次 Enter 触发
- **网络断 / API 报错时保留中文 + LOG(ERROR)** — 不静默吞错
- **首次 schema build ~3-5s** — librime 编译 luna_pinyin.table.bin (13MB)
- **微信 ghost cursor 修了，但仅限 PC 端** — 微信小程序内部 webview 仍走 Chrome IME 路径

---

## 版本历史

### v0.4.x（当前）
- **任意自然语言目标** — 切换语言改 PowerShell InputBox，user 输入任意 DeepSeek 能理解的描述
  - 4 大类：语种 / 圈层风格 / 场景文体 / 虚构自定义
  - **金融式说话 / 留学生式说话 / 互联网黑话** 等圈层风格是核心卖点
- **拼音→汉字 自学习强化** — Rime user_dict 增强（encoder/sentence/completion/initial_quality）
- **标点直输** — schema 覆盖 punctuator preset，单候选直接出，对齐 Microsoft IME
- **图标 0 margin 紧贴**，每 size 独立 LANCZOS 渲染
- WinSparkle 自动检查彻底删除（不再弹「新版本 0.17.4」跳 rime.im）
- 切换语言菜单从 server-side popup 改 TSF DLL 弹 PS InputBox
- 删字 bug fix：BackSpace 在 composition 外时 pop accumulated_ 末尾 UTF-8 char
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
- **流式翻译** — DeepSeek streaming API → 边翻边显示
- **错误 toast** — API 失败时托盘气泡提示
- **风格 preset 库** — 内建几十种圈层风格 prompt template
- **离线 fallback** — 本地小 model 兜底

PR 前请阅 `third_party/weasel/librime/plugins/typeanything/src/typeanything_processor.cc` 了解核心翻译/替换链路。

---

## 致谢

- [rime/weasel](https://github.com/rime/weasel) — Windows TSF 框架，我们 fork 自此
- [rime/librime](https://github.com/rime/librime) — Rime 输入法引擎
- [DeepSeek](https://platform.deepseek.com/) — LLM provider，性价比 + 中文质量
- 神仙鱼 logo 来自 [hrdai.com](https://hrdai.com) 品牌系统

---

## License

MIT — 详见 [LICENSE](LICENSE)。

Weasel 上游代码（`third_party/weasel/`）保持其原 license（GPL v3 / BSD-3-Clause）；本仓库新增代码以 MIT 发布。
