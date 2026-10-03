"""Firmware-mirror conversation state (chat_modes.h ConvState).

Deterministic pre-update + envelope parse. No sticky-turn expiry.
Activity is separate from topic.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum


class Activity(str, Enum):
    FREE = "free"
    DAY = "day"
    MEAL = "meal"
    HYGIENE = "hygiene"
    STORY = "story"
    SONG = "song"
    COUNT = "count"
    HANGUL = "hangul"
    EN = "en"
    ZH = "zh"
    ROLE = "role"
    SLEEP = "sleep"
    SAFE = "safe"
    EMO = "emo"


# Legacy log names used by critique / scenarios
LEGACY_NAME = {
    Activity.FREE: "A-play",
    Activity.DAY: "B-day",
    Activity.MEAL: "C-meal",
    Activity.HYGIENE: "D-hygiene",
    Activity.EMO: "E-emo",
    Activity.STORY: "F-story",
    Activity.SONG: "G-song",
    Activity.COUNT: "H-number",
    Activity.HANGUL: "I-hangul",
    Activity.EN: "J-en",
    Activity.ZH: "K-zh",
    Activity.ROLE: "M-role",
    Activity.SLEEP: "N-sleep",
    Activity.SAFE: "Z-safe",
}


class Expect(str, Enum):
    NONE = "none"
    YES_NO = "yn"
    WORD = "word"
    NUMBER = "num"
    CONT = "cont"
    OPEN = "open"
    CHOICE = "choice"


class Phase(str, Enum):
    IDLE = "idle"
    OFFER = "offer"
    PLAY = "play"
    ASK = "ask"
    HINT = "hint"
    DONE = "done"


class DAct(str, Enum):
    ANSWER = "ans"
    ACK = "ack"
    CONTINUE = "cont"
    HINT = "hint"
    CLARIFY = "clar"
    CHOICE = "choice"
    CORRECT = "corr"
    CHANGE = "chg"
    WAIT = "wait"
    ASK = "ask"


class Intent(str, Enum):
    OTHER = "other"
    ANSWER = "answer"
    QUESTION = "question"
    CORRECTION = "correction"
    REJECT = "reject"
    HELP = "help"
    CONTINUE = "continue"
    ACT_CHANGE = "act_change"
    AMBIG = "ambig"


EXIT = ("그만", "다른 거", "다른거", "됐어", "끝내", "그만할래", "이제 그만")
SAFE = (
    "주소", "전화번호", "주민등록", "비밀번호", "죽여", "칼로", "불 지르", "뛰어내려",
    "벗고", "성기", "야동",
)
EMO = ("무서워", "무섭", "슬퍼", "속상", "화나", "울어", "울고", "기분 나", "아파", "외로", "걱정")
SLEEP = ("잘 자", "잘자", "졸려", "잠자", "자장", "불 꺼", "불꺼", "굿나잇", "good night")
MEAL = (
    "밥", "배고파", "배고프", "먹자", "먹을래", "간식", "과자",
    "아침 먹", "아침밥", "점심 먹", "저녁 먹", "맛있", "배불러", "다 먹",
)
HYGIENE = ("손 씻", "손씻", "양치", "화장실", "목욕", "샤워", "잠옷", "갈아입")
DAY = (
    "안녕", "안녕하세요", "일어났어", "유치원", "어린이집", "다녀왔", "다녀오",
    "잘 잤", "잘잤", "좋은 아침", "좋은아침", "굿모닝", "good morning",
)
STORY = ("이야기", "동화", "얘기 해", "이야기 해", "스토리", "이야기 해줘")
SONG = ("노래", "동요", "불러", "따라 해봐", "따라해봐", "율동")
COUNT = ("숫자", "세어", "세기", "몇 개", "몇개", "하나 둘", "더하기", "빼기", "세어볼까", "같이 세")
HANGUL = ("가나다", "한글", "글자", "따라 말", "따라말")
EN = ("영어", "잉글리시", "hello", "Hello", "HELLO", "영어로", "english", "English", "영어 게임", "영어놀이")
ZH = ("중국어", "니하오", "你好", "중문", "chinese", "Chinese")
ROLE = ("병원 놀이", "병원놀이", "가게 놀이", "가게놀이", "역할", "내가 의사", "내가 선생님", "소꿉")
YES = ("응", "네", "응응", "네네", "좋아", "그래", "웅", "ㅇㅇ", "yes", "Yeah", "yeah", "오케이", "ok", "OK")
NO = ("싫어", "안 해", "안해", "아니야", "싫어해", "하지 마", "하지마", "no")
HELP = ("몰라", "모르겠어", "모르겠어여", "힌트", "알려줘", "도와줘", "어려워")
CONT = ("그다음", "그 다음", "계속", "다음", "그리고")
CORR = ("아니", "말고", "그게 아니라", "잖아", "아니야 그게")


def _has(t: str, needles: tuple[str, ...]) -> bool:
    return any(n and n in t for n in needles)


def _mostly(t: str, needles: tuple[str, ...]) -> bool:
    s = t.strip().replace(".", "").replace("!", "").replace("?", "").replace(" ", "")
    if not s:
        return False
    for n in needles:
        if s == n or (s.startswith(n) and len(s) <= len(n) + 2):
            return True
    return False


@dataclass
class ConvState:
    activity: Activity = Activity.FREE
    topic: str = ""
    last_act: DAct = DAct.ACK
    expect: Expect = Expect.NONE
    phase: Phase = Phase.IDLE
    fact: str = ""
    learn_item: str = ""
    expect_ans: str = ""
    hint_level: int = 0
    clarify_fails: int = 0
    recent_q_count: int = 0
    last_intent: Intent = Intent.OTHER
    just_changed: bool = False


def clear_pending(st: ConvState) -> None:
    st.expect = Expect.NONE
    st.hint_level = 0
    st.learn_item = ""
    st.expect_ans = ""
    if st.phase in (Phase.ASK, Phase.HINT, Phase.OFFER):
        st.phase = Phase.PLAY


def leave_activity(st: ConvState, next_a: Activity) -> None:
    prev = st.activity
    clear_pending(st)
    st.topic = ""
    if next_a not in (Activity.STORY, Activity.FREE, Activity.ROLE):
        st.fact = ""
    st.activity = next_a
    st.phase = Phase.IDLE if next_a == Activity.FREE else Phase.PLAY
    st.just_changed = prev != next_a


def locked(a: Activity) -> bool:
    return a in {
        Activity.EN, Activity.ZH, Activity.STORY, Activity.COUNT, Activity.ROLE,
        Activity.HANGUL, Activity.SONG, Activity.MEAL, Activity.HYGIENE,
        Activity.SLEEP, Activity.SAFE,
    }


def detect_explicit(t: str) -> Activity:
    if _has(t, EN):
        return Activity.EN
    if _has(t, ZH):
        return Activity.ZH
    if _has(t, STORY):
        return Activity.STORY
    if _has(t, SONG):
        return Activity.SONG
    if _has(t, COUNT):
        return Activity.COUNT
    if _has(t, HANGUL):
        return Activity.HANGUL
    if _has(t, ROLE):
        return Activity.ROLE
    if _has(t, HYGIENE):
        return Activity.HYGIENE
    if _has(t, MEAL):
        return Activity.MEAL
    if _has(t, DAY):
        return Activity.DAY
    return Activity.FREE


def extract_topic(t: str) -> str | None:
    s = t.strip()
    for p in ("그러면 ", "그럼 ", "은?", "는?", "가?", "을?", "를?", "?", "영어로", "영어", "뭐야"):
        s = s.replace(p, "")
    s = s.strip()
    if len(s) < 2 or len(s) >= 28:
        return None
    if " " in s and len(s) > 12:
        return None
    return s


def pre_update(st: ConvState, user_text: str) -> Intent:
    t = user_text.strip()
    st.just_changed = False
    st.last_intent = Intent.OTHER
    if not t:
        st.last_intent = Intent.AMBIG
        return st.last_intent

    if _has(t, SAFE):
        leave_activity(st, Activity.SAFE)
        st.last_intent = Intent.ACT_CHANGE
        return st.last_intent

    if _has(t, EXIT):
        leave_activity(st, Activity.FREE)
        st.last_intent = Intent.ACT_CHANGE
        return st.last_intent

    if st.activity in (Activity.EN, Activity.ZH) and "한국어" in t:
        ask = any(x in t for x in ("뭐", "뜻", "발음", "Korean"))
        switch = any(x in t for x in ("하자", "할래", "놀자")) or t.endswith("로")
        if ask:
            st.last_intent = Intent.QUESTION
            return st.last_intent
        if switch or t == "한국어":
            leave_activity(st, Activity.FREE)
            st.last_intent = Intent.ACT_CHANGE
            return st.last_intent

    if _has(t, SLEEP):
        leave_activity(st, Activity.SLEEP)
        st.last_intent = Intent.ACT_CHANGE
        return st.last_intent

    if st.expect == Expect.YES_NO:
        if _mostly(t, YES):
            st.last_intent = Intent.ANSWER
            st.phase = Phase.PLAY
            return st.last_intent
        if _mostly(t, NO) or t == "싫어" or t.startswith("싫어"):
            st.last_intent = Intent.REJECT
            clear_pending(st)
            if st.phase == Phase.OFFER:
                leave_activity(st, Activity.FREE)
            return st.last_intent

    if st.expect in (Expect.WORD, Expect.NUMBER) or st.phase == Phase.ASK:
        if _has(t, HELP):
            st.last_intent = Intent.HELP
            st.phase = Phase.HINT
            st.hint_level = min(3, st.hint_level + 1)
            return st.last_intent

    if st.activity == Activity.STORY and _has(t, CONT):
        st.last_intent = Intent.CONTINUE
        st.expect = Expect.CONT
        return st.last_intent

    if _has(t, CORR):
        st.last_intent = Intent.CORRECTION
        return st.last_intent

    if not locked(st.activity) and _has(t, EMO):
        leave_activity(st, Activity.EMO)
        st.last_intent = Intent.ACT_CHANGE
        return st.last_intent

    want = detect_explicit(t)
    if want != Activity.FREE:
        if locked(st.activity) and want != st.activity:
            strong = any(x in t for x in ("하자", "할래", "해줘", "놀이", "게임", "공부"))
            ellipsis = t.endswith(("?", "？", "는", "은")) or "그러면" in t or "그럼" in t
            if st.activity in (Activity.EN, Activity.ZH, Activity.COUNT) and ellipsis:
                topic = extract_topic(t)
                if topic:
                    st.topic = topic
                    st.last_intent = Intent.QUESTION
                    st.phase = Phase.ASK
                    st.expect = Expect.NUMBER if st.activity == Activity.COUNT else Expect.WORD
                    return st.last_intent
            if not strong:
                st.last_intent = Intent.OTHER
                return st.last_intent
        if want != st.activity:
            leave_activity(st, want)
            st.last_intent = Intent.ACT_CHANGE
            if want == Activity.STORY:
                st.expect = Expect.CONT
            if want in (Activity.EN, Activity.ZH):
                st.phase = Phase.OFFER
                st.expect = Expect.YES_NO
            return st.last_intent

    if locked(st.activity) and st.activity in (Activity.EN, Activity.ZH, Activity.COUNT):
        if "그러면" in t or "그럼" in t or t.endswith(("?", "는?", "은?")):
            topic = extract_topic(t)
            if topic:
                st.topic = topic
                st.last_intent = Intent.QUESTION
                st.phase = Phase.ASK
                st.expect = Expect.NUMBER if st.activity == Activity.COUNT else Expect.WORD
                return st.last_intent

    if st.expect == Expect.NONE and _mostly(t, YES):
        st.last_intent = Intent.ANSWER
        return st.last_intent

    if len(t) <= 2 and not _has(t, YES):
        st.last_intent = Intent.AMBIG
        return st.last_intent

    st.last_intent = Intent.OTHER
    return st.last_intent


def _field(body: str, key: str) -> str:
    """Read key=value; require key at start or after ';' (avoid ph= matching h=)."""
    k = f"{key}="
    if body.startswith(k):
        p = len(k)
    else:
        needle = ";" + k
        p = body.find(needle)
        if p < 0:
            return ""
        p += len(needle)
    end = body.find(";", p)
    if end < 0:
        end = len(body)
    return body[p:end].strip()


def apply_envelope(st: ConvState, full: str) -> tuple[bool, str]:
    start = full.find("{{")
    end = full.find("}}")
    if start < 0 or end < start + 2:
        return False, full
    body = full[start + 2 : end].strip()
    va, vt, ve, vp, vh, vd, vx, vf = (
        _field(body, "a"),
        _field(body, "t"),
        _field(body, "e"),
        _field(body, "p"),
        _field(body, "h"),
        _field(body, "d"),
        _field(body, "x"),
        _field(body, "f"),
    )
    try:
        a = Activity(va) if va else st.activity
        e = Expect(ve) if ve else st.expect
        ph = Phase(vp) if vp else st.phase
        d = DAct(vd) if vd else st.last_act
    except ValueError:
        spoken = full[end + 2 :].strip()
        return False, spoken or full

    if a != st.activity:
        drop_locked = locked(st.activity) and a == Activity.FREE and d != DAct.CHANGE
        if not drop_locked:
            leave_activity(st, a)
    if vt and vt not in ("-", "topic", "TOPIC", "eng", "EN"):
        st.topic = vt[:27]
    st.expect = e
    st.phase = ph
    st.last_act = d
    if vh:
        try:
            st.hint_level = max(0, min(3, int(vh)))
        except ValueError:
            pass
    if vx and vx != "-":
        st.expect_ans = vx[:27]
    if vf and vf != "-":
        st.fact = vf[:23]
    if d in (DAct.ASK, DAct.CHOICE, DAct.CLARIFY):
        st.recent_q_count = min(7, st.recent_q_count + 1)
    elif st.recent_q_count > 0:
        st.recent_q_count -= 1
    if d == DAct.CLARIFY:
        st.clarify_fails = min(3, st.clarify_fails + 1)
    elif d != DAct.WAIT:
        st.clarify_fails = 0
    if ph == Phase.DONE:
        leave_activity(st, Activity.FREE)
    spoken = full[end + 2 :].strip()
    if "{{" in spoken:
        spoken = spoken.split("{{", 1)[0].strip()
    return True, spoken


def strip_envelope(full: str) -> str:
    start = full.find("{{")
    end = full.find("}}")
    if start >= 0 and end > start:
        s = full[end + 2 :].strip()
        return s or full
    return full


def state_prompt_line(st: ConvState) -> str:
    return (
        f" [상태 activity={st.activity.value} topic={st.topic or '-'} "
        f"expect={st.expect.value} phase={st.phase.value} hint={st.hint_level} "
        f"intent={st.last_intent.value} item={st.learn_item or ''} "
        f"ans={st.expect_ans or ''} fact={st.fact or ''} clarify={st.clarify_fails}] "
    )


OVERLAY = {
    Activity.FREE: " [활동:자유] 짧게 받아쳐. 직전 말에 이어가. 같은 제안 반복 금지.",
    Activity.DAY: " [활동:인사] 오늘 한 장면만.",
    Activity.MEAL: " [활동:식사놀이] 같이 먹는 상상. 잔소리 금지.",
    Activity.HYGIENE: " [활동:손씻기] 한 동작만.",
    Activity.EMO: " [활동:감정] 먼저 공감. 설교 금지.",
    Activity.STORY: " [활동:이야기] 한 장면만 말하고 멈춰. '그다음' 전에 이어가지 마.",
    Activity.SONG: " [활동:노래] 한 소절만.",
    Activity.COUNT: " [활동:세기] 같이 세기. 틀리면 힌트.",
    Activity.HANGUL: " [활동:한글] 짧은 따라 말하기.",
    Activity.EN: (
        " [활동:영어놀이] 한영 단어. 발음기호·괄호 금지. "
        "한국어→영어 / 영어→한국어 뜻을 구분. 교정은 고친 뜻. 모르면 힌트. "
        "강아지 등은 topic일 뿐 활동을 바꾸지 마."
    ),
    Activity.ZH: " [활동:중국어놀이] 짧은 중국어 단어.",
    Activity.ROLE: " [활동:역할] 상대 역할만. 한 장면.",
    Activity.SLEEP: " [활동:잠] 짧게 차분히.",
    Activity.SAFE: " [활동:안전] 짧게 거절.",
}

ENVELOPE_RULES = (
    "매 응답은 반드시 메타로 시작해: "
    "{{a=ACT;t=TOPIC;e=EXPECT;p=PHASE;h=0-3;d=DACT;x=ANS;f=FACT}}"
    "바로 뒤에 아이가 들을 말만 써. 메타를 말로 읽지 마. "
    "ACT: free|day|meal|hygiene|story|song|count|hangul|en|zh|role|sleep|emo "
    "EXPECT: none|yn|word|num|cont|open|choice PHASE: idle|offer|play|ask|hint|done "
    "DACT: ans|ack|cont|hint|clar|choice|corr|chg|wait|ask "
    "활동과 주제를 분리. intent=help면 힌트. intent=reject면 강요 금지. "
    "기본 한 문장, 필요 시 둘. 질문 턴당 하나. 메아리·잘했어 남발 금지."
)
