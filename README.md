# TypeAnything

> **Type Chinese pinyin, get any of 30 languages — at the OS level, in any app.**
> 在任意应用打拼音 → 空格选词 → Enter 触发 LLM 翻译 → 中文当场被替换为目标语言文本

[![IME](https://img.shields.io/badge/Windows-IME-blueviolet)](https://learn.microsoft.com/en-us/windows/win32/tsf/text-services-framework)
[![Built on](https://img.shields.io/badge/Built%20on-Weasel%20%2B%20librime-orange)](https://github.com/rime/weasel)
[![License](https://img.shields.io/badge/License-MIT-blue)](LICENSE)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen)]()

---

## 它是什么

把你打的拼音——

```
nihao  → [Space 选「你好」] → [Enter] → Hello
```

——在**任意 Windows 应用**（微信 / Word / Chrome / VSCode / 记事本 / Discord / Slack…）原地替换为目标语言文本：

- **30 种语言一键切换**（托盘菜单选）：English / 日本語 / 한국어 / 粵語 / Français / Deutsch / Español / Italiano / Português / Русский / العربية / Tiếng Việt / ไทย / Bahasa Indonesia / Türkçe / हिन्दी / Nederlands / Polski / Svenska / Ελληνικά / עברית / فارسی / Українська / Čeština / Dansk / Suomi / Norsk / Magyar / Română / Bahasa Melayu
- **TSF 级集成**（Text Services Framework）— 不是 IME 上面套层 hook，是 Windows 输入法本体，微信不会闪鬼影光标
- **后台异步 LLM**（DeepSeek `deepseek-chat`，p99 ~1s）— 中文先落地，翻译来了通过 SendInput VK_BACK + clipboard 静默替换
- **专业级翻译 prompt**（5 条 rule：避免直译/保留语气/技术术语/俚语自然化/语境推断）

不是「IME 翻译插件」。是 **fork 整个 Weasel C++ TSF 框架 + librime 引擎，加自研 Rime processor 插件 + 自研 30 国语言切换托盘菜单 + 神仙鱼品牌图标 + 专业翻译 prompt** 的合体输入法产品。

---

## 为什么做它

跨语言协作的真正瓶颈不在「会不会说英文」，在**打字速度**——你能用拼音 250 字/分钟想出来的句子，用英文键盘只剩 60 字/分钟。市面上的解法普遍：

| 通病 | TypeAnything 改进 |
|------|--------------------|
| 浏览器插件方案（DeepL Tab 等）只在 web 内有效 | **OS 级 TSF**，微信/Word/任何原生 app 都生效 |
| 「划词翻译」要先打中文再选 → 中断思路 | **打字过程内联**，Enter 当场替换，零额外动作 |
| sidecar / hook 方案微信里出现幽灵光标（双 caret） | **直接 fork Weasel TSF**，框架级集成无 hook 冲突 |
| 谷歌输入法 + 翻译只支持 1-2 语言 | **30 语言托盘热切**，1 秒切换目标语言 |
| 翻译质量靠默认 prompt（直译生硬） | **5 条 rule prompt**：保留语气/上下文推断/技术术语保留/俚语自然化/避免冗词 |
| 阻塞 LLM 调用导致 UI 卡死 | **后台 worker 线程 + 版本号校验**，用户继续打字不阻塞，过期请求丢弃 |
| 鬼图标 / 维护中托盘气泡 / 6 项菜单选项噪音 | **托盘 4 项极简**：切换语言 / 检查更新 / 重启 / 退出，神仙鱼品牌图标 |
| 安装要装 7 个组件、改 5 个注册表 | **一条 PowerShell** 命令完成 binary 替换 + schema 部署 + TSF 注册 + 描述改名 |
| 上锁文件无法替换需手动重启 | **MoveFileEx pending-on-reboot** 自动兜底，graceful `/q` IPC + `taskkill /F /T` 三段 kill |

---

## 工作原理

```
┌─ 用户打字 ──────────────────────────────────────────┐
│  nihao  + [Space]                                  │
│   ↓                                                 │
│  Weasel TSF DLL → librime engine → 「你好」         │
│   ↓ commit_notifier 触发                            │
│  TypeAnything Rime processor 累积「你好」         │
└─────────────────────────────────────────────────────┘
                      ↓ [Enter]
┌─ 后台异步 ──────────────────────────────────────────┐
│  spawn worker thread (Chinese 仍可见 ~1s)           │
│   ↓                                                 │
│  WinHttpPost → api.deepseek.com/v1/chat/completions │
│   payload = professional translator prompt + 你好  │
│   target_lang ← %APPDATA%\Rime\typeanything_      │
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
- 可选：从源码自行 build（VS 2022 BuildTools + xmake + boost 1.84，详见「从源码构建」）

### 一条命令部署

```powershell
# 管理员 PowerShell
git clone https://github.com/A-cat-with-carrots/TypeAnything.git
cd TypeAnything
.\install-typeanything-to-weasel.ps1 -ApiKey "sk-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
```

脚本干这些：
1. 优雅 `/q` 关 WeaselServer + `taskkill /F /T` + `MoveFileEx` pending-on-reboot 兜底
2. 备份 Weasel 原版 binary 到 `*.bak`
3. 替换 `rime.dll` / `weaselx64.dll` / `WeaselServer.exe` / `WeaselDeployer.exe` + `system32\weasel.dll`
4. 隐藏其他 schema（让 Deployer 「方案列表」只剩 TypeAnything）
5. 写 `typeanything.schema.yaml` + 注入 API key
6. 改 TSF 注册表 IME 描述为 `TypeAnything`
7. 重 deploy + 重启 server

完成后**注销重登**或**重启**让所有 TSF 客户端进程加载新 `weaselx64.dll`。然后 Win+Space 切到 TypeAnything。

### 切换目标语言

托盘右下角 → 神仙鱼图标右键 → **切换语言** → 选 30 语言之一。立即生效，无需重启。

---

## 用法

### 流程速览

```
1. Win+Space 切到 TypeAnything
2. 任意输入框打拼音：nihao shijie
3. Space 选词 → 「你好世界」
4. Enter → 中文消失 → 「Hello world」 落地
```

### 改语言不重启

```
托盘鱼图标右键 → 切换语言 → 选 Français
↓
Rime/typeanything_lang.txt 写入 "fr"
↓
下次 Enter 触发翻译时 processor 重读文件 → "Bonjour le monde"
```

---

## 文件结构

```
TypeAnything/
├── README.md                                    本文档
├── LICENSE                                      MIT
├── install-typeanything-to-weasel.ps1         源码构建路径的部署脚本（含 MoveFileEx 兜底）
├── Install-TypeAnything.bat                   一键安装包入口（自动 UAC 提权，弹 API key 输入框）
├── fish.ico                                     神仙鱼品牌图标
└── third_party/
    └── weasel/                                  fork 自 rime/weasel（C++ TSF 框架）
        ├── WeaselServer/                        托盘 server — 改 4 项菜单 + 30 语言切换弹窗
        │   ├── WeaselServerApp.cpp              ★ 加 Switch Language popup + 写 lang.txt
        │   ├── WeaselServer.rc                  ★ 减菜单到 4 项（切换/检查/重启/退出）
        │   └── WeaselTrayIcon.cpp               ★ 删「维护中」气泡通知
        ├── WeaselUI/
        │   └── WeaselPanel.cpp                  ★ 候选窗 status icon 关掉
        ├── WeaselTSF/                           TSF DLL — 微信兼容核心
        ├── WeaselDeployer/                      Schema 编译器
        ├── RimeWithWeasel/                      Weasel ↔ librime 胶水
        ├── resource/                            ★ 6 个 ICO 全替换神仙鱼
        │   ├── weasel.ico zh.ico en.ico
        │   ├── full.ico half.ico reload.ico
        ├── include/
        │   ├── resource.h                       ★ 加 ID_WEASELTRAY_SWITCH_LANG / LANG_BASE
        │   └── WeaselUtility.h                  ★ get_weasel_ime_name() 永远返 TypeAnything
        ├── _build_weasel_xmake.ps1              xmake 构建脚本（绕过 msbuild FileTracker bug）
        ├── xmake.lua                            ★ 含 boost prebuilt + ATL 检测 + INCLUDE 大写修正
        └── librime/
            └── plugins/
                └── typeanything/              ★ 我们的核心 Rime 插件
                    ├── src/
                    │   ├── typeanything_processor.cc   ★ 30 lang + WinHttp + commit hook + 异步替换
                    │   ├── typeanything_processor.h
                    │   └── typeanything_module.cc      RIME_REGISTER_MODULE
                    ├── schema/
                    │   └── typeanything.schema.yaml    ★ luna_pinyin + simplifier + DeepSeek 配置
                    └── CMakeLists.txt
```

★ = TypeAnything 改动 / 新增。其他保持上游兼容。

---

## 自定义

### 1. 加新语言

编辑 `third_party/weasel/librime/plugins/typeanything/src/typeanything_processor.cc` 的 `kLangs[]` 数组：

```cpp
static const LangEntry kLangs[] = {
  {"en", "English"},
  {"ja", "Japanese"},
  // 加这行：
  {"sw", "Swahili"},
};
```

同步加到 `WeaselServer/WeaselServerApp.cpp` 的同名数组（托盘菜单也要新项）。重 build 即可。

### 2. 改 LLM provider

`typeanything.schema.yaml`：

```yaml
typeanything:
  api_key: "sk-..."
  model: deepseek-chat       # → claude-3-5-sonnet-20241022
  host: api.deepseek.com     # → api.anthropic.com
  path: /v1/chat/completions # → /v1/messages
  temperature: 0.3
```

OpenAI / Anthropic 兼容协议直接换 host + path 即可。Anthropic `/v1/messages` 协议不同需改 processor 的请求 builder。

### 3. 改翻译 prompt

`typeanything_processor.cc` 里 `kSystemPrompt` 字符串 — 5 条 rule 写在那。

### 4. 改图标

替换 `fish.ico` + `third_party/weasel/resource/*.ico`。多分辨率 ICO 用 PIL 转：

```python
from PIL import Image
im = Image.open("your-logo.png").convert("RGBA")
im.save("fish.ico", format="ICO",
        sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
```

### 5. 不替换 system32\weasel.dll（保留原 Weasel 图标）

`install-typeanything-to-weasel.ps1` 注释掉 `Copy-LockedOrPending $srcTSF $sys32Dll` 那段。语言栏图标会保留 Weasel 默认（不变神仙鱼），但 Weasel 安装目录里的所有 DLL/EXE 仍是 TypeAnything 版。

---

## 从源码构建

```powershell
# 装依赖
choco install -y visualstudio2022buildtools  # 或装 VS Community
choco install -y python                       # 3.10+
# Boost 1.84 prebuilt：
# 下载 https://sourceforge.net/projects/boost/files/boost-binaries/1.84.0/
# 选 boost_1_84_0-msvc-14.3-64.exe → 装到 C:\local\boost_1_84_0
# xmake：
Invoke-Expression (Invoke-Webrequest 'https://xmake.io/psget.text' -UseBasicParsing).Content

# clone + build librime（含我们的 plugin）
cd third_party\weasel\librime
git submodule update --init --recursive
.\build.bat               # 产 dist\lib\rime.dll

# build Weasel UI（TSF/Server/Deployer）
cd ..
.\_build_weasel_xmake.ps1
```

产物路径：
- `librime\dist\lib\rime.dll` — librime 含 typeanything plugin
- `build\windows\x64\release\WeaselServer\WeaselServer.exe` — 托盘 server
- `build\windows\x64\release\WeaselDeployer\WeaselDeployer.exe`
- `build\windows\x64\release\WeaselTSF\weaselx64.dll` — TSF DLL

跑 `.\install-typeanything-to-weasel.ps1 -ApiKey "..."` 部署。

---

## 已知限制

- **macOS / Linux 暂不支持** — 用 squirrel (macOS) / fcitx-rime (Linux) 重新 fork 等量工作没做
- **新 weaselx64.dll 真生效需注销重登** — 旧 dll 被每个文本输入进程持锁
- **Switch Language 改后已打的字不回译** — 只影响下次 Enter 触发；想重译需复制原中文重新 Enter
- **网络断 / API 报错时静默丢弃** — 当前实现：worker 线程异常吞掉，中文留在原位。未来加 toast 错误提示
- **首次 schema build ~30s** — librime 编译 luna_pinyin.table.bin (13MB)，仅首次
- **WeChat ghost cursor 修了，但仅限 PC 端** — 微信小程序内部 webview 输入仍走 Chrome IME 路径

---

## 版本历史

### v0.3（当前）
- 30 语言托盘热切（English / 日本語 / 한국어 / 粵語 + 26 种）
- 神仙鱼品牌图标全套（托盘 + TSF 语言栏 + 系统区）
- 托盘菜单 4 项极简：切换语言 / 检查更新 / 重启 / 退出
- 5 条 rule 专业翻译 prompt（避免直译/保留语气/技术术语/俚语自然化/语境推断）
- 关闭候选窗 status icon
- 检查更新跳转 GitHub Releases 页面
- 网络错误时保留中文 + LOG(ERROR)，不静默吞错
- install 脚本：MoveFileEx pending-on-reboot 兜底 + 轮询 schema 编译完成（非固定 60s 等待）
- system32\weasel.dll 同步替换（TSF 语言栏图标也变神仙鱼）

---

## 贡献

重点方向：
- **macOS port**：用 squirrel 框架，复用我们的 typeanything plugin（C++ 跨平台）
- **Linux port**：用 ibus-rime 或 fcitx-rime
- **流式翻译**：DeepSeek streaming API → 边翻边显示，p50 从 1s 降到 200ms
- **错误 toast**：API 失败时托盘气泡提示
- **离线 fallback**：本地小 model（Qwen 1.5B）做兜底翻译

PR 前请阅 `third_party/weasel/librime/plugins/typeanything/src/typeanything_processor.cc` 了解核心翻译/替换链路。

---

## 致谢

- [rime/weasel](https://github.com/rime/weasel) — Windows TSF 框架，我们 fork 自此
- [rime/librime](https://github.com/rime/librime) — Rime 输入法引擎核心
- [rime/plum](https://github.com/rime/plum) — schema 包管理参考
- [DeepSeek](https://platform.deepseek.com/) — LLM provider，性价比 + 中文质量
- 神仙鱼 logo 来自 [hrdai.com](https://hrdai.com) 品牌系统

---

## License

MIT — 详见 [LICENSE](LICENSE)。

Weasel 上游代码（`third_party/weasel/`）保持其原 license（GPL v3 / BSD-3-Clause 见各文件头）；本仓库新增代码（`librime/plugins/typeanything/`、`install-*.ps1`、修改的 .cpp 文件 diff）以 MIT 发布。
