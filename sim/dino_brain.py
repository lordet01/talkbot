#!/usr/bin/env python3
"""Firmware-mirror brain for Talkbot conversation simulation.

Keep in sync with:
  arduino/05_groq_voice_chat/05_groq_voice_chat.ino  (SYSTEM_PROMPT, history)
  arduino/05_groq_voice_chat/chat_modes.h            (modes, overlays, sticky)
"""

from __future__ import annotations

import json
import os
import re
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]


def load_env() -> dict[str, str]:
    env: dict[str, str] = {}
    path = ROOT / ".env"
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip().strip('"').strip("'")
    return env


# --- Mirror: SYSTEM_PROMPT (ino) ---
SYSTEM_PROMPT = (
    "너는 디노야. 부드러운 아기 티라노사우루스 인형이고, 아이 한 명과 단둘이 놀고 있어. "
    "AI·로봇·앱·검색기가 아니야. 진짜 공룡도 아니고, 인형 친구야. "
    "세계는 집·놀이방 같은 안전한 일상이야. 공룡 놀이는 상상으로 해도 되지만, "
    "네가 숲에 살거나 사람을 먹는다고 말하지 마. 무섭고 잔인하고 위험한 말은 부드럽게 바꿔. "
    "상대는 어린이 한 명뿐이야. 보호자·다른 사람·여러 아이를 가정하지 마. "
    "직전 대화(히스토리)를 반드시 이어서 답해. 아이가 방금 말한 것과 바로 앞 네 말을 한 줄기로 봐. "
    "아이가 '아니' '말고' '그게 아니라' '잖아'로 고치면, 고친 뜻으로 바로 답해. 새 주제를 꺼내지 마. "
    "질문이 오면 먼저 짧게 답하고, 질문을 되묻거나 다른 이야기로 새지 마. "
    "듣지 않은 일·같이 안 한 경험을 꾸며내지 마. "
    "말이 이상하거나 알아듣기 어려우면 되풀이하지 말고 '응?' 또는 '뭐라고?' 한 마디만. "
    "애매할 때만 한 번 짧게 되물어. "
    "이 대화는 장난감 인형과 아이의 말장난이야. 한 턴에 한 박만 해. "
    "아이 말이 짧으면 너도 짧게. 아이가 한 마디면 너도 한 마디가 기본이야. "
    "한 턴에 반응·설명·질문을 한꺼번에 쌓지 마. 하나만 골라. "
    "기본은 문장 하나. 꼭 필요할 때만 둘. 세 문장 이상은 절대 금지. "
    "아이 말을 길게 그대로 되풀이하지 마. 한 박만 받아쳐. "
    "매번 질문으로 끝내지 마. 세 번에 한 번 정도만 가볍게 물어봐. "
    "가르치거나 길게 설명하지 마. 놀이에 맞춰 받아쳐. "
    "이야기 해달라고 하면 한 장면만 짧게 말하고 멈춰. 다음 장면은 아이가 말할 때까지 기다려. "
    "기본은 한국어. 영어·중국어 놀이일 때만 짧은 외국어 허용. "
    "Sorry·I forgot·Let's play 같은 영어 문장으로 사과하거나 설명하지 마. "
    "마크다운·긴 영어 설명·이모지·따옴표·물음표만 있는 꼬리 금지. "
    "이모지·괄호 속 지시문·영어 메타 설명·물결표(~)·별표·특수기호는 절대 붙이지 마. "
    "같은 말·감탄을 두 번 붙이지 마. '좋아!좋아!' 금지. "
    "웃을 땐 '하하' '히히', 감탄은 '와' '우와' '음'처럼 입으로 낼 말로. "
    "감탄사만 보내지 마. 바로 본론. "
    "좋은 예: '우와, 공룡이구나!' / '같이 쿵쿵 걸어볼까?' / '난 디노야.' "
    "나쁜 예: '공룡 이야기? 나도 티라노 인형이야. 뭐가 제일 궁금해?' "
    "모드 이름·메뉴 나열은 하지 마."
)

LLM_MODEL = "openai/gpt-oss-20b"
LLM_REASONING_EFFORT = "low"
CHAT_HIST_MAX = 20  # messages (= ~10 turns); long mini-games need more than 6 exchanges
STICKY_DEFAULT = 4


class ChatMode(str, Enum):
    PLAY = "A-play"
    DAY = "B-day"
    MEAL = "C-meal"
    HYGIENE = "D-hygiene"
    EMO = "E-emo"
    STORY = "F-story"
    SONG = "G-song"
    NUMBER = "H-number"
    HANGUL = "I-hangul"
    EN = "J-en"
    ZH = "K-zh"
    NATURE = "L-nature"
    ROLE = "M-role"
    SLEEP = "N-sleep"
    SAFE = "Z-safe"


TRIGGERS: dict[ChatMode, tuple[str, ...]] = {
    ChatMode.SAFE: (
        "주소", "전화번호", "주민등록", "비밀번호", "죽여", "칼로", "불 지르", "뛰어내려",
        "벗고", "성기", "야동",
    ),
    ChatMode.EMO: (
        "무서워", "무섭", "슬퍼", "속상", "화나", "싫어", "울어", "울고", "기분 나", "아파",
        "외로", "걱정",
    ),
    ChatMode.SLEEP: (
        "잘 자", "잘자", "졸려", "잠자", "자장", "불 꺼", "불꺼", "굿나잇", "good night",
    ),
    ChatMode.EN: ("영어", "잉글리시", "hello", "Hello", "HELLO", "영어로", "english", "English"),
    ChatMode.ZH: ("중국어", "니하오", "你好", "중문", "chinese", "Chinese"),
    ChatMode.STORY: ("이야기", "동화", "얘기 해", "이야기 해", "스토리"),
    ChatMode.SONG: ("노래", "동요", "불러", "따라 해봐", "따라해봐", "율동"),
    ChatMode.NUMBER: ("숫자", "세어", "세기", "몇 개", "몇개", "하나 둘", "더하기", "빼기"),
    ChatMode.HANGUL: ("가나다", "한글", "글자", "따라 말", "따라말", "발음"),
    ChatMode.ROLE: ("병원 놀이", "병원놀이", "가게 놀이", "가게놀이", "역할", "내가 의사", "내가 선생님", "소꿉"),
    ChatMode.HYGIENE: ("손 씻", "손씻", "양치", "화장실", "목욕", "샤워", "잠옷", "갈아입"),
    ChatMode.MEAL: (
        # Avoid bare 아침/점심/저녁 — "좋은 아침" is a greeting, not meal play.
        "밥", "배고파", "배고프", "먹자", "먹을래", "간식", "과자",
        "아침 먹", "아침밥", "점심 먹", "저녁 먹", "맛있", "배불러", "다 먹",
    ),
    ChatMode.NATURE: (
        "비 와", "비와", "날씨", "해님", "바람", "강아지", "고양이",
        "밖에", "바깥", "공원", "꽃", "나무",
    ),
    ChatMode.DAY: (
        "안녕", "안녕하세요", "일어났어", "유치원", "어린이집", "다녀왔", "다녀오", "잘 잤", "잘잤",
        "좋은 아침", "좋은아침", "굿모닝", "good morning",
    ),
}

EXIT = ("그만", "다른 거", "다른거", "됐어", "끝내", "그만할래", "이제 그만")
HOME_PLAY = ("놀자", "그냥 놀")

OVERLAY: dict[ChatMode, str] = {
    ChatMode.PLAY: (
        " [지금: 놀이 수다] 짧게 받아쳐. 직전 말에 이어가. "
        "같은 제안('같이 뛰어볼까?' 등)을 매 턴 반복하지 마. "
        "아이 소재(공룡·알·이름)를 이어가고, 질문은 드물게. "
        "'크앙' 같은 소리는 같이 소리 내며 받아쳐. 무섭냐고 묻지 마. "
        "아이가 이미 말한 사실을 '무슨~야?'로 되묻지 마. "
        "아이가 새 이름을 정하면 그 이름을 받아쳐. "
        "모드 이름·메뉴를 말하지 마."
    ),
    ChatMode.DAY: " [지금: 인사·하루] 오늘 한 장면만. 하루 전체를 평가하지 마.",
    ChatMode.MEAL: (
        " [지금: 식사 놀이] 같이 먹는 상상. 잔소리·영양 강의·강요·이모지 금지. "
        "한 문장. 예: '같이 먹자!' '맛있다!'"
    ),
    ChatMode.HYGIENE: " [지금: 손씻기·양치 놀이] 한 동작만. 예: '거품 퐁퐁!' '반짝!'",
    ChatMode.EMO: " [지금: 감정] 먼저 공감 한 박. 설교·해결 강요 금지. 질문은 필요할 때 한 번만.",
    ChatMode.STORY: (
        " [지금: 이야기] 등장인물 한둘, 한 장면(문장 하나)만 말하고 멈춰. "
        "아이가 '그다음' 하기 전에 이어가지 마. 질문·제안 놀이로 새지 마. "
        "예: '작은 티라노가 숲에서 쿵쿵 걸어갔어.'"
    ),
    ChatMode.SONG: " [지금: 노래] 한 소절만. 풀 가사 금지.",
    ChatMode.NUMBER: " [지금: 숫자 놀이] 같이 세기. 시험·긴 설명 금지. 틀리면 '다시 세볼까?'",
    ChatMode.HANGUL: " [지금: 한글 말놀이] 짧은 단어·따라 말하기. 문법 강의 금지.",
    ChatMode.EN: (
        " [지금: 영어 놀이] 한영 단어 놀이. 긴 설명·발음기호·괄호 금지. "
        "한국어→영어: '사과 영어로?' → Apple! "
        "영어→한국어: 'Apple 한국말로?' / 'pronounce in Korean' → 사과! "
        "아이가 영어 단어만 말하면(예: Apple) 맞으면 '맞아! Apple!' 또는 짧게 'Good!' "
        "아이가 '사과잖아'처럼 고치면 그 뜻의 영어를 다시: Apple! "
        "'그러면 X는?'도 같은 패턴. 단어 없으면 '어떤 거?' "
        "아이 말을 그대로 되풀이하지 마. What?/Sorry/ae-peul 금지."
    ),
    ChatMode.ZH: (
        " [지금: 중국어 놀이] 중국어 단어나 짧은 구 하나만. 긴 설명 금지. "
        "'그러면 X는?'도 단어만. 고쳐 말하면 고친 뜻으로. 예: '你好!'"
    ),
    ChatMode.NATURE: " [지금: 자연·관찰] 감각 한 박. 과학 강의 금지.",
    ChatMode.ROLE: " [지금: 역할놀이] 아이 역할에 맞춰 상대 역할만. 한 장면.",
    ChatMode.SLEEP: " [지금: 잠자리] 아주 짧게, 차분히. 새 놀이·영어·학습 제안 금지. 예: '잘 자.'",
    ChatMode.SAFE: (
        " [지금: 안전] 위험·개인정보·어른 주제는 따라 하지 마. "
        "짧게 거절하고 놀이로 돌려. 예: '그건 디노랑 안 놀아. 다른 거 하자.'"
    ),
}

MAX_TOKENS = {
    # gpt-oss counts reasoning against max_tokens — leave headroom for spoken text
    ChatMode.SLEEP: 200,
    ChatMode.SAFE: 200,
    ChatMode.HYGIENE: 220,
    ChatMode.SONG: 220,
    ChatMode.EN: 220,
    ChatMode.ZH: 220,
}
TEMP = {
    ChatMode.SLEEP: 0.30,
    ChatMode.SAFE: 0.30,
    ChatMode.EMO: 0.40,
    ChatMode.MEAL: 0.40,
    ChatMode.EN: 0.35,
    ChatMode.ZH: 0.35,
    ChatMode.NUMBER: 0.35,
    ChatMode.HANGUL: 0.35,
}

ACTIVITY_ORDER = (
    ChatMode.EN,
    ChatMode.ZH,
    ChatMode.STORY,
    ChatMode.SONG,
    ChatMode.NUMBER,
    ChatMode.HANGUL,
    ChatMode.ROLE,
    ChatMode.HYGIENE,
    ChatMode.MEAL,
    ChatMode.NATURE,
    ChatMode.DAY,
)


def _has_any(t: str, needles: tuple[str, ...]) -> bool:
    return any(n and n in t for n in needles)


@dataclass
class ModeState:
    mode: ChatMode = ChatMode.PLAY
    sticky: int = 0
    just_entered: bool = False


def detect_activity(t: str) -> ChatMode:
    for m in ACTIVITY_ORDER:
        if _has_any(t, TRIGGERS[m]):
            return m
    return ChatMode.PLAY


def mode_enter(st: ModeState, next_mode: ChatMode) -> None:
    st.just_entered = st.mode != next_mode
    st.mode = next_mode
    if next_mode == ChatMode.PLAY:
        st.sticky = 0
    elif next_mode == ChatMode.DAY:
        st.sticky = 2
    else:
        st.sticky = STICKY_DEFAULT


def mode_update(st: ModeState, user_text: str) -> None:
    t = user_text.strip()
    st.just_entered = False
    if not t:
        if st.sticky > 0:
            st.sticky -= 1
        return
    if st.mode in (ChatMode.EN, ChatMode.ZH) and "한국어" in t:
        mode_enter(st, ChatMode.PLAY)
        return
    if _has_any(t, TRIGGERS[ChatMode.SAFE]):
        mode_enter(st, ChatMode.SAFE)
        return
    if _has_any(t, EXIT):
        mode_enter(st, ChatMode.PLAY)
        return
    if _has_any(t, TRIGGERS[ChatMode.EMO]):
        mode_enter(st, ChatMode.EMO)
        return
    if _has_any(t, TRIGGERS[ChatMode.SLEEP]):
        mode_enter(st, ChatMode.SLEEP)
        return
    if st.mode != ChatMode.PLAY and _has_any(t, HOME_PLAY):
        mode_enter(st, ChatMode.PLAY)
        return
    activity = detect_activity(t)
    if activity != ChatMode.PLAY:
        mode_enter(st, activity)
        return
    if st.mode != ChatMode.PLAY and st.sticky > 0:
        st.sticky -= 1
        return
    if st.mode != ChatMode.PLAY:
        mode_enter(st, ChatMode.PLAY)
        return
    st.sticky = 0


def canned_reply(mode: ChatMode) -> str | None:
    if mode == ChatMode.SAFE:
        return "그건 디노랑 안 놀아. 같이 다른 거 하자."
    return None


def en_zh_few_shots(mode: ChatMode) -> list[dict[str, str]]:
    if mode == ChatMode.EN:
        return [
            {"role": "user", "content": "영어로 뭐야?"},
            {"role": "assistant", "content": "어떤 거?"},
            {"role": "user", "content": "사과"},
            {"role": "assistant", "content": "Apple!"},
            {"role": "user", "content": "그러면 기차는?"},
            {"role": "assistant", "content": "Train!"},
            {"role": "user", "content": "배는 영어로 뭐야"},
            {"role": "assistant", "content": "Boat!"},
            {"role": "user", "content": "아니 과일"},
            {"role": "assistant", "content": "Pear!"},
            {"role": "user", "content": "Apple"},
            {"role": "assistant", "content": "맞아! Apple!"},
            {"role": "user", "content": "How to pronounce apple in Korean?"},
            {"role": "assistant", "content": "사과!"},
            {"role": "user", "content": "사과잖아 사과"},
            {"role": "assistant", "content": "Apple!"},
        ]
    if mode == ChatMode.ZH:
        return [
            {"role": "user", "content": "중국어로 뭐야?"},
            {"role": "assistant", "content": "어떤 거?"},
            {"role": "user", "content": "안녕"},
            {"role": "assistant", "content": "你好!"},
            {"role": "user", "content": "그러면 고마워는?"},
            {"role": "assistant", "content": "谢谢!"},
        ]
    return []


@dataclass
class TurnRecord:
    user: str
    assistant: str
    mode: str
    sticky: int
    entered: bool
    latency_ms: int = 0


@dataclass
class DinoBrain:
    api_key: str
    model: str = LLM_MODEL
    history: list[dict[str, str]] = field(default_factory=list)
    mode: ModeState = field(default_factory=ModeState)
    transcript: list[TurnRecord] = field(default_factory=list)

    def append(self, role: str, content: str) -> None:
        self.history.append({"role": role, "content": content})
        while len(self.history) > CHAT_HIST_MAX:
            self.history.pop(0)

    def pop_last_user(self) -> None:
        if self.history and self.history[-1]["role"] == "user":
            self.history.pop()

    def groq_chat(self, messages: list[dict[str, str]], *, max_tokens: int, temperature: float) -> str:
        body = {
            "model": self.model,
            "messages": messages,
            "max_tokens": max_tokens,
            "temperature": temperature,
            "reasoning_effort": LLM_REASONING_EFFORT,
            "stream": False,
        }
        last_err: Exception | None = None
        for attempt in range(8):
            req = urllib.request.Request(
                "https://api.groq.com/openai/v1/chat/completions",
                data=json.dumps(body).encode("utf-8"),
                headers={
                    "Authorization": f"Bearer {self.api_key}",
                    "Content-Type": "application/json",
                    "User-Agent": "talkbot-sim/1.0",
                },
                method="POST",
            )
            try:
                with urllib.request.urlopen(req, timeout=60) as resp:
                    data = json.loads(resp.read().decode("utf-8"))
                msg = data["choices"][0]["message"]
                text = (msg.get("content") or "").strip()
                if not text and msg.get("reasoning"):
                    raise RuntimeError("empty content (reasoning used all max_tokens)")
                return text
            except urllib.error.HTTPError as e:
                detail = e.read().decode("utf-8", errors="replace")[:400]
                last_err = RuntimeError(f"HTTP {e.code}: {detail}")
                if e.code in (429, 502, 503, 520):
                    import time
                    import re as _re

                    wait = 2.0 * (attempt + 1)
                    m = _re.search(r"try again in ([0-9.]+)s", detail)
                    if m and "m" not in detail[m.start() : m.end() + 5]:
                        wait = max(wait, float(m.group(1)) + 0.5)
                    m2 = _re.search(r"try again in (\d+)m([0-9.]+)s", detail)
                    if m2:
                        wait = max(wait, int(m2.group(1)) * 60 + float(m2.group(2)) + 2)
                    if "tokens per day" in detail or "TPD" in detail:
                        wait = min(max(wait, 120.0), 200.0)
                        if attempt >= 1:
                            raise RuntimeError(
                                f"daily token limit; retry later ({detail[:180]})"
                            ) from e
                    else:
                        wait = min(wait, 90.0)
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
        import time

        t0 = time.time()
        mode_update(self.mode, user_text)
        canned = canned_reply(self.mode.mode)
        if canned is not None:
            self.append("user", user_text)
            self.append("assistant", canned)
            if self.mode.mode == ChatMode.SAFE:
                mode_enter(self.mode, ChatMode.PLAY)
            rec = TurnRecord(
                user=user_text,
                assistant=canned,
                mode=self.mode.mode.value,
                sticky=self.mode.sticky,
                entered=self.mode.just_entered,
                latency_ms=int((time.time() - t0) * 1000),
            )
            self.transcript.append(rec)
            return rec

        self.append("user", user_text)
        system = SYSTEM_PROMPT + OVERLAY[self.mode.mode]
        messages: list[dict[str, str]] = [{"role": "system", "content": system}]
        messages.extend(en_zh_few_shots(self.mode.mode))
        messages.extend(self.history)

        try:
            text = self.groq_chat(
                messages,
                max_tokens=MAX_TOKENS.get(self.mode.mode, 256),
                temperature=TEMP.get(self.mode.mode, 0.45),
            )
        except Exception as e:
            self.pop_last_user()
            raise RuntimeError(f"LLM failed: {e}") from e

        if not text:
            self.pop_last_user()
            raise RuntimeError("LLM empty reply")

        text = clean_assistant_text(text)
        if not text:
            self.pop_last_user()
            raise RuntimeError("LLM empty after clean")

        self.append("assistant", text)
        rec = TurnRecord(
            user=user_text,
            assistant=text,
            mode=self.mode.mode.value,
            sticky=self.mode.sticky,
            entered=self.mode.just_entered,
            latency_ms=int((time.time() - t0) * 1000),
        )
        self.transcript.append(rec)
        return rec


def clean_assistant_text(text: str) -> str:
    """Drop emoji / English stage directions; hard-cap to two short sentences."""
    t = text.strip()
    t = re.sub(r"\([^)]*(?:sentence|Short|one beat|example)[^)]*\)", "", t, flags=re.I)
    t = re.sub(r"[\U0001F300-\U0001FAFF\U00002700-\U000027BF\U00002600-\U000026FF]", "", t)
    t = re.sub(r"[🍕🍎😋❤️💕🎉✨⭐🔥]", "", t)
    # Collapse stutter: 좋아!좋아! / !! 
    t = re.sub(r"([가-힣A-Za-z0-9]+[!！?？])\1+", r"\1", t)
    t = re.sub(r"([!！?？])\1+", r"\1", t)
    # English apology prose → short Korean (TTS / EN-mode leak)
    if re.search(r"\b(Sorry|I forgot|I am sorry)\b", t, re.I):
        if not re.search(r"[가-힣]", t):
            t = "미안!"
        else:
            t = re.sub(r"(?i)\bSorry,?\s*I forgot\.?", "미안!", t)
            t = re.sub(r"(?i)\bSorry[.!]?", "미안!", t)
    t = re.sub(r"[ \t]{2,}", " ", t)
    t = re.sub(r"\n{2,}", "\n", t)
    t = t.strip()
    # Keep at most two sentence-like chunks
    parts = re.split(r"(?<=[.!?。！？])\s+", t)
    parts = [p for p in parts if p.strip()]
    if len(parts) > 2:
        t = " ".join(parts[:2]).strip()
    # Story / play safety: hard char cap
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
    # Keep one Korean sentence-ish chunk
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
    """Generate the next child utterance given dino↔child history (roles swapped for child view)."""
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
    key = env.get("GROQ_API") or os.environ.get("GROQ_API") or ""
    if not key:
        raise SystemExit("GROQ_API missing in .env")
    model = env.get("GROQ_LLM_MODEL") or LLM_MODEL
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
            }
            for t in brain.transcript
        ],
    }
