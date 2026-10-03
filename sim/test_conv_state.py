#!/usr/bin/env python3
"""Deterministic conversation-state tests (no LLM)."""

from __future__ import annotations

import copy
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from conv_state import (  # noqa: E402
    Activity,
    ConvState,
    Expect,
    Intent,
    Phase,
    apply_envelope,
    pre_update,
)


def check(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


def test_yes_accepts_proposal() -> None:
    st = ConvState(activity=Activity.COUNT, expect=Expect.YES_NO, phase=Phase.OFFER)
    intent = pre_update(st, "응")
    check(intent == Intent.ANSWER, f"응 → answer, got {intent}")
    check(st.activity == Activity.COUNT, "stay in count")


def test_reject_not_emo() -> None:
    st = ConvState(activity=Activity.EN, expect=Expect.YES_NO, phase=Phase.OFFER)
    intent = pre_update(st, "싫어")
    check(intent == Intent.REJECT, f"싫어 → reject, got {intent}")
    check(st.activity != Activity.EMO, "reject must not enter emo")


def test_help_hint() -> None:
    st = ConvState(activity=Activity.EN, expect=Expect.WORD, phase=Phase.ASK, topic="사과")
    intent = pre_update(st, "몰라")
    check(intent == Intent.HELP, f"몰라 → help, got {intent}")
    check(st.phase == Phase.HINT, "phase hint")
    check(st.hint_level == 1, "hint level 1")


def test_en_topic_dog_not_nature() -> None:
    st = ConvState(activity=Activity.EN, expect=Expect.WORD, phase=Phase.ASK, topic="사과")
    intent = pre_update(st, "강아지는?")
    check(st.activity == Activity.EN, "keep EN activity")
    check(st.topic == "강아지", f"topic dog, got {st.topic!r}")
    check(intent == Intent.QUESTION, f"intent question, got {intent}")


def test_correction_stays() -> None:
    st = ConvState(activity=Activity.EN, expect=Expect.WORD, phase=Phase.ASK, topic="배")
    intent = pre_update(st, "아니, 과일 배")
    check(intent == Intent.CORRECTION, f"got {intent}")
    check(st.activity == Activity.EN, "stay EN")


def test_ani_you_is_not_vocab_correction() -> None:
    st = ConvState(activity=Activity.EN, expect=Expect.CHOICE, phase=Phase.ASK)
    intent = pre_update(st, "아니 니가 뭐..")
    check(intent != Intent.CORRECTION, f"got {intent}")


def test_story_continue() -> None:
    st = ConvState(activity=Activity.STORY, phase=Phase.PLAY)
    intent = pre_update(st, "그다음?")
    check(intent == Intent.CONTINUE, f"got {intent}")
    check(st.expect == Expect.CONT, "expect cont")


def test_explicit_switch() -> None:
    st = ConvState(activity=Activity.FREE)
    intent = pre_update(st, "이제 공룡 이야기 해줘")
    check(intent == Intent.ACT_CHANGE, f"got {intent}")
    check(st.activity == Activity.STORY, "story")


def test_no_sticky_expiry() -> None:
    st = ConvState(activity=Activity.EN, expect=Expect.WORD, phase=Phase.ASK, topic="사과")
    for _ in range(12):
        pre_update(st, "음")
    check(st.activity == Activity.EN, "EN must not expire by turn count")


def test_morning_not_meal() -> None:
    st = ConvState()
    pre_update(st, "좋은 아침이야")
    check(st.activity == Activity.DAY, f"got {st.activity}")


def test_envelope_apply() -> None:
    st = ConvState(activity=Activity.EN)
    ok, spoken = apply_envelope(
        st, "{{a=en;t=강아지;e=word;p=ask;h=0;d=ans;x=Dog;f=}}Dog!"
    )
    check(ok, "envelope ok")
    check(spoken == "Dog!", f"spoken={spoken!r}")
    check(st.topic == "강아지", "topic")
    check(st.expect_ans == "Dog", "expect ans")


def test_envelope_invalid_no_corrupt() -> None:
    st = ConvState(activity=Activity.EN, topic="사과")
    snap = copy.deepcopy(st)
    ok, spoken = apply_envelope(st, "{{a=notreal;t=x;e=word;p=ask;h=0;d=ans;x=;f=}}hi")
    check(not ok, "invalid")
    check(st.topic == snap.topic, "topic unchanged")
    check(spoken == "hi", f"spoken={spoken!r}")


def test_envelope_does_not_speak_meta() -> None:
    st = ConvState(activity=Activity.FREE, expect=Expect.WORD)
    ok, spoken = apply_envelope(
        st, "{{a=;t=;e=ambig;p=ask;h=0;d=clar;x=;f=}}영어공사는 무슨 뜻이야?"
    )
    check(ok, "ambig expect should apply")
    check("{{" not in spoken, f"meta leaked: {spoken!r}")
    check(spoken.startswith("영어공사"), f"spoken={spoken!r}")
    check(st.expect == Expect.NONE, f"expect={st.expect}")


def test_free_cannot_stick_word_expect() -> None:
    st = ConvState(activity=Activity.FREE)
    ok, spoken = apply_envelope(st, "{{a=free;t=;e=word;p=play;h=0;d=ans;x=;f=}}디노야.")
    check(ok, "ok")
    check(spoken == "디노야.", f"spoken={spoken!r}")
    check(st.expect == Expect.NONE, f"expect stuck as {st.expect}")


def test_englishongsa_not_en_mode() -> None:
    st = ConvState(activity=Activity.FREE)
    pre_update(st, "영어공사")
    check(st.activity == Activity.FREE, f"got {st.activity}")


def test_api_failure_restore_pattern() -> None:
    """Simulates firmware: snapshot before pre_update side effects kept on failure path."""
    st = ConvState(activity=Activity.FREE)
    snap = copy.deepcopy(st)
    pre_update(st, "영어 하자")
    check(st.activity == Activity.EN, "entered en")
    st = snap  # restore on API fail
    check(st.activity == Activity.FREE, "restored")


def main() -> None:
    tests = [
        test_yes_accepts_proposal,
        test_reject_not_emo,
        test_help_hint,
        test_en_topic_dog_not_nature,
        test_correction_stays,
        test_ani_you_is_not_vocab_correction,
        test_story_continue,
        test_explicit_switch,
        test_no_sticky_expiry,
        test_morning_not_meal,
        test_envelope_apply,
        test_envelope_invalid_no_corrupt,
        test_envelope_does_not_speak_meta,
        test_free_cannot_stick_word_expect,
        test_englishongsa_not_en_mode,
        test_api_failure_restore_pattern,
    ]
    failed = 0
    for fn in tests:
        try:
            fn()
            print(f"OK  {fn.__name__}")
        except Exception as e:
            failed += 1
            print(f"FAIL {fn.__name__}: {e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    raise SystemExit(1 if failed else 0)


if __name__ == "__main__":
    main()
