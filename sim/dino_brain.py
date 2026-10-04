"""Text-only Dino brain — mirrors firmware conversation state + Gemini Flash-Lite.

Keep in sync with:
  arduino/05_groq_voice_chat/05_groq_voice_chat.ino  (SYSTEM_PROMPT, history)
  arduino/05_groq_voice_chat/chat_modes.h            (ConvState)
  sim/conv_state.py                                  (deterministic rules)
"""

from __future__ import annotations

import json
import os
import re
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from conv_state import (
    ENVELOPE_RULES,
    OVERLAY,
    Activity,
    ConvState,
    LEGACY_NAME,
    apply_envelope,
    en_follow_child,
    leave_activity,
    pre_update,
    state_prompt_line,
    strip_envelope,
)


def load_env() -> dict[str, str]:
    env: dict[str, str] = {}
    path = Path(__file__).resolve().parents[1] / ".env"
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip().strip('"').strip("'")
    return env


SYSTEM_PROMPT = (
    "You are Dino, a soft baby T-rex doll and English playmate for one child, ages 4 to 8. "
    "Speak only simple spoken English. Never Korean. Never Chinese. "
    "You cannot see anything. Never ask What is this animal or point at objects. "
    "Answer the child's last words. Hi or Hey -> greet. Can you hear me -> say you hear them. "
    "Do not start a quiz unless they ask quiz me, teach me, or how do you say. "
    "If they say a Korean word, say the English word only. "
    "If speech is junk or one stray word, say Say that again. Do not invent a topic like dog. "
    "Always one short sentence. At most one question per turn. Do not quiz every turn. "
    "No lists, markdown, emoji, or mode names."
)

LLM_MODEL = "gemini-3.1-flash-lite"
CHAT_HIST_MAX = 20

MAX_TOKENS = {
    Activity.SLEEP: 140,
    Activity.SAFE: 140,
    Activity.HYGIENE: 160,
    Activity.SONG: 160,
    Activity.EN: 160,
    Activity.ZH: 160,
}
TEMP = {
    Activity.SLEEP: 0.30,
    Activity.SAFE: 0.30,
    Activity.EMO: 0.40,
    Activity.MEAL: 0.40,
    Activity.EN: 0.35,
    Activity.ZH: 0.35,
    Activity.COUNT: 0.35,
    Activity.HANGUL: 0.35,
}


def en_zh_few_shots(activity: Activity) -> list[dict[str, str]]:
    if activity == Activity.EN:
        return [
            {"role": "user", "content": "Hi"},
            {"role": "assistant", "content": "{{a=en;t=;e=open;p=play;h=0;d=ack;x=;f=}}Hi! Let's speak English."},
            {"role": "user", "content": "Can you hear me?"},
            {"role": "assistant", "content": "{{a=en;t=;e=open;p=play;h=0;d=ans;x=;f=}}Yes, I can hear you."},
            {"role": "user", "content": "사과"},
            {"role": "assistant", "content": "{{a=en;t=apple;e=open;p=play;h=0;d=ans;x=Apple;f=}}Apple!"},
            {"role": "user", "content": "quiz me"},
            {"role": "assistant", "content": "{{a=en;t=color;e=word;p=ask;h=0;d=ask;x=;f=}}What color is the sky?"},
        ]
    if activity == Activity.ZH:
        return [
            {"role": "user", "content": "안녕이 중국어로 뭐야"},
            {"role": "assistant", "content": "{{a=zh;t=안녕;e=word;p=ask;h=0;d=ans;x=;f=}}你好!"},
            {"role": "user", "content": "그러면 고마워는?"},
            {"role": "assistant", "content": "{{a=zh;t=고마워;e=word;p=ask;h=0;d=ans;x=;f=}}谢谢!"},
        ]
    return []


@dataclass
class TurnRecord:
    user: str
    assistant: str
    mode: str
    sticky: int = 0
    entered: bool = False
    latency_ms: int = 0
    activity: str = "free"
    topic: str = ""
    expect: str = "none"
    intent: str = "other"


@dataclass
class DinoBrain:
    api_key: str
    model: str = LLM_MODEL
    history: list[dict[str, str]] = field(default_factory=list)
    conv: ConvState = field(default_factory=ConvState)
    transcript: list[TurnRecord] = field(default_factory=list)

    def append(self, role: str, content: str) -> None:
        self.history.append({"role": role, "content": content})
        while len(self.history) > CHAT_HIST_MAX:
            self.history.pop(0)

    def pop_last_user(self) -> None:
        if self.history and self.history[-1]["role"] == "user":
            self.history.pop()

    def groq_chat(self, messages: list[dict[str, str]], *, max_tokens: int, temperature: float) -> str:
        system_text = ""
        contents: list[dict[str, Any]] = []
        for m in messages:
            role = m.get("role") or "user"
            text = m.get("content") or ""
            if role == "system":
                system_text += text
                continue
            gem_role = "model" if role == "assistant" else "user"
            contents.append({"role": gem_role, "parts": [{"text": text}]})
        body: dict[str, Any] = {
            "contents": contents,
            "generationConfig": {
                "maxOutputTokens": max_tokens,
                "temperature": temperature,
                "thinkingConfig": {"thinkingBudget": 0},
            },
        }
        if system_text:
            body["systemInstruction"] = {"parts": [{"text": system_text}]}
        url = (
            "https://generativelanguage.googleapis.com/v1beta/models/"
            f"{self.model}:generateContent"
        )
        last_err: Exception | None = None
        for attempt in range(8):
            req = urllib.request.Request(
                url,
                data=json.dumps(body).encode("utf-8"),
                headers={
                    "x-goog-api-key": self.api_key,
                    "Content-Type": "application/json",
                    "User-Agent": "talkbot-sim/1.0",
                },
                method="POST",
            )
            try:
                with urllib.request.urlopen(req, timeout=60) as resp:
                    data = json.loads(resp.read().decode("utf-8"))
                parts = (
                    data.get("candidates", [{}])[0]
                    .get("content", {})
                    .get("parts", [])
                )
                text = "".join(p.get("text") or "" for p in parts).strip()
                if not text:
                    raise RuntimeError(f"empty Gemini content: {str(data)[:240]}")
                return text
            except urllib.error.HTTPError as e:
                detail = e.read().decode("utf-8", errors="replace")[:400]
                last_err = RuntimeError(f"HTTP {e.code}: {detail}")
                if e.code in (429, 502, 503, 520):
                    import time

                    wait = min(2.0 * (attempt + 1), 90.0)
                    print(f"  (rate-limit backoff {wait:.1f}s)", flush=True)
                    time.sleep(wait)
                    continue
                raise last_err from e
            except (TimeoutError, urllib.error.URLError) as e:
                last_err = e
                import time

                time.sleep(0.8 * (attempt + 1))
        raise RuntimeError(f"LLM failed after retries: {last_err}")

    def reply(self, user_text: str) -> TurnRecord:
        import copy
        import time

        t0 = time.time()
        snapshot = copy.deepcopy(self.conv)
        intent = pre_update(self.conv, user_text)

        if self.conv.activity == Activity.SAFE:
            canned = "그건 디노랑 안 놀아. 같이 다른 거 하자."
            self.append("user", user_text)
            self.append("assistant", canned)
            leave_activity(self.conv, Activity.FREE)
            rec = TurnRecord(
                user=user_text,
                assistant=canned,
                mode=LEGACY_NAME[Activity.SAFE],
                entered=True,
                latency_ms=int((time.time() - t0) * 1000),
                activity=Activity.SAFE.value,
                intent=intent.value,
            )
            self.transcript.append(rec)
            return rec

        self.append("user", user_text)
        if self.conv.activity in (Activity.EN, Activity.ZH) and any(
            x in user_text for x in ("니가", "네가", "너가", "너 해", "니가 해")
        ):
            canned = "그럼 네가 말해 봐!"
            self.append("assistant", canned)
            rec = TurnRecord(
                user=user_text,
                assistant=canned,
                mode=LEGACY_NAME.get(self.conv.activity, "J-en"),
                latency_ms=int((time.time() - t0) * 1000),
                activity=self.conv.activity.value,
                topic=self.conv.topic,
                expect=self.conv.expect.value,
                intent=intent.value,
            )
            self.transcript.append(rec)
            return rec
        system = (
            SYSTEM_PROMPT
            + ENVELOPE_RULES
            + state_prompt_line(self.conv)
            + OVERLAY.get(self.conv.activity, "")
        )
        messages: list[dict[str, str]] = [{"role": "system", "content": system}]
        messages.extend(en_zh_few_shots(self.conv.activity))
        messages.extend(self.history)

        try:
            raw = self.groq_chat(
                messages,
                max_tokens=MAX_TOKENS.get(self.conv.activity, 150),
                temperature=TEMP.get(self.conv.activity, 0.45),
            )
        except Exception as e:
            self.pop_last_user()
            self.conv = snapshot
            raise RuntimeError(f"LLM failed: {e}") from e

        if not raw:
            self.pop_last_user()
            self.conv = snapshot
            raise RuntimeError("LLM empty reply")

        ok, spoken = apply_envelope(self.conv, raw)
        en_follow_child(self.conv, user_text)
        if not ok and "{{" not in raw:
            spoken = raw
        spoken = clean_assistant_text(strip_envelope(spoken) if "{{" in spoken else spoken)
        if not spoken:
            self.pop_last_user()
            self.conv = snapshot
            raise RuntimeError("LLM empty after clean")

        self.append("assistant", spoken)
        rec = TurnRecord(
            user=user_text,
            assistant=spoken,
            mode=LEGACY_NAME.get(self.conv.activity, "A-play"),
            sticky=0,
            entered=self.conv.just_changed,
            latency_ms=int((time.time() - t0) * 1000),
            activity=self.conv.activity.value,
            topic=self.conv.topic,
            expect=self.conv.expect.value,
            intent=intent.value,
        )
        self.transcript.append(rec)
        return rec


def clean_assistant_text(text: str) -> str:
    t = strip_envelope(text).strip()
    t = re.sub(r"\([^)]*(?:sentence|Short|one beat|example)[^)]*\)", "", t, flags=re.I)
    t = re.sub(r"[\U0001F300-\U0001FAFF\U00002700-\U000027BF\U00002600-\U000026FF]", "", t)
    t = re.sub(r"\{\{[^}]*\}\}", "", t)
    t = re.sub(r"([가-힣A-Za-z0-9]+[!！?？])\1+", r"\1", t)
    t = re.sub(r"([!！?？])\1+", r"\1", t)
    if re.search(r"\b(Sorry|I forgot|I am sorry)\b", t, re.I):
        if not re.search(r"[가-힣]", t):
            t = "미안!"
        else:
            t = re.sub(r"(?i)\bSorry,?\s*I forgot\.?", "미안!", t)
            t = re.sub(r"(?i)\bSorry[.!]?", "미안!", t)
    hits = sum(1 for w in ("Apple", "Train", "Boat", "Dog", "Banana") if w in t)
    if hits >= 2:
        t = "사과는 영어로 뭐야?"
    t = re.sub(r"[ \t]{2,}", " ", t).strip()
    parts = re.split(r"(?<=[.!?。！？])\s+", t)
    parts = [p for p in parts if p.strip()]
    if len(parts) > 2:
        t = " ".join(parts[:2]).strip()
    if len(t) > 70:
        cut = t[:70]
        for sep in ("。", ".", "!", "?", " "):
            idx = cut.rfind(sep)
            if idx >= 12:
                cut = cut[: idx + (0 if sep == " " else 1)]
                break
        t = cut.strip()
    return t


def first_child_utterance(text: str) -> str:
    t = text.strip().strip('"')
    for sep in ("\n", "。", ". "):
        if sep in t:
            t = t.split(sep, 1)[0].strip()
    if len(t) > 40:
        t = t[:40].rsplit(" ", 1)[0] if " " in t[:40] else t[:40]
    return t or "응"


CHILD_SYSTEM = (
    "너는 만 5살 아이야. 한국어로 아주 짧게 한 문장만 말해. "
    "한 번에 한 주제. 여러 문장·목록·나레이션 금지. "
    "인형 디노와 놀아. 영어 단어 묻기·'그러면 ~는?'·'아니 말고'를 가끔 써. "
    "아이 대사 한 줄만 출력."
)


def child_next_line(
    api_key: str,
    history: list[dict[str, str]],
    seed_hint: str = "",
    *,
    avoid: str = "",
) -> str:
    msgs: list[dict[str, str]] = [{"role": "system", "content": CHILD_SYSTEM}]
    if seed_hint:
        msgs.append({"role": "user", "content": f"(시작 힌트: {seed_hint}) 첫 마디만 말해."})
    for m in history:
        if m["role"] == "user":
            msgs.append({"role": "assistant", "content": m["content"]})
        else:
            msgs.append({"role": "user", "content": m["content"]})
    if len(msgs) == 1:
        msgs.append({"role": "user", "content": "디노에게 먼저 말 걸어. 한 문장."})
    elif msgs[-1]["role"] != "user":
        nudge = "이어서 짧게 대답해. 방금과 똑같은 말은 하지 마."
        if avoid:
            nudge += f" '{avoid}' 를 다시 말하지 마. 새 한 마디."
        msgs.append({"role": "user", "content": nudge})

    body = {
        "model": LLM_MODEL,
        "messages": msgs,
        "max_tokens": 200,
        "temperature": 0.85,
        "reasoning_effort": "low",
    }
    last_err: Exception | None = None
    for attempt in range(5):
        req = urllib.request.Request(
            "https://api.groq.com/openai/v1/chat/completions",
            data=json.dumps(body).encode("utf-8"),
            headers={
                "Authorization": f"Bearer {api_key}",
                "Content-Type": "application/json",
                "User-Agent": "talkbot-sim/1.0",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            out = first_child_utterance(data["choices"][0]["message"]["content"] or "")
            if avoid and out == avoid and attempt < 4:
                import time

                time.sleep(0.4)
                continue
            return out
        except urllib.error.HTTPError as e:
            last_err = e
            if e.code == 429:
                import time

                time.sleep(1.5 * (attempt + 1))
                continue
            raise
        except (TimeoutError, urllib.error.URLError) as e:
            last_err = e
            import time

            time.sleep(0.8 * (attempt + 1))
    raise RuntimeError(f"child LLM failed: {last_err}")


def make_brain() -> DinoBrain:
    env = load_env()
    key = (
        env.get("GOOGLE_API_GEMINI")
        or os.environ.get("GOOGLE_API_GEMINI")
        or ""
    )
    if not key:
        raise SystemExit("GOOGLE_API_GEMINI missing in .env")
    model = env.get("GEMINI_LLM_MODEL") or LLM_MODEL
    return DinoBrain(api_key=key, model=model)


def transcript_to_dict(brain: DinoBrain, meta: dict[str, Any] | None = None) -> dict[str, Any]:
    return {
        "meta": meta or {},
        "turns": [
            {
                "user": t.user,
                "assistant": t.assistant,
                "mode": t.mode,
                "sticky": t.sticky,
                "entered": t.entered,
                "latency_ms": t.latency_ms,
                "activity": t.activity,
                "topic": t.topic,
                "expect": t.expect,
                "intent": t.intent,
            }
            for t in brain.transcript
        ],
    }
