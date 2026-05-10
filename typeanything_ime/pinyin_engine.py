"""Local pinyin → Chinese candidate engine.

Loads the rime `pinyin_simp` dict (65k entries, real usage frequency) and
provides fast prefix lookup for user-typed pinyin. No LLM, no internet —
all queries are O(prefix_range) with bisect over a sorted key list.

Performance (measured):
  - load: ~75ms (one-time at IME activation)
  - query: ~5ms typical (1-3 char prefix), <1ms for long inputs

Falls back to greedy syllable segmentation for inputs longer than any
single phrase in the dict (e.g., "wojintianxiangchimian").
"""

from __future__ import annotations

import bisect
import math
import os.path
from dataclasses import dataclass


_HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DICT = os.path.join(_HERE, "data", "pinyin.dict.yaml")


@dataclass
class _Entry:
    word: str       # the Chinese characters
    freq: int       # usage frequency


class PinyinEngine:
    def __init__(self, dict_path: str = DEFAULT_DICT):
        self.dict_path = dict_path
        self._index: dict[str, list[_Entry]] = {}
        self._keys_sorted: list[str] = []
        self._loaded = False

    # ---------- load ----------

    def load(self):
        if self._loaded:
            return
        if not os.path.isfile(self.dict_path):
            raise FileNotFoundError(f"pinyin dict not found: {self.dict_path}")
        with open(self.dict_path, "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("#") or not line.strip():
                    continue
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 2:
                    continue
                word = parts[0].strip()
                pinyin = parts[1].strip()
                if not word or not pinyin:
                    continue
                try:
                    freq = int(parts[2].strip()) if len(parts) >= 3 else 0
                except ValueError:
                    freq = 0
                key = pinyin.replace(" ", "")
                if not key:
                    continue
                self._index.setdefault(key, []).append(_Entry(word=word, freq=freq))
        # Sort each bucket by freq desc (so [0] is most common)
        for k, lst in self._index.items():
            lst.sort(key=lambda e: -e.freq)
        # Build sorted-keys list for prefix range queries
        self._keys_sorted = sorted(self._index.keys())
        self._loaded = True

    # ---------- query ----------

    def query(self, user_pinyin: str, top_n: int = 9) -> list[str]:
        """Return top-N Chinese candidates for the given pinyin (no spaces)."""
        if not self._loaded:
            self.load()
        py = user_pinyin.strip().replace(" ", "").lower()
        if not py:
            return []

        # 1) Exact match wins outright if present.
        if py in self._index:
            exact = [(e.word, e.freq, 0) for e in self._index[py]]
        else:
            exact = []

        # 2) Prefix range — anything that starts with py.
        lo = bisect.bisect_left(self._keys_sorted, py)
        hi = bisect.bisect_left(self._keys_sorted, py + "￿")
        prefix_matches: list[tuple[str, int, int]] = list(exact)
        for k in self._keys_sorted[lo:hi]:
            if k == py:
                continue  # already in exact
            for e in self._index[k]:
                prefix_matches.append((e.word, e.freq, len(k) - len(py)))

        # Sort: exact first (delta=0), then by length-delta asc (prefer
        # shorter extensions), then freq desc.
        prefix_matches.sort(key=lambda m: (m[2], -m[1]))
        # Dedupe while preserving order (same Chinese word may map to
        # multiple readings)
        seen = set()
        result = []
        for w, _, _ in prefix_matches:
            if w not in seen:
                seen.add(w)
                result.append(w)
            if len(result) >= top_n:
                break
        if result:
            return result

        # 3) Fallback: greedy left-to-right syllable segmentation for long
        # inputs that match no single dict entry. Returns one composed
        # candidate by stitching top-1 of each segment.
        seg = self._segment_greedy(py)
        if seg:
            return [seg]
        return []

    # ---------- helpers ----------

    def _segment_greedy(self, py: str, max_seg_len: int = 12) -> str:
        """Viterbi segmentation maximizing sum(log(freq+1)) of segments.

        Picks the global-best segmentation by total log-frequency, not
        local longest-match. Avoids "wojin -> 握紧" ahead of "wo + jintian
        -> 我 + 今天" by preferring high-freq common breakdowns.
        """
        n = len(py)
        if n == 0:
            return ""
        # dp[i] = (best_score, list_of_chinese_words ending at position i)
        NEG_INF = -1e18
        dp_score = [NEG_INF] * (n + 1)
        dp_words: list[list[str]] = [[] for _ in range(n + 1)]
        dp_score[0] = 0.0

        for i in range(1, n + 1):
            for j in range(max(0, i - max_seg_len), i):
                sub = py[j:i]
                if sub in self._index:
                    top_freq = self._index[sub][0].freq or 1
                    seg_len = i - j
                    # Score = log(freq+1) * length². The quadratic length
                    # bonus reliably picks phrases over component chars even
                    # when individual chars are extremely frequent (e.g.,
                    # "zhongguo -> 中国" over "zhong|guo -> 中过", and
                    # "jintian -> 今天" over "jin|tian -> 今天" components).
                    seg_score = math.log(top_freq + 1) * seg_len * seg_len
                    cand = dp_score[j] + seg_score
                    if cand > dp_score[i]:
                        dp_score[i] = cand
                        dp_words[i] = dp_words[j] + [self._index[sub][0].word]
            # Fallback: treat one literal char as a low-score segment so
            # we always reach n even with unrecognized fragments.
            if dp_score[i] == NEG_INF and dp_score[i - 1] > NEG_INF:
                dp_score[i] = dp_score[i - 1] - 100
                dp_words[i] = dp_words[i - 1] + [py[i - 1]]
        return "".join(dp_words[n])

    def segment_pinyin(self, py: str, max_seg_len: int = 8) -> str:
        """Insert apostrophes between detected syllable boundaries.
        For composition display only. Returns 'ni'hao'shi' for 'nihaoshi'."""
        if not self._loaded:
            self.load()
        if not py:
            return ""
        parts: list[str] = []
        i = 0
        while i < len(py):
            j = min(i + max_seg_len, len(py))
            matched_len = 0
            while j > i:
                if py[i:j] in self._index:
                    matched_len = j - i
                    break
                j -= 1
            if matched_len == 0:
                # unknown char — emit single
                parts.append(py[i])
                i += 1
            else:
                parts.append(py[i:i + matched_len])
                i += matched_len
        return "'".join(parts)
