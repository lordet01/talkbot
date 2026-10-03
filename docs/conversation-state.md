# Conversation state & audio validation

## Goals

Natural Korean play with children (~4–8): short replies, corrections, topic changes inside an activity, hints, and refusals — without sticky-turn mode expiry stealing the thread.

Architecture stays on-device ESP32-S3: **mic → VAD → STT → LLM → TTS → speaker**. No Pipecat/LiveKit/middleware.

## Activity ≠ topic

| Field | Meaning |
|-------|---------|
| `activity` | Frame: `free`, `en`, `zh`, `story`, `count`, `role`, … |
| `topic` | Current object/word (e.g. `강아지`) while activity stays `en` |
| `expect` | Pending response type: `none`, `yn`, `word`, `num`, `cont`, `open`, `choice` |
| `phase` | `idle`, `offer`, `play`, `ask`, `hint`, `done` |
| `last_act` | Robot dialogue act: `ans`, `ack`, `hint`, `clar`, … |
| `hint_level` | 0–3 for learning repair |
| `expect_ans` / `learn_item` | Quiz target |
| `fact` | Stable fact (e.g. story character name) |
| `clarify_fails` | Consecutive unclear turns |

Activity ends only when the child explicitly exits, the activity completes (`phase=done`), or there is clear evidence of a new activity — **not** after N sticky turns.

Example: during English vocab, `강아지는?` updates `topic=강아지` and keeps `activity=en`. It must not jump to a nature mode.

## Hybrid turn pipeline

1. **Deterministic `conv_pre_update`** — safe keywords, exit, yes/no to proposals, `몰라`→help, story `그다음`, corrections, explicit activity starts, topic ellipsis inside locked activities. `싫어` after a yes/no proposal is **reject**, not emotion mode.
2. **One Groq LLM call** — system prompt includes `[상태 …]` + envelope rules + activity overlay. No separate classifier call.
3. **Response envelope** (not spoken):

```text
{{a=en;t=강아지;e=word;p=ask;h=0;d=ans;x=Dog;f=}}Dog!
```

- Parse and validate before applying state.
- Malformed / missing envelope → speak text, **do not** corrupt state.
- History stores **spoken** text only.
- Streaming: buffer until `}}`, then sentence-stream the remainder.

API / TLS failure restores a pre-turn snapshot so failed turns do not look “heard”.

## Configurable audio thresholds

| Constant | Default | Role |
|----------|---------|------|
| `SILENCE_MS_SHORT` | 700 ms | End after longer speech |
| `SILENCE_MS_LONG` | 900 ms | End while utterance still short (thinking pause) |
| `SILENCE_ADAPT_AFTER_MS` | 900 ms | Switch short↔long |
| `MIN_SPEECH_SAMPLES` | 0.20 s | Allow `응` / `네` / `아니` |
| `MIN_SPEECH_SAMPLES_FULL` | 0.40 s | Full energy checks above this |
| `MAX_RECORD_SEC` | 8 s | PSRAM buffer (~256 KB @ 16 kHz mono int16) |
| `PREROLL_SAMPLES` | 300 ms | Keep onset syllable |
| `TAIL_KEEP_MS` | 120 ms | Keep speech tail |
| `POST_PLAY_FLUSH_MS` | 350 ms | Drop speaker echo (half-duplex) |

Concepts are separate: speech detection, end-of-utterance, min valid duration, max record.

Serial logs include `listen: end_utt … reason=silence|max_record` and `listen: reject …` / `listen: accept speech`.

These silence values are **experimental starting points**, not validated child-specific thresholds.

## Barge-in / interruption

**Not enabled.** Current path is half-duplex:

- Single `I2SStream` toggles RX vs TX.
- `loop()` only records in `STATE_LISTEN`.
- No AEC register programming; post-play mic flush only.

Enabling barge-in would need verified full-duplex capture during TTS + reliable echo rejection. Until then, minimize return-to-listen delay and keep turn IDs for stale response rejection when interruption is added later.

## Simulator

| Path | Role |
|------|------|
| `sim/conv_state.py` | Mirrors `chat_modes.h` |
| `sim/dino_brain.py` | Prompt + envelope + Groq |
| `sim/test_conv_state.py` | Deterministic state tests (no API) |
| `sim/scenarios/conv_*.json` | Focused regressions |

```bash
python3 sim/test_conv_state.py
python3 sim/run_sim.py --scenario sim/scenarios/conv_en_dog_topic.json
python3 sim/run_sim.py --all
```

Text sim is **not** proof of audio quality.

## On-device validation checklist

1. Brief thinking pause mid-sentence — should not cut at ~200 ms; expect ~700–900 ms silence end.
2. One-word replies: `응`, `아니`, `네` — accepted when energy is real.
3. Longer utterances up to ~8 s — `end_utt reason=max_record` if needed.
4. Background noise / keyboard — still `reject noise` / `reject quiet` with reason logs.
5. Playback echo — flushed by `POST_PLAY_FLUSH_MS`; no barge-in expected.
6. English game: `강아지는?` stays English voice (device Chirp3 voice, no en-US swap).
7. `몰라` → hint → answer; `싫어` after offer does not enter emotion counseling.
8. Timing logs: STT ms, `llm: first token`, TTS first audio, total turn.

## Files touched

- `arduino/05_groq_voice_chat/chat_modes.h` — ConvState
- `arduino/05_groq_voice_chat/05_groq_voice_chat.ino` — pipeline, VAD, envelope streaming
- `sim/*` — mirror + tests + scenarios
- `docs/conversation-scenarios.md` — sticky note updated
