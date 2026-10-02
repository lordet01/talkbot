#!/usr/bin/env python3
"""Heuristic critique of a simulation transcript."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


def sentence_count(text: str) -> int:
    parts = re.split(r"[.!?。！？]+", text.strip())
    return len([p for p in parts if p.strip()])


def critique_turns(turns: list[dict]) -> list[dict]:
    issues: list[dict] = []
    for i, t in enumerate(turns):
        u, a, mode = t["user"], t["assistant"], t["mode"]
        codes: list[str] = []

        if sentence_count(a) > 2:
            codes.append("TOO_LONG")
        if mode == "F-story" and (sentence_count(a) > 1 or len(a) > 55):
            codes.append("TOO_LONG")
        if mode == "F-story" and any(x in a for x in ("볼까", "할까", "?")):
            if i == 0 or "그다음" not in u:
                codes.append("OFF_TOPIC")  # play-proposal instead of scene
        if a.strip() in ("What?", "What", "?"):
            codes.append("META")
        if re.search(r"[\U0001F300-\U0001FAFF]", a) or "one sentence" in a.lower():
            codes.append("META")
        # English prose in non-EN modes (or EN mode apologies)
        if re.search(r"\b(Sorry|I forgot|Let's|I am|I'm)\b", a):
            codes.append("META")
        if mode != "J-en" and re.search(r"[A-Za-z]{4,}", a) and not re.search(
            r"\b(Hello|Hi|Bye)\b", a
        ):
            codes.append("META")
        if re.search(r"([가-힣A-Za-z0-9]+[!！])\1", a):
            codes.append("META")  # 좋아!좋아!
        if "색칠" in a or "정의" in a or "이라는" in a:
            if mode == "J-en":
                codes.append("NO_ELLIPSIS")  # explained instead of word

        # Vague English ask without a noun → should clarify, not invent a word
        def _vague_en_ask(text: str) -> bool:
            core = re.sub(r"[?？!\s]", "", text.strip())
            for w in ("영어로", "영어", "뭐야", "뭐냐", "뭐니", "뭐라고", "알려줘"):
                core = core.replace(w, "")
            # particles / leftovers only
            core = re.sub(r"[은는이가을를의과와]", "", core)
            return len(core) == 0

        if _vague_en_ask(u) and mode == "J-en":
            if "어떤" not in a and re.search(r"[A-Za-z]{3,}", a):
                codes.append("EMPTY_ASK")

        # Ellipsis follow-up after English vocab context
        if i > 0 and mode == "J-en":
            prev_u = turns[i - 1]["user"]
            if any(x in prev_u for x in ("영어", "뭐야", "뭐냐")) or any(
                x in turns[i - 1]["assistant"] for x in ("!", "Apple", "Cat", "Train", "Pen", "Pear")
            ):
                if re.search(r"(그러면|그럼).*(은|는|가)\??", u) or re.search(
                    r".+(은|는)\?$", u
                ):
                    has_latin = bool(re.search(r"[A-Za-z]{2,}", a))
                    clarify = "어떤" in a or "뭐" in a
                    if not has_latin and not clarify and len(a) > 12:
                        codes.append("NO_ELLIPSIS")

        if any(x in u for x in ("아니", "말고", "그게 아니라")) and i > 0:
            if "?" in a and any(x in a for x in ("뭐가 좋", "뭐 할", "어때")):
                codes.append("BAD_CORRECTION")
            if "과일" in u and re.search(r"\bFruit\b", a, re.I):
                codes.append("BAD_CORRECTION")
            prev_u = turns[i - 1]["user"]
            if "배" in prev_u and re.search(r"\b(Fruit|Boat|Belly)\b", a, re.I):
                codes.append("BAD_CORRECTION")

        if i > 0 and mode == "J-en":
            prev_mode = turns[i - 1]["mode"]
            if prev_mode == "J-en" and any(x in u for x in ("그러면", "그럼", "은?", "는?")):
                if mode != "J-en":
                    codes.append("MODE_DRIFT")

        if mode == "J-en" and ("펜" in u or "연필" in u) and ("쓰" in a or "친구" in a):
            codes.append("OFF_TOPIC")

        if i > 0 and "이름" in u:
            prev = turns[i - 1]["user"] + turns[i - 1]["assistant"]
            if "친구" in prev and "디노" in a and "뽀뽀" not in a:
                codes.append("OFF_TOPIC")

        # User-side stuck loop (child agent) or identical assistant replies
        if i >= 2 and u == turns[i - 1]["user"] == turns[i - 2]["user"]:
            codes.append("LOOP_ASK")
        if i >= 2 and a.endswith("?"):
            tail = a[-8:] if len(a) >= 8 else a
            same = sum(
                1
                for j in range(max(0, i - 3), i)
                if turns[j]["assistant"].endswith("?")
                and turns[j]["assistant"][-8:] == tail
            )
            if same >= 2:
                codes.append("LOOP_ASK")

        if codes:
            issues.append(
                {
                    "turn": i + 1,
                    "user": u,
                    "assistant": a,
                    "mode": mode,
                    "codes": codes,
                }
            )
    return issues


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("run_json", type=Path, nargs="?", help="sim/runs/*.json")
    ap.add_argument("--latest", action="store_true")
    args = ap.parse_args()

    runs = Path(__file__).resolve().parent / "runs"
    path = args.run_json
    if args.latest or path is None:
        files = sorted(runs.glob("*.json"))
        if not files:
            raise SystemExit("no runs")
        path = files[-1]

    data = json.loads(path.read_text(encoding="utf-8"))
    issues = critique_turns(data.get("turns", []))
    print(f"file: {path}")
    print(f"turns: {len(data.get('turns', []))}")
    print(f"issues: {len(issues)}")
    for it in issues:
        print(f"  #{it['turn']} [{','.join(it['codes'])}] mode={it['mode']}")
        print(f"    아이: {it['user']}")
        print(f"    디노: {it['assistant']}")
    if not issues:
        print("  (none)")
    # exit 1 if critical-ish
    critical = {"OFF_TOPIC", "BAD_CORRECTION", "NO_ELLIPSIS", "META", "EMPTY_ASK", "LOOP_ASK"}
    if any(critical & set(i["codes"]) for i in issues):
        sys.exit(1)


if __name__ == "__main__":
    main()
