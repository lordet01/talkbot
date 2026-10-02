#!/usr/bin/env python3
"""Run scenarios + free turns, critique each, print summary. Exit 1 if critical issues."""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from critique import critique_turns  # noqa: E402
from run_sim import run_free, run_scripted  # noqa: E402

CRITICAL = {"OFF_TOPIC", "BAD_CORRECTION", "NO_ELLIPSIS", "META", "LOOP_ASK", "EMPTY_ASK", "TOO_LONG"}


def critique_path(path: Path) -> list[dict]:
    data = json.loads(path.read_text(encoding="utf-8"))
    issues = critique_turns(data.get("turns", []))
    print(f"\n-- critique {path.name} --")
    print(f"turns={len(data.get('turns', []))} issues={len(issues)}")
    for it in issues:
        print(f"  #{it['turn']} [{','.join(it['codes'])}]")
        print(f"    아이: {it['user']}")
        print(f"    디노: {it['assistant']}")
    return issues


def main() -> None:
    scenarios = sorted((Path(__file__).resolve().parent / "scenarios").glob("*.json"))
    all_issues: list[tuple[str, list[dict]]] = []
    for p in scenarios:
        out = run_scripted(p)
        issues = critique_path(out)
        all_issues.append((p.name, issues))
        time.sleep(1.0)

    free_out = run_free(turns=12, seed="디노야 영어 공부하자")
    free_issues = critique_path(free_out)
    all_issues.append(("free", free_issues))

    print("\n======== SUMMARY ========")
    critical_n = 0
    for name, issues in all_issues:
        crit = [i for i in issues if CRITICAL & set(i["codes"])]
        critical_n += len(crit)
        status = "FAIL" if crit else ("WARN" if issues else "PASS")
        print(f"  {status} {name}: {len(issues)} issues ({len(crit)} critical)")
    if critical_n:
        sys.exit(1)


if __name__ == "__main__":
    main()
