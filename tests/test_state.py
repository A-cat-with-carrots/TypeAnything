"""State machine tests — Plan F5 (per-keystroke local engine + LLM stage 2).

Engine queries are simulated by directly calling sm.set_candidates(...)
with the expected result. The shell (main.py) does engine.query() in
production; tests don't need a live engine.
"""

import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "typeanything_ime"))

from state import StateMachine, State, Action  # noqa: E402


# ---------- basics ----------

def test_initial_state():
    sm = StateMachine()
    assert sm.state == State.IDLE
    assert sm.composition_text() == ""


def test_pinyin_starts_typing_and_emits_query():
    sm = StateMachine()
    out = sm.on_pinyin_char("n")
    assert sm.state == State.TYPING
    assert sm.pinyin_buffer == "n"
    assert Action.QUERY_PINYIN in out.actions


def test_set_candidates_renders_panel():
    sm = StateMachine()
    sm.on_pinyin_char("n")
    out = sm.set_candidates(["你", "拟", "尼"])
    assert sm.candidates == ["你", "拟", "尼"]
    assert Action.SET_CANDIDATES in out.actions
    assert out.candidates == ["你", "拟", "尼"]


def test_pinyin_extends_buffer_and_requeries():
    sm = StateMachine()
    sm.on_pinyin_char("n")
    sm.set_candidates(["你"])
    out = sm.on_pinyin_char("i")
    assert sm.pinyin_buffer == "ni"
    assert Action.QUERY_PINYIN in out.actions


# ---------- candidate selection ----------

def test_space_picks_first_candidate():
    sm = StateMachine()
    sm.on_pinyin_char("n")
    sm.set_candidates(["你", "拟"])
    out = sm.on_space()
    assert sm.chinese_buffer == "你"
    assert sm.state == State.IDLE
    assert sm.pinyin_buffer == ""
    assert Action.CLEAR_CANDIDATES in out.actions


def test_digit_picks_specific_candidate():
    sm = StateMachine()
    for ch in "shiyan":
        sm.on_pinyin_char(ch)
    sm.set_candidates(["实验", "试验", "誓言"])
    out = sm.on_digit(2)
    assert sm.chinese_buffer == "试验"
    assert sm.state == State.IDLE


def test_digit_outside_range_falls_through_committing_first():
    sm = StateMachine()
    sm.on_pinyin_char("n")
    sm.set_candidates(["你"])
    out = sm.on_digit(5)
    # Out-of-range: commit candidates[0] + the digit
    assert out.commit == "你5"


def test_multiple_pinyin_cycles_accumulate_chinese():
    sm = StateMachine()
    for ch in "ni":
        sm.on_pinyin_char(ch)
    sm.set_candidates(["你"])
    sm.on_space()
    assert sm.chinese_buffer == "你"

    for ch in "hao":
        sm.on_pinyin_char(ch)
    sm.set_candidates(["好"])
    sm.on_space()
    assert sm.chinese_buffer == "你好"


def test_space_with_no_candidates_commits_raw_pinyin():
    sm = StateMachine()
    sm.on_pinyin_char("x")
    sm.set_candidates([])  # engine returned nothing
    out = sm.on_space()
    assert sm.chinese_buffer == "x"
    assert sm.state == State.IDLE


# ---------- enter / stage 2 ----------

def test_enter_in_idle_with_chinese_emits_translate_full():
    sm = StateMachine()
    sm.chinese_buffer = "你好"
    out = sm.on_enter()
    assert Action.TRANSLATE_FULL in out.actions


def test_full_result_to_translated():
    sm = StateMachine()
    sm.chinese_buffer = "你好"
    sm.on_enter()
    out = sm.on_full_result("你好", "Hello")
    assert sm.state == State.TRANSLATED
    assert sm.translation == "Hello"
    assert out.composition == "Hello"


def test_space_in_translated_commits():
    sm = StateMachine()
    sm.chinese_buffer = "你好"
    sm.on_enter()
    sm.on_full_result("你好", "Hello")
    out = sm.on_space()
    assert out.commit == "Hello"
    assert sm.state == State.IDLE


def test_enter_mid_typing_picks_first_then_translates():
    sm = StateMachine()
    sm.chinese_buffer = "你好"
    sm.on_pinyin_char("s")
    sm.set_candidates(["世"])
    out = sm.on_enter()
    # Should have committed 世 to chinese_buffer and emitted TRANSLATE_FULL
    assert sm.chinese_buffer == "你好世"
    assert sm.pinyin_buffer == ""
    assert Action.TRANSLATE_FULL in out.actions


def test_enter_in_idle_no_buffer_passthrough():
    sm = StateMachine()
    out = sm.on_enter()
    assert out.consumed is False


# ---------- editing ----------

def test_backspace_in_typing_trims_pinyin():
    sm = StateMachine()
    sm.on_pinyin_char("n")
    sm.on_pinyin_char("i")
    out = sm.on_backspace()
    assert sm.pinyin_buffer == "n"
    assert Action.QUERY_PINYIN in out.actions


def test_backspace_clears_pinyin_keeps_chinese():
    sm = StateMachine()
    sm.chinese_buffer = "你好"
    sm.on_pinyin_char("s")
    out = sm.on_backspace()
    assert sm.pinyin_buffer == ""
    assert sm.chinese_buffer == "你好"
    assert sm.state == State.IDLE


def test_backspace_in_idle_with_chinese_trims_chinese():
    sm = StateMachine()
    sm.chinese_buffer = "你好世"
    out = sm.on_backspace()
    assert sm.chinese_buffer == "你好"


def test_escape_in_typing_clears_pinyin():
    sm = StateMachine()
    sm.chinese_buffer = "你好"
    sm.on_pinyin_char("s")
    out = sm.on_escape()
    assert sm.pinyin_buffer == ""
    assert sm.chinese_buffer == "你好"
    assert sm.state == State.IDLE


def test_escape_in_idle_with_chinese_clears_all():
    sm = StateMachine()
    sm.chinese_buffer = "你好"
    out = sm.on_escape()
    assert sm.chinese_buffer == ""


# ---------- error / commit ----------

def test_full_llm_error_to_error_state():
    sm = StateMachine()
    sm.chinese_buffer = "你好"
    sm.on_enter()
    out = sm.on_llm_error("你好")
    assert sm.state == State.ERROR


def test_space_in_error_commits_chinese():
    sm = StateMachine()
    sm.chinese_buffer = "你好"
    sm.on_enter()
    sm.on_llm_error("你好")
    out = sm.on_space()
    assert out.commit == "你好"
    assert sm.state == State.IDLE


def test_other_char_in_idle_no_buffer_passthrough():
    sm = StateMachine()
    out = sm.on_other_char("!")
    assert out.consumed is False


def test_other_char_commits_chinese_plus_punct():
    sm = StateMachine()
    sm.chinese_buffer = "你好"
    out = sm.on_other_char("!")
    assert out.commit == "你好!"
    assert sm.state == State.IDLE


def test_other_char_in_typing_picks_first_then_commits_with_char():
    sm = StateMachine()
    sm.chinese_buffer = "你"
    sm.on_pinyin_char("h")
    sm.set_candidates(["好"])
    out = sm.on_other_char(",")
    assert out.commit == "你好,"


def test_pinyin_after_translated_commits_then_starts_new():
    sm = StateMachine()
    sm.chinese_buffer = "你好"
    sm.on_enter()
    sm.on_full_result("你好", "Hello")
    out = sm.on_pinyin_char("n")
    assert sm.state == State.TYPING
    assert sm.pinyin_buffer == "n"
    assert out.commit == "Hello"


def test_set_candidates_outside_typing_ignored():
    sm = StateMachine()
    out = sm.set_candidates(["你"])
    # IDLE: should be no-op
    assert sm.candidates == []
