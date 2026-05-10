"""
Validation 2 (per TypeEverything.md "The First Real Action"):
Test Pinyin2Hanzi output quality on 5 common phrases.

Pass criterion: at least 4/5 outputs are recognizable Chinese.
If <4/5, fall back to "pinyin literal -> LLM" path (skip Pinyin2Hanzi).
"""

import sys
from Pinyin2Hanzi import DefaultHmmParams, viterbi

hmm = DefaultHmmParams()

cases = [
    (("ni", "hao"), "你好"),
    (("wo", "jiao", "xiao", "ming"), "我叫小明"),
    (("ming", "tian", "kai", "hui"), "明天开会"),
    (("wo", "de", "lao", "ban", "dui"), "我的老板对/队"),
    (("bang", "mai", "kang", "di"), "帮买康迪"),
]

print(f"Python {sys.version_info.major}.{sys.version_info.minor} - Pinyin2Hanzi viterbi test\n")
print(f"{'Pinyin':<35} {'Output':<15} {'Expected':<20}")
print("-" * 70)

for syllables, expected in cases:
    paths = viterbi(hmm_params=hmm, observations=syllables, path_num=1)
    output = "".join(paths[0].path) if paths else "<empty>"
    pinyin_str = " ".join(syllables)
    print(f"{pinyin_str:<35} {output:<15} {expected:<20}")

print("\nMulti-candidate (top 3) for ambiguous cases:")
for syllables in [("ni", "hao"), ("wo", "jiao", "xiao", "ming")]:
    paths = viterbi(hmm_params=hmm, observations=syllables, path_num=3)
    candidates = [("".join(p.path), round(p.score, 3)) for p in paths]
    print(f"  {' '.join(syllables):<25} -> {candidates}")
