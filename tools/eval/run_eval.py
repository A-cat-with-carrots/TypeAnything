"""TypeAnything chip eval v2 — 40 chip × 4 sentences through one of four
hardcoded prompts (A/B/C/D) per chip's pre-assigned category. Mirrors the
production architecture: classify at chip-save time (here just a static
table since the 40 chips are internal), then translate with the
category-specific prompt.

Reads API key from D:/hrdai/aiForType/config.json (gitignored).
"""
from __future__ import annotations

import json
import sys
import time
import urllib.request
import urllib.error
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

# Windows console is cp936 (GBK) — printing Korean / Devanagari / 粵語 etc.
# crashes the entire eval. Reconfigure stdout to UTF-8 with safe fallback.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = Path(__file__).resolve().parent.parent.parent  # aiForType/
CONFIG_PATH = ROOT / "config.json"
OUT_PATH = Path(__file__).resolve().parent / "results.jsonl"

# ─── Four category-specific translate prompts (mirror processor.cc) ────────

PROMPT_A = (
    "Translate the user's Chinese into fluent, idiomatic {LANG}.\n"
    "\n"
    "For any natural language (English / 日本語 / 한국어 / Français / Deutsch / "
    "Español / 粵語 / 俄语 etc.):\n"
    "1. Default register: casual / neutral (friend-to-friend), unless the "
    "source is clearly formal/business/emotional — then mirror that.\n"
    "2. For T-V distinction languages (日/韩/法/德/西), default to informal "
    "(タメ口/반말/tu/du/tú) when register defaults to casual.\n"
    "3. Match the source register CONSISTENTLY — never mix 敬语/반말 / "
    "tú/usted / formal/casual within one output.\n"
    "4. Adapt idioms + cultural references — never word-by-word translate. "
    "Network slang (emo / 躺平 / 开摆 / 绝绝子 / yyds / 破防): use "
    "target-language equivalent if it exists; otherwise paraphrase the "
    "emotional spirit.\n"
    "5. Keep proper nouns / English terms / numbers / URLs unchanged.\n"
    "6. For constructed languages (Klingon / Esperanto / Sindarin etc.): "
    "use documented vocab + grammar; prefer well-known canonical phrases. "
    "HARD LIMIT — output ≤ 15 words. For anything beyond that vocabulary "
    "supports, switch to a single-line romanized-English paraphrase rather "
    "than inventing fictional grammar.\n"
    "7. For 粵語 / Cantonese: output in 繁體中文 + 粤字 (嘅/咗/喺/咁/啦/啦/喎/嗎). "
    "Do not use simplified Chinese.\n"
    "\n"
    "Output ONLY the translation. No quotes, no markdown, no notes, no prefix."
)

PROMPT_B = (
    "Rewrite the user's Chinese in the speaking/writing style of \"{LANG}\".\n"
    "\n"
    "\"{LANG}\" describes a Chinese sociolect / profession / persona / era / "
    "genre / writing style.\n"
    "\n"
    "0. PRESERVE SEMANTIC SHAPE (highest priority — applies to ALL chips):\n"
    "   - If input is a question, output a question.\n"
    "   - If input is a statement, output a statement.\n"
    "   - If input is a request / command / greeting, output the same type.\n"
    "   - NEVER expand a short prompt into a long article body, Q&A reply, "
    "or essay. The chip is a VOICE for re-clothing the input — not a topic "
    "to generate content about.\n"
    "   - Examples:\n"
    "     * 知乎大佬腔 + 「为什么 X」 → output a question in 知乎大佬 voice "
    "(想问下各位大佬 / 从沉没成本角度看…), NOT a 500-word essay answer.\n"
    "     * 营销号体 + 「我饿了」 → \"震惊！我居然饿了！家人们谁懂啊！\" "
    "(short clickbait sentence), NOT a full fake article.\n"
    "     * 小红书 + 「这款口红太美了，秋冬必入」 → one sentence with "
    "「姐妹们 / 绝绝子 / 必入」 sprinkled in, NOT a 500-word seeding post.\n"
    "\n"
    "1. OUTPUT MUST STAY IN CHINESE CHARACTERS. Limited code-switching with "
    "single English/foreign words is OK only when the style canonically "
    "does so (留学生式 / 互联网黑话 / HR 式) — typically ≤20% of tokens. "
    "NEVER produce a full sentence in English / 日本語 / non-Chinese.\n"
    "\n"
    "2. Inject ≥2 signature lexical / syntactic markers of \"{LANG}\":\n"
    "   - 金融式: 估值 / runway / Q3 / deal / term sheet / LP-GP / IRR\n"
    "   - 留学生式: paper / due / TA / lab / project / emo / midterm / "
    "literally — 中英混搭 ≥1 英文词在情感句也要有\n"
    "   - 互联网黑话: 闭环 / 抓手 / 对齐 / 赋能 / 颗粒度 / 心智\n"
    "   - 学术大佬式: 本人认为 / 研究表明 / 范式 / 边际 / 可证伪 / 方法论\n"
    "   - HR 式: 同学 / 对齐 / align / 向上反馈 / 我理解你 / OD / owner\n"
    "   - 销售式: 哥 / 姐 / 老板 / 给您安排到位 / 量大从优 / 您看价格\n"
    "   - 二次元: awsl / 本命 / 草 / 不愧是你 / 颜文字 / desu / 嘤嘤嘤\n"
    "   - 东北话: 整 / 咋 / 嗷 / 搁 / 咋整 / 咋滴 / 不得劲儿 / 嗷嗷的\n"
    "   - 台湾腔: 啦 / 耶 / 喔 / 真的假的 / 超 + 形容词 / 「啦」「耶」用繁体字\n"
    "   - 老北京话: 您 / 儿化 / 麻利儿 / 搓一顿 / 章程 / 擎好儿 / 不落忍\n"
    "   - 古汉语风格: 矣 / 乎 / 焉 / 之 / 散直 / 共膳 / 时辰 / 呜呼\n"
    "   - 知乎大佬腔: 谢邀 / 作为一名 / 本质上 / 沉没成本 / 边际效用 / "
    "范式 / 心智模型 / IRR — 仅借用知乎大佬词汇，不要写 Q&A essay。\n"
    "   - 鲁迅式中文: 罢 / 便 / 倒也 / 实则 / 未必 / 「大抵是…了」 / "
    "「横竖都…」 / 否定收尾 / 反讽 + 反问\n"
    "   - 小红书种草: 姐妹们 / 集美们 / 绝绝子 / 必入 / yyds / 黄皮也能 hold / emoji\n"
    "   - 营销号体: 震惊! / 家人们谁懂啊 / 90% 的人都不知道 / 看完转疯了\n"
    "   - 武侠小说体: 阁下 / 在下 / 江湖 / 咽不下这口气 / 内力 / 恩怨\n"
    "   - 玄幻爽文体: 本座 / 紫府 / 凝丹 / 此仇不报誓不为人 / 尔等凡人 / 天材地宝\n"
    "   - 客服式: 亲 / 呢 / 您看下 / 理解您的心情 / 帮您 / 哦 / 哈\n"
    "   - 法律文书: 系 / 兹 / 特此 / 不予 / 依法依规 / 相对方 / 保留权利\n"
    "   - 公文体: 现就 / 请遵照执行 / 特此通知 / 责成 / 严正关切\n"
    "   - 渣男话术: 在吗 / 宝 / 你好像我前任 / 我都是为你好 / 你想想 / 下一波. "
    "FORBIDDEN in 渣男话术: Q3 / term sheet / runway / IRR / deal / 估值 "
    "(金融词不属于渣男人设)\n"
    "   - 舔狗式: 宝 / 你说的都对 / 我可以的 / 哪怕一点点 / 求你 / 只要你不嫌弃\n"
    "   - AI 助手腔: 您好 / 我注意到 / 建议您 / 请问您还需要 / 我可以为您…\n"
    "   - 玛丽苏文: 他眸光一沉 / 薄唇紧抿 / 心头一颤 / 错付了 / 万千星辰. "
    "FORBIDDEN in 玛丽苏文: 本座 / 紫府 / 凝丹 (玄幻 only) / 阁下 / 在下 / 区区 "
    "(古汉语/古风小生 only) / 吾共膳 / 用饭 (古汉语)\n"
    "   - 古风小生: 公子 / 在下 / 区区不才 / 告退 / 鄙人 / 斗胆\n"
    "\n"
    "3. AMPLIFY — even when input already matches the style, rewrite "
    "noticeably. NEVER echo verbatim. Short inputs (1-5 chars) must still "
    "inject ≥1 style marker.\n"
    "\n"
    "4. Length constraint:\n"
    "   - Default: output ≤ 3× input character count.\n"
    "   - 知乎大佬腔 special: ≤ max(5× input, 400 chars). Long-form by "
    "nature but must stop, not ramble into 600+ chars.\n"
    "   - 营销号体 / 小红书种草 / 玛丽苏文: ≤ 4× input (slightly looser for "
    "stylized prose) but never exceed 200 chars on short inputs.\n"
    "   - 玄幻爽文体 / 武侠小说体 / 古风小生 / 古汉语风格: ≤ 3× input. These "
    "are NOT essay chips — a one-line statement input must yield a one-line "
    "stylized statement, NOT a 「方案如下：第一/第二/第三」 numbered lecture.\n"
    "   - NO chip may turn a statement into a bullet/numbered list unless the "
    "input itself was a list.\n"
    "\n"
    "5. Same-family disambiguation. Avoid bleeding markers across:\n"
    "   - 古汉语 / 武侠 / 古风小生 / 玄幻 / 玛丽苏 — each must use its own "
    "marker set. Specifically: \n"
    "     * 玛丽苏文 must NOT use 本座 / 紫府 (those are 玄幻 only).\n"
    "     * 武侠 must NOT use 紫府 / 凝丹.\n"
    "     * 古汉语 must NOT use 公子 / 在下 (古风小生 territory).\n"
    "   - 法律文书 / 公文体 / AI 助手腔 / 客服式 — separated by markers above.\n"
    "\n"
    "6. CROSS-CHIP VOCABULARY LOCK (global, applies to EVERY chip):\n"
    "   6a. FINANCE terms {term sheet / runway / IRR / 估值 / Q3 / deal / "
    "LP / GP / 对赌} are ALLOWED ONLY in 金融式说话. FORBIDDEN in every "
    "other chip — including 互联网黑话, HR 式说话, 留学生式说话. "
    "(\"接受不了\" must NOT become \"接受不了这个 deal 的 term sheet\".)\n"
    "   6b. INTERNET-JARGON {颗粒度 / 对齐 / 闭环 / 抓手 / 赋能 / 心智 / "
    "降本增效} are ALLOWED ONLY in 互联网黑话 (and 对齐/向上反馈 in HR 式). "
    "FORBIDDEN in: 玄幻爽文体 / 二次元 / 销售式 / AI 助手腔 / 客服式 / "
    "法律文书 / 公文体 / 玛丽苏文 / 武侠小说体 / 古汉语 / 古风小生 / "
    "鲁迅式中文 / 营销号体 / 留学生式说话.\n"
    "   6c. When the input is an emotional statement (e.g. 「接受不了」), "
    "do NOT smuggle in business/finance metaphors unless the chip is "
    "金融式说话 or 互联网黑话.\n"
    "\n"
    "7. 营销号体 special: allowed to add 震惊!! / 家人们谁懂啊 / clickbait "
    "openings, BUT the event subject (who did what to whom) MUST remain "
    "identifiable. Do not invent concepts not in the input "
    "(no 灵魂拷问 / 满血复活 unless the input mentioned them).\n"
    "\n"
    "8. 留学生式 special: even in emotional (S3-like) inputs you MUST inject "
    "≥1 English word (emo / literally / can't / 心碎 desu — pick natural).\n"
    "\n"
    "9. Preserve original meaning + emotion. Keep proper nouns / numbers / "
    "URLs unchanged.\n"
    "\n"
    "Output ONLY the rewritten Chinese. No labels, no quotes, no prefix."
)

PROMPT_C = (
    "Translate the user's Chinese into the foreign language implied by "
    "\"{LANG}\", then apply the named voice/style.\n"
    "\n"
    "HARD LANGUAGE LOCK (overrides everything below):\n"
    "  - If chip name ends with «英语 / English» → output MUST be English. "
    "Never Chinese / Cantonese / Japanese.\n"
    "  - If chip name ends with «日语 / Japanese» → output MUST be Japanese.\n"
    "  - If chip name names «粵語 / Cantonese» (e.g. 港片黑帮台词) → "
    "output MUST be 繁體 Cantonese.\n"
    "  - The chip's NAMED LANGUAGE always wins over input emotional cues. "
    "If input is emotional (e.g. 「他怎么能这么对我」), still output in the "
    "chip's locked language — never switch to Chinese / Cantonese just because "
    "the emotion feels 港片-like.\n"
    "\n"
    "Language inference from chip name:\n"
    "- Explicit \"X-style / X-slang / X-prose / X-体 / X-式 + Y-language\" "
    "→ target = Y.\n"
    "  - 学术英语 → English (NEVER Cantonese, NEVER Chinese)\n"
    "  - 商务日语 → Japanese\n"
    "  - Cockney slang → English (East London dialect)\n"
    "  - Indian English → English (Indian variety)\n"
    "- Cultural reference → infer language:\n"
    "  - 港片黑帮台词 → 粤語 (Cantonese, 繁體 + 粤字)\n"
    "  - 海盗腔 → English (pirate)\n"
    "  - 忍者口吻 → Japanese\n"
    "  - 西部牛仔 → American English\n"
    "  - Klingon battle prose → Klingon\n"
    "\n"
    "Apply ≥2 style markers distinctive of the named voice:\n"
    "  - Cockney slang: h-dropping ('ouse, 'e) / rhyming slang "
    "(china plate=mate, dog and bone=phone, pony=£25) / \"me\" for \"my\"\n"
    "  - 港片黑帮台词: 粤字 (嘅/咗/喺/咁/呢) + 黑帮词 "
    "(兄弟/收数/老细/搞掂/大佬/扑街/使乜咁多废话/睇路) + "
    "短促命令句 + occasional code-switch (OK / sorry / boss)\n"
    "  - 学术英语: passive voice / hedging (suggests, may indicate) / "
    "nominalization / 「The author / It is suggested」\n"
    "  - 商务日语: 敬语 (です/ます/いただく) + 「申し上げます」「お差し支えなければ」 "
    "/ closing 「よろしくお願い申し上げます」\n"
    "  - Indian English: ROTATE markers across sentences — do NOT pile every "
    "marker into every output. Use 1-2 markers per sentence:\n"
    "    Sentence 1: «do the needful» or «kindly»\n"
    "    Sentence 2: «only» intensifier or «no?» tag\n"
    "    Sentence 3: «present continuous overuse» (I am understanding / "
    "she is having)\n"
    "    Sentence 4: «prepone» or «itself» emphatic\n"
    "    Never use «no?» on every sentence. Never stack «do the needful» + "
    "«only» + «no?» in the same line.\n"
    "  - Klingon battle prose: short imperative sentences (jagh ghorgh! "
    "yIghoS!). HARD LIMIT — output ≤ 15 words.\n"
    "    * MANDATORY: every output MUST end with a one-line English gloss "
    "in [brackets]. Example format: `jIghung. yIghoS! [I hunger. Move out!]`\n"
    "    * MANDATORY: the Klingon portion MUST differ between sentences. "
    "Never reuse the exact same Klingon string twice. Vary vocabulary "
    "(jIghung / qoH / mughom / Hegh / qaq / SuvwI' / yIyepHa') based on "
    "the input's actual meaning.\n"
    "    * If vocab fails, prefer romanized transliteration over inventing.\n"
    "\n"
    "For resource-scarce languages: use documented vocab; for missing words, "
    "romanize-transliterate; preserve the language's spelling characteristics. "
    "Never invent fictional grammar. Never produce loops (e.g. repeating "
    "「qaStaHvIS jagh, qaStaHvIS jagh」 — emit each lexeme at most once).\n"
    "\n"
    "Preserve meaning. Keep register consistent. Keep proper nouns / "
    "numbers / URLs unchanged.\n"
    "\n"
    "Output ONLY the styled translation. No labels, no quotes, no prefix."
)

PROMPT_D = (
    "Encode the user's Chinese characters per the cipher / encoding scheme "
    "named in \"{LANG}\".\n"
    "\n"
    "Known schemes:\n"
    "  - 火星文: substitute ≥60% of Hanzi with phonetic / visual variants "
    "(ωǒ俄勒 / 莪 / 啲 / ㄋ / の / 様 / 卟 / 茣 / 妁 / 哋). 2008-era QQ-space "
    "style: 拉丁字母 / 注音符号 / 日文假名 / 异体字 / 拆字 mix.\n"
    "    * Self-check before output: count Hanzi in input, count substituted "
    "in output. Density must be ≥ 60%. If not, rewrite with more substitutions.\n"
    "    * Never duplicate the same character / prefix more than once. Output "
    "must have the same character count as input (1:1 mapping, no extra "
    "leading tokens like \"ωǒ\" prepended to the sentence).\n"
    "  - 拼音体: replace each Hanzi with its pinyin, NO tone marks, NO "
    "hyphens. Insert a single space between EACH syllable (one space per "
    "Hanzi, e.g. 我们 → wo men, not women). Keep English / numbers / "
    "punctuation as-is.\n"
    "  - leet speak: a→4 / e→3 / i→1 / o→0 / s→5 / t→7 / l→1.\n"
    "  - Pig Latin: move first consonant cluster + \"ay\" to word end.\n"
    "  - rot13: shift Latin letters by 13.\n"
    "\n"
    "For an unfamiliar cipher named in \"{LANG}\":\n"
    "  - Apply best-effort character substitution following the scheme's "
    "spirit. Aim ≥60% character transformation density.\n"
    "\n"
    "1. Preserve word boundaries + punctuation.\n"
    "2. Preserve semantic structure — encoding ONLY, NO rewording.\n"
    "3. Keep English / numbers / URLs untransformed (unless the cipher "
    "applies to ASCII like leet/rot13).\n"
    "\n"
    "Output ONLY the encoded text. No labels, no quotes, no prefix."
)

PROMPTS = {"A": PROMPT_A, "B": PROMPT_B, "C": PROMPT_C, "D": PROMPT_D}

# ─── 3 shared baseline sentences + 1 chip-tailored signature ──────────────

S_COMMON = {
    "S1_daily":   "我饿了，下班一起吃饭吗",
    "S2_biz":     "这个项目下周必须交付，给我个清晰的方案",
    "S3_emo":     "他怎么能这么对我，我真的接受不了",
}

# 40 chips: (category, chip_name, S4_signature_sentence)
CHIPS = [
    # ── A: Natural language (7) ─────────────────────────────────
    ("A", "English",     "我今天 emo 了，开摆"),
    ("A", "日本語",       "带老板出去应酬，他喝多了开始耍酒疯"),
    ("A", "한국어",       "最近压力好大，想躺平"),
    ("A", "Français",    "这家店性价比很高，强烈推荐"),
    ("A", "Deutsch",     "这个方案不太靠谱，需要重新评估"),
    ("A", "Español",     "他人很热情，话很多，但说重点不行"),
    ("A", "粵語",        "老板今晚请客，去 KTV 喝酒"),

    # ── B: Chinese style (25) ───────────────────────────────────
    ("B", "金融式说话",     "我们看这个项目，估值给到一个亿没问题"),
    ("B", "留学生式说话",   "明天 due 一个 paper 我还没开始写"),
    ("B", "互联网黑话",     "这个产品没找到 PMF，需要重新定位"),
    ("B", "学术大佬式",     "这篇文章数据有问题，结论站不住脚"),
    ("B", "HR 式说话",      "这季度绩效评估要客观，避免主观判断"),
    ("B", "销售式说话",     "这个客户跟了三个月还没下单，需要再 push 一下"),
    ("B", "二次元",         "这个动漫我已经追了一年了"),
    ("B", "东北话",         "哎呀妈呀这事儿整的，我都不知道咋说"),
    ("B", "台湾腔",         "这家店真的超好吃，你一定要试试"),
    ("B", "老北京话",       "您给我来碗炸酱面，多放醋"),
    ("B", "古汉语风格",     "我每天工作十二个小时，太累了"),
    ("B", "知乎大佬腔",      "为什么大厂程序员都想去创业"),
    ("B", "鲁迅式中文",     "这社会人情冷暖，看得人心寒"),
    ("B", "小红书种草体",   "这款口红颜色太美了，秋冬必入"),
    ("B", "营销号体",       "震惊！90% 的人都不知道这个秘密"),
    ("B", "武侠小说体",     "他一掌劈出，敌人应声倒地"),
    ("B", "玄幻爽文体",     "我已经突破到金丹期了"),
    ("B", "客服式",         "您这个问题我已经记录了，稍后给您反馈"),
    ("B", "法律文书",       "经查，李某违反公司规定，应予处罚"),
    ("B", "公文体",         "请各部门积极配合此项工作，务必按时完成"),
    ("B", "渣男话术",       "宝你又生气啦，我都是为你好"),
    ("B", "舔狗式",         "你说什么我都听你的，求你别走"),
    ("B", "AI 助手腔",      "请问您还需要其他帮助吗"),
    ("B", "玛丽苏文",       "他凝视着我，目光深邃如夜"),
    ("B", "古风小生",       "在下不才，斗胆问一句芳名"),

    # ── C: Cross style (6) ──────────────────────────────────────
    ("C", "学术英语",            "最新研究表明，AI 对就业的影响被高估了"),
    ("C", "商务日语",            "请贵公司在本月内回复合同条款"),
    ("C", "港片黑帮台词",        "兄弟，今晚我们去码头收数"),
    ("C", "Klingon battle prose","敌人来了，准备战斗！"),
    ("C", "Cockney slang",       "老兄借我五十块急用"),
    ("C", "Indian English",      "请尽快回复我的邮件，事情很急"),

    # ── D: Cipher (2) ───────────────────────────────────────────
    ("D", "火星文",     "今天心情很好"),
    ("D", "拼音体",     "我们明天再聊"),
]


def call_llm(api_key: str, endpoint: str, model: str, temperature: float,
             category: str, chip_text: str, user_text: str,
             timeout: float = 30.0) -> dict:
    system_prompt = PROMPTS[category].replace("{LANG}", chip_text)
    payload = {
        "model": model,
        "temperature": temperature,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user",   "content": user_text},
        ],
    }
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        endpoint,
        data=data,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
        },
        method="POST",
    )
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8")
        latency_ms = int((time.time() - t0) * 1000)
        obj = json.loads(body)
        out = obj["choices"][0]["message"]["content"].strip()
        return {"ok": True, "out": out, "latency_ms": latency_ms}
    except urllib.error.HTTPError as e:
        return {"ok": False,
                "err": f"HTTP {e.code}: {e.read().decode('utf-8', 'replace')[:200]}",
                "latency_ms": int((time.time() - t0) * 1000)}
    except Exception as e:
        return {"ok": False, "err": str(e),
                "latency_ms": int((time.time() - t0) * 1000)}


def main():
    cfg = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    api_key  = cfg["api_key"]
    endpoint = cfg["endpoint"]
    model    = cfg.get("model", "deepseek-chat")
    temp     = float(cfg.get("temperature", 0.3))

    tasks = []
    for category, chip, s4_text in CHIPS:
        for sid, stext in S_COMMON.items():
            tasks.append((category, chip, sid, stext))
        tasks.append((category, chip, "S4_signature", s4_text))

    print(f"Total tasks: {len(tasks)} (= {len(CHIPS)} chips x 4 sentences)")
    print(f"Endpoint: {endpoint}")
    print(f"Model:    {model}")
    print(f"Temp:     {temp}")
    print()

    results = []
    done = 0
    t_start = time.time()

    def run_one(t):
        category, chip, sid, stext = t
        r = call_llm(api_key, endpoint, model, temp, category, chip, stext)
        return {
            "category": category,
            "chip": chip,
            "sentence_id": sid,
            "input": stext,
            "output": r.get("out", ""),
            "ok": r["ok"],
            "err": r.get("err", ""),
            "latency_ms": r["latency_ms"],
        }

    with ThreadPoolExecutor(max_workers=8) as pool:
        futures = {pool.submit(run_one, t): t for t in tasks}
        for fut in as_completed(futures):
            row = fut.result()
            results.append(row)
            done += 1
            mark = "OK" if row["ok"] else "ERR"
            print(f"[{done:3d}/{len(tasks)}] {mark} {row['category']} | "
                  f"{row['chip']:24s} | {row['sentence_id']:14s} | "
                  f"{row['latency_ms']:5d}ms")
            if not row["ok"]:
                print(f"    err: {row['err']}")

    elapsed = time.time() - t_start
    ok_count = sum(1 for r in results if r["ok"])
    print()
    print(f"Done in {elapsed:.1f}s. OK={ok_count}/{len(results)}")

    # Sort by (category, chip, sentence_id) for stable output.
    cat_order = {"A": 0, "B": 1, "C": 2, "D": 3}
    sid_order = {"S1_daily": 0, "S2_biz": 1, "S3_emo": 2, "S4_signature": 3}
    results.sort(key=lambda r: (cat_order.get(r["category"], 9),
                                 r["chip"],
                                 sid_order.get(r["sentence_id"], 9)))

    with OUT_PATH.open("w", encoding="utf-8") as f:
        for r in results:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    print(f"Wrote {OUT_PATH}")


if __name__ == "__main__":
    main()
