"""State machine — Plan F5 (per-keystroke local pinyin engine + LLM stage 2).

Stage 1 (per pinyin keystroke): local engine (PinyinEngine) → Chinese
candidates. <2ms. Pure data lookup.

Stage 2 (Enter): LLM call (chinese_buffer → target language). ~500-700ms,
user-initiated.

Engine query is performed by the shell (main.py). The state machine
exposes set_candidates() so the shell can push lookup results back.

States:
  IDLE        — no pinyin; chinese_buffer may be empty or non-empty
  TYPING      — pinyin_buffer non-empty (candidates panel shown)
  TRANSLATED  — stage-2 result shown, awaiting commit
  ERROR       — translation failed

Composition text:
  IDLE        -> chinese_buffer
  TYPING      -> chinese_buffer + segmented_pinyin (e.g., "你好ni'hao")
  TRANSLATED  -> translation
  ERROR       -> chinese_buffer + " !"
"""

from dataclasses import dataclass, field
from enum import Enum, auto


class State(Enum):
    IDLE = auto()
    TYPING = auto()
    TRANSLATED = auto()
    ERROR = auto()


class Action(Enum):
    NONE = auto()
    SET_COMPOSITION = auto()
    SET_CANDIDATES = auto()
    CLEAR_CANDIDATES = auto()
    COMMIT = auto()
    QUERY_PINYIN = auto()       # shell: query engine with current pinyin_buffer
    TRANSLATE_FULL = auto()     # shell: call LLM stage 2 on chinese_buffer


@dataclass
class Outcome:
    actions: list[Action] = field(default_factory=list)
    composition: str = ""
    composition_hint: str = ""  # optional pinyin segmentation hint
    commit: str = ""
    candidates: list[str] = field(default_factory=list)
    consumed: bool = True

    def add(self, action: Action):
        self.actions.append(action)


@dataclass
class StateMachine:
    state: State = State.IDLE
    pinyin_buffer: str = ""
    chinese_buffer: str = ""
    candidates: list[str] = field(default_factory=list)
    translation: str = ""
    max_buffer: int = 40

    def reset(self):
        self.state = State.IDLE
        self.pinyin_buffer = ""
        self.chinese_buffer = ""
        self.candidates = []
        self.translation = ""

    # ---------- helpers ----------

    def _has_chinese(self) -> bool:
        return bool(self.chinese_buffer)

    def _commit_first_candidate(self):
        if self.candidates:
            self.chinese_buffer += self.candidates[0]
        self.candidates = []
        self.pinyin_buffer = ""

    def _clear_candidates_in(self, out: Outcome):
        if self.candidates:
            self.candidates = []
            out.add(Action.CLEAR_CANDIDATES)

    # ---------- key entrypoints ----------

    def on_pinyin_char(self, ch: str) -> Outcome:
        out = Outcome()
        if self.state == State.IDLE:
            self.state = State.TYPING
            self.pinyin_buffer = ch
        elif self.state == State.TYPING:
            self.pinyin_buffer += ch
        elif self.state == State.TRANSLATED:
            out.add(Action.COMMIT)
            out.commit = self.translation
            self.reset()
            self.state = State.TYPING
            self.pinyin_buffer = ch
        elif self.state == State.ERROR:
            out.add(Action.COMMIT)
            out.commit = self.chinese_buffer
            self.reset()
            self.state = State.TYPING
            self.pinyin_buffer = ch

        out.composition = self.composition_text()
        out.add(Action.SET_COMPOSITION)
        out.add(Action.QUERY_PINYIN)  # shell: refresh candidates
        return out

    def set_candidates(self, candidates: list[str]) -> Outcome:
        """Shell pushes engine's lookup result back into the SM."""
        out = Outcome()
        if self.state != State.TYPING:
            return out
        self.candidates = candidates[:9]
        out.candidates = list(self.candidates)
        out.composition = self.composition_text()
        out.add(Action.SET_COMPOSITION)
        if self.candidates:
            out.add(Action.SET_CANDIDATES)
        else:
            out.add(Action.CLEAR_CANDIDATES)
        return out

    def on_digit(self, n: int) -> Outcome:
        out = Outcome()
        if self.state == State.TYPING and 1 <= n <= len(self.candidates):
            self.chinese_buffer += self.candidates[n - 1]
            self.candidates = []
            self.pinyin_buffer = ""
            self.state = State.IDLE
            out.add(Action.CLEAR_CANDIDATES)
            out.composition = self.composition_text()
            out.add(Action.SET_COMPOSITION)
            return out
        return self.on_other_char(str(n))

    def on_space(self) -> Outcome:
        out = Outcome()
        if self.state == State.IDLE:
            out.consumed = False
        elif self.state == State.TYPING:
            if self.candidates:
                self._commit_first_candidate()
                self.state = State.IDLE
                out.add(Action.CLEAR_CANDIDATES)
            else:
                # no candidates — commit the raw pinyin
                self.chinese_buffer += self.pinyin_buffer
                self.pinyin_buffer = ""
                self.state = State.IDLE
        elif self.state == State.TRANSLATED:
            out.add(Action.COMMIT)
            out.commit = self.translation
            self.reset()
        elif self.state == State.ERROR:
            out.add(Action.COMMIT)
            out.commit = self.chinese_buffer
            self.reset()
        out.composition = self.composition_text()
        out.add(Action.SET_COMPOSITION)
        return out

    def on_enter(self) -> Outcome:
        out = Outcome()
        if self.state == State.IDLE:
            if self._has_chinese():
                out.add(Action.TRANSLATE_FULL)
                out.composition = self.composition_text()
                return out
            out.consumed = False
        elif self.state == State.TYPING:
            # pick first candidate (if any), then translate accumulated chinese
            if self.candidates:
                self._commit_first_candidate()
                out.add(Action.CLEAR_CANDIDATES)
            else:
                self.chinese_buffer += self.pinyin_buffer
                self.pinyin_buffer = ""
            self.state = State.IDLE
            if self._has_chinese():
                out.add(Action.TRANSLATE_FULL)
                out.composition = self.composition_text()
                return out
            self.reset()
        elif self.state == State.TRANSLATED:
            out.add(Action.COMMIT)
            out.commit = self.translation
            self.reset()
        elif self.state == State.ERROR:
            out.add(Action.COMMIT)
            out.commit = self.chinese_buffer
            self.reset()
        out.composition = self.composition_text()
        out.add(Action.SET_COMPOSITION)
        return out

    def on_backspace(self) -> Outcome:
        out = Outcome()
        if self.state == State.IDLE:
            if self._has_chinese():
                self.chinese_buffer = self.chinese_buffer[:-1]
                if not self.chinese_buffer:
                    self.reset()
            else:
                out.consumed = False
        elif self.state == State.TYPING:
            self.pinyin_buffer = self.pinyin_buffer[:-1]
            if not self.pinyin_buffer:
                self.candidates = []
                self.state = State.IDLE
                out.add(Action.CLEAR_CANDIDATES)
                if not self._has_chinese():
                    self.reset()
            else:
                out.add(Action.QUERY_PINYIN)  # refresh after trim
        elif self.state == State.TRANSLATED:
            self.translation = ""
            self.state = State.IDLE
        elif self.state == State.ERROR:
            self.chinese_buffer = self.chinese_buffer[:-1]
            if not self.chinese_buffer:
                self.reset()
            else:
                self.state = State.IDLE
        out.composition = self.composition_text()
        out.add(Action.SET_COMPOSITION)
        return out

    def on_escape(self) -> Outcome:
        out = Outcome()
        if self.state == State.IDLE:
            if self._has_chinese():
                self.reset()
            else:
                out.consumed = False
        elif self.state == State.TYPING:
            self.pinyin_buffer = ""
            self.candidates = []
            out.add(Action.CLEAR_CANDIDATES)
            self.state = State.IDLE
            if not self._has_chinese():
                self.reset()
        elif self.state == State.TRANSLATED:
            self.translation = ""
            self.state = State.IDLE
        elif self.state == State.ERROR:
            self.reset()
        out.composition = self.composition_text()
        out.add(Action.SET_COMPOSITION)
        return out

    def on_full_result(self, original_chinese: str, target_text: str) -> Outcome:
        out = Outcome()
        if self.state != State.IDLE:
            return out
        if original_chinese != self.chinese_buffer:
            return out
        if not target_text:
            return self.on_llm_error(original_chinese)
        self.translation = target_text
        self.state = State.TRANSLATED
        out.composition = self.composition_text()
        out.add(Action.SET_COMPOSITION)
        return out

    def on_llm_error(self, original_chinese: str) -> Outcome:
        out = Outcome()
        if self.state == State.IDLE and original_chinese == self.chinese_buffer:
            self.state = State.ERROR
        out.composition = self.composition_text()
        out.add(Action.SET_COMPOSITION)
        return out

    def on_other_char(self, ch: str) -> Outcome:
        out = Outcome()
        if self.state == State.IDLE:
            if self._has_chinese():
                out.add(Action.COMMIT)
                out.commit = self.chinese_buffer + ch
                self.reset()
            else:
                out.consumed = False
                return out
        elif self.state == State.TYPING:
            # commit first candidate (if any) + chinese_buffer + char
            if self.candidates:
                self._commit_first_candidate()
                out.add(Action.CLEAR_CANDIDATES)
            else:
                self.chinese_buffer += self.pinyin_buffer
                self.pinyin_buffer = ""
            out.add(Action.COMMIT)
            out.commit = self.chinese_buffer + ch
            self.reset()
        elif self.state == State.TRANSLATED:
            out.add(Action.COMMIT)
            out.commit = self.translation + ch
            self.reset()
        elif self.state == State.ERROR:
            out.add(Action.COMMIT)
            out.commit = self.chinese_buffer + ch
            self.reset()
        out.composition = ""
        out.add(Action.SET_COMPOSITION)
        return out

    # ---------- presentation ----------

    def composition_text(self) -> str:
        if self.state == State.IDLE:
            return self.chinese_buffer
        if self.state == State.TYPING:
            return self.chinese_buffer + self.pinyin_buffer
        if self.state == State.TRANSLATED:
            return self.translation
        if self.state == State.ERROR:
            return self.chinese_buffer + " !"
        return ""
