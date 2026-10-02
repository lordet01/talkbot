---
name: talkbot-conversation-sim
description: >-
  Runs text conversation simulations for the Talkbot doll (디노) without mic/speaker,
  finds awkward long-turn replies, and iterates prompt/mode fixes. Use when the user
  asks to simulate dialogue, improve conversation quality, long-turn chat, 대화 시뮬,
  어색한 답, or prompt/mode iteration without flashing the device to talk.
---

# Talkbot conversation simulation

Voice hardware is too slow for prompt iteration. Use the **text simulator** that mirrors
firmware chat modes + system prompt + Groq LLM, then fix awkwardness and re-run.

## When to use

- Long-turn / multi-turn conversation quality work
- English/play/story mode coherence without speaking into the doll
- After changing `SYSTEM_PROMPT`, `chat_modes.h`, or history logic

## Layout

| Path | Role |
|------|------|
| `sim/dino_brain.py` | Firmware-mirror: modes, sticky, history, Groq chat |
| `sim/run_sim.py` | Scripted + free long-turn runner |
| `sim/critique.py` | Heuristic + optional LLM critique of a transcript |
| `sim/scenarios/*.json` | Fixed child lines (regression) |
| `sim/runs/` | Timestamped transcripts + scores |
| `docs/conversation-scenarios.md` | Product conversation design |

## Workflow (mandatory loop)

```
Task Progress:
- [ ] 1. Sync mirror with firmware prompts/modes if they drifted
- [ ] 2. Run regression scenarios (scripted)
- [ ] 3. Run free long-turn (child LLM) for 12–20 turns
- [ ] 4. Critique: list awkward turns with reason codes
- [ ] 5. Patch SYSTEM_PROMPT / chat_modes.h / sim mirror together
- [ ] 6. Re-run failed scenarios only, then full suite
- [ ] 7. Stop when critical issues are gone or user says stop
```

### Commands

```bash
# Scripted regression (no child LLM)
python3 sim/run_sim.py --scenario sim/scenarios/en_vocab_followup.json

# All scenarios
python3 sim/run_sim.py --all

# Free long-turn (child agent continues for N turns)
python3 sim/run_sim.py --free --turns 16 --seed "영어 공부하자"

# Critique last run
python3 sim/critique.py sim/runs/<latest>.json
```

Needs `GROQ_API` in repo `.env` (same as firmware).

### Keep mirror in sync

If you edit firmware prompts, update `sim/dino_brain.py` constants in the **same change**:

- `SYSTEM_PROMPT` ↔ `05_groq_voice_chat.ino`
- Mode overlays / sticky / triggers ↔ `chat_modes.h`
- History length (`CHAT_HIST_MAX`) ↔ ring buffer in `.ino`

## Design goals (long-turn)

1. **Thread continuity**: reply follows the last 2–4 exchanges; no topic jump.
2. **Ellipsis**: `그러면 X는?` continues the same question pattern.
3. **Correction**: `아니` / `말고` revises the prior answer, does not start a new chat.
4. **One beat**: max ~2 short sentences; no lectures.
5. **Mode stickiness**: stay in J-en (etc.) across related turns; exit on clear signal only.
6. **History**: enough turns for a mini-game (default ≥12 messages / ~6 exchanges).

## Awkwardness codes

Use these tags in critiques and commits:

| Code | Meaning |
|------|---------|
| `OFF_TOPIC` | Ignores prior turn / starts unrelated thread |
| `NO_ELLIPSIS` | Misses `그러면 X는?` style follow-up |
| `BAD_CORRECTION` | Ignores `아니/말고` fix (e.g. Fruit! instead of Pear!) |
| `TOO_LONG` | >2 sentences or lecture |
| `META` | "What?" / emoji / parenthetical stage directions |
| `MODE_DRIFT` | Leaves or enters mode wrongly |
| `EMPTY_ASK` | Vague ask answered with guess instead of short clarify |
| `LOOP_ASK` | Same suggestion/question repeated every turn |

## Long-turn sticky note

Firmware sticky (~4) is for *mode preference*, not conversation memory.
Memory is the history ring buffer (`CHAT_HIST_MAX`, default 12 messages).
When free sims feel amnesiac mid-game, raise history cap in **both** `.ino` and `dino_brain.py`.

## Fix priority

1. History bugs / mode sticky (firmware + mirror)
2. System / overlay prompt wording
3. Few-shot format examples (pattern only, not a word dictionary)
4. Temperature / max_tokens only if format still fails

**Do not** add a closed English word dictionary as the main answer path. LLM answers any word; firmware/sim only teach the *pattern*.

## Output to user

After a loop, report briefly in Korean:

1. Which scenarios ran / pass-fail
2. Top awkward turns (quote + code)
3. What you changed
4. Re-run result
