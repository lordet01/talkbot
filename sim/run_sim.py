#!/usr/bin/env python3
"""Run Talkbot conversation simulations (scripted or free long-turn)."""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from dino_brain import (  # noqa: E402
    child_next_line,
    make_brain,
    transcript_to_dict,
)


def print_turn(i: int, rec) -> None:
    flag = " ★" if rec.entered else ""
    topic = getattr(rec, "topic", "") or "-"
    expect = getattr(rec, "expect", "") or "-"
    print(
        f"\n[{i}] mode={rec.mode} act={getattr(rec, 'activity', '?')} "
        f"t={topic} e={expect}{flag} ({rec.latency_ms}ms)"
    )
    print(f"  아이: {rec.user}")
    print(f"  디노: {rec.assistant}")


def run_scripted(path: Path) -> Path:
    import time

    data = json.loads(path.read_text(encoding="utf-8"))
    name = data.get("name", path.stem)
    lines = data["child_lines"]
    brain = make_brain()
    print(f"=== scenario: {name} ({len(lines)} turns) ===")
    for i, line in enumerate(lines, 1):
        if i > 1:
            time.sleep(1.2)
        rec = brain.reply(line)
        print_turn(i, rec)
    out = save_run(brain, {"type": "scripted", "scenario": str(path), "name": name})
    print(f"\nsaved: {out}")
    return out


def run_free(turns: int, seed: str) -> Path:
    import time

    brain = make_brain()
    print(f"=== free long-turn ({turns}) seed={seed!r} ===")
    # First child line: seed or child LLM
    if seed:
        child = seed
    else:
        child = child_next_line(brain.api_key, [], seed_hint="영어 공부나 놀이로 시작")
    for i in range(1, turns + 1):
        if i > 1:
            time.sleep(0.6)
        rec = brain.reply(child)
        print_turn(i, rec)
        if i >= turns:
            break
        time.sleep(0.6)
        child = child_next_line(brain.api_key, brain.history, avoid=child)
        if not child:
            child = "응"
    out = save_run(brain, {"type": "free", "turns": turns, "seed": seed})
    print(f"\nsaved: {out}")
    return out


def save_run(brain, meta: dict) -> Path:
    runs = Path(__file__).resolve().parent / "runs"
    runs.mkdir(parents=True, exist_ok=True)
    ts = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    name = meta.get("name") or meta.get("type") or "run"
    safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in str(name))[:40]
    out = runs / f"{ts}_{safe}.json"
    payload = transcript_to_dict(brain, meta)
    out.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description="Talkbot conversation simulator")
    ap.add_argument("--scenario", type=Path, help="JSON scenario with child_lines")
    ap.add_argument("--all", action="store_true", help="Run all sim/scenarios/*.json")
    ap.add_argument("--free", action="store_true", help="Free long-turn with child LLM")
    ap.add_argument("--turns", type=int, default=16)
    ap.add_argument("--seed", type=str, default="영어 공부하자")
    args = ap.parse_args()

    scenarios_dir = Path(__file__).resolve().parent / "scenarios"
    if args.all:
        paths = sorted(scenarios_dir.glob("*.json"))
        if not paths:
            raise SystemExit(f"no scenarios in {scenarios_dir}")
        for p in paths:
            run_scripted(p)
        return
    if args.scenario:
        run_scripted(args.scenario)
        return
    if args.free:
        run_free(args.turns, args.seed)
        return
    ap.print_help()
    raise SystemExit(2)


if __name__ == "__main__":
    main()
