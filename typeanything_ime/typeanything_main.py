"""PIME entrypoint — Plan F5.

Per-keystroke local pinyin engine for instant Chinese candidates (mature-IME
behavior — no Space needed to see candidates). LLM only for stage 2 (Enter
translates accumulated Chinese to the target language).

Modes:
  english_mode (Shift+Space toggle)  — passthrough; nothing intercepted.
  target_lang  (lang-bar menu button) — runtime-switched per session.

Lives at: C:\\Program Files (x86)\\PIME\\python\\input_methods\\typeanything\\typeanything_ime_main.py
"""

import os.path
import sys

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

try:
    from keycodes import (
        VK_BACK, VK_ESCAPE, VK_RETURN, VK_SPACE, VK_TAB, VK_SHIFT, VK_CAPITAL,
    )
    from textService import TextService, COMMAND_LEFT_CLICK, COMMAND_RIGHT_CLICK, COMMAND_MENU
except ImportError:
    VK_BACK = 0x08
    VK_ESCAPE = 0x1B
    VK_RETURN = 0x0D
    VK_SPACE = 0x20
    VK_TAB = 0x09
    VK_SHIFT = 0x10
    VK_CAPITAL = 0x14
    COMMAND_LEFT_CLICK = 0
    COMMAND_RIGHT_CLICK = 1
    COMMAND_MENU = 2

    class TextService:
        def __init__(self, client):
            self.client = client
        def setCompositionString(self, s): pass
        def setCompositionCursor(self, pos): pass
        def setCommitString(self, s): pass
        def setCandidateList(self, cand): pass
        def setShowCandidates(self, show): pass
        def setSelKeys(self, keys): pass
        def addButton(self, button_id, **kwargs): pass
        def changeButton(self, button_id, **kwargs): pass
        def removeButton(self, button_id): pass

from state import StateMachine, Action, State  # noqa: E402
from config import load_config  # noqa: E402
from llm_client import LLMSession  # noqa: E402
from pinyin_engine import PinyinEngine  # noqa: E402


# Language-bar button command IDs
ID_MODE_TOGGLE = 100
ID_LANG_BASE = 200


class TypeAnythingTextService(TextService):
    def __init__(self, client):
        super().__init__(client)
        self.cfg = load_config(_THIS_DIR)
        self.sm = StateMachine(max_buffer=self.cfg.max_buffer)
        self.engine = PinyinEngine()
        self._llm = None
        self.english_mode = False
        self.target_lang = self.cfg.target_lang
        self.lang_options = list(self.cfg.lang_options)
        self._engine_loaded = False

    # ----- PIME lifecycle -----

    def onActivate(self):
        TextService.onActivate(self)
        # Lazy-load engine on first activation (~75ms)
        if not self._engine_loaded:
            try:
                self.engine.load()
                self._engine_loaded = True
            except Exception:
                pass
        if self._llm is None and self.cfg.api_key:
            self._llm = LLMSession(
                api_key=self.cfg.api_key,
                model=self.cfg.model,
                endpoint=self.cfg.endpoint,
                timeout_s=self.cfg.timeout_s,
                temperature=self.cfg.temperature,
            )
        try:
            self.setSelKeys("123456789")
        except Exception:
            pass
        self._add_buttons()

    def onDeactivate(self):
        TextService.onDeactivate(self)
        # Defensive: explicitly close any open composition + candidate window
        # so switching IMEs (e.g., to MS Pinyin) leaves no residual UI state.
        try:
            self.setCompositionString("")
            self.setCompositionCursor(0)
            self.setCandidateList([])
            self.setShowCandidates(False)
        except Exception:
            pass
        self.sm.reset()
        if self._llm is not None:
            self._llm.close()
            self._llm = None
        try:
            self.removeButton("typeanything-mode")
            self.removeButton("typeanything-lang")
        except Exception:
            pass

    def onCompositionTerminated(self, forced):
        TextService.onCompositionTerminated(self, forced)
        self.sm.reset()
        try:
            self.setCompositionString("")
            self.setCandidateList([])
            self.setShowCandidates(False)
        except Exception:
            pass

    # ----- key filter -----

    def filterKeyDown(self, keyEvent):
        # Always intercept Shift+Space (mode toggle hotkey).
        if keyEvent.keyCode == VK_SPACE and keyEvent.isKeyDown(VK_SHIFT):
            return True
        if self.english_mode:
            return False
        # Active state — intercept everything we can act on.
        if self.sm.state != State.IDLE or self.sm.chinese_buffer:
            return True
        # Idle — only intercept lowercase a-z.
        if keyEvent.isPrintableChar():
            ch = chr(keyEvent.charCode)
            if ch.isalpha() and ch.islower():
                return True
        return False

    def onKeyDown(self, keyEvent):
        # Shift+Space: toggle 中/英 passthrough mode regardless of state.
        if keyEvent.keyCode == VK_SPACE and keyEvent.isKeyDown(VK_SHIFT):
            self.english_mode = not self.english_mode
            self._update_mode_button()
            return True

        if self.english_mode:
            return False

        if keyEvent.keyCode == VK_BACK:
            outcome = self.sm.on_backspace()
        elif keyEvent.keyCode == VK_ESCAPE:
            outcome = self.sm.on_escape()
        elif keyEvent.keyCode == VK_SPACE:
            outcome = self.sm.on_space()
        elif keyEvent.keyCode == VK_RETURN:
            outcome = self.sm.on_enter()
        elif keyEvent.isPrintableChar():
            ch = chr(keyEvent.charCode)
            if ch.isalpha() and ch.islower():
                outcome = self.sm.on_pinyin_char(ch)
            elif ch.isdigit():
                outcome = self.sm.on_digit(int(ch))
            else:
                outcome = self.sm.on_other_char(ch)
        else:
            return False

        return self._apply_outcome(outcome)

    # ----- language-bar buttons -----

    def _add_buttons(self):
        try:
            self.addButton("typeanything-mode",
                text="中" if not self.english_mode else "EN",
                tooltip="Shift+Space: toggle Chinese / English passthrough",
                commandId=ID_MODE_TOGGLE,
                type="button",
            )
            self.addButton("typeanything-lang",
                text=self.target_lang[:3].upper(),
                tooltip="Click to choose target language",
                commandId=ID_LANG_BASE,
                type="menu",
            )
        except Exception:
            pass

    def _update_mode_button(self):
        try:
            self.changeButton("typeanything-mode",
                text="中" if not self.english_mode else "EN")
        except Exception:
            pass

    def _update_lang_button(self):
        try:
            self.changeButton("typeanything-lang",
                text=self.target_lang[:3].upper())
        except Exception:
            pass

    def onCommand(self, commandId, commandType):
        if commandId == ID_MODE_TOGGLE:
            self.english_mode = not self.english_mode
            self._update_mode_button()
            return
        if ID_LANG_BASE <= commandId < ID_LANG_BASE + len(self.lang_options):
            idx = commandId - ID_LANG_BASE
            self.target_lang = self.lang_options[idx]
            self._update_lang_button()
            return

    def onMenu(self, buttonId):
        if buttonId == "typeanything-lang":
            return [
                {"text": lang.title(), "id": ID_LANG_BASE + i}
                for i, lang in enumerate(self.lang_options)
            ]
        return None

    # ----- outcome application -----

    def _apply_outcome(self, outcome) -> bool:
        # Engine query (stage 1, local, fast)
        if Action.QUERY_PINYIN in outcome.actions:
            try:
                cands = self.engine.query(self.sm.pinyin_buffer, top_n=9)
            except Exception:
                cands = []
            follow = self.sm.set_candidates(cands)
            outcome = follow

        # LLM stage 2 (sync, blocking ~500-700ms)
        if Action.TRANSLATE_FULL in outcome.actions:
            chinese = self.sm.chinese_buffer
            try:
                if self._llm is None:
                    raise RuntimeError("LLM not initialized")
                target = self._llm.chinese_to_target(chinese, self.target_lang)
                follow = self.sm.on_full_result(chinese, target)
            except Exception:
                follow = self.sm.on_llm_error(chinese)
            outcome = follow

        for action in outcome.actions:
            if action == Action.SET_COMPOSITION:
                self.setCompositionString(outcome.composition)
                self.setCompositionCursor(len(outcome.composition))
            elif action == Action.COMMIT:
                if outcome.commit:
                    self.setCommitString(outcome.commit)
            elif action == Action.SET_CANDIDATES:
                if outcome.candidates:
                    self.setCandidateList(outcome.candidates)
                    self.setShowCandidates(True)
                else:
                    self.setCandidateList([])
                    self.setShowCandidates(False)
            elif action == Action.CLEAR_CANDIDATES:
                self.setCandidateList([])
                self.setShowCandidates(False)
        return outcome.consumed
