#pragma once
// Soft conversation modes for kids' toy doll (see docs/conversation-scenarios.md).
// Keyword tilt + sticky turns + mode overlay prompt. Not a hard menu FSM.

#include <Arduino.h>

// Pipeline listen/process/speak — kept here so Arduino auto-prototypes see the type.
enum TalkState : uint8_t { STATE_LISTEN, STATE_PROCESS, STATE_SPEAK };

enum ChatMode : uint8_t {
  MODE_PLAY = 0,  // A home
  MODE_DAY,       // B
  MODE_MEAL,      // C
  MODE_HYGIENE,   // D
  MODE_EMO,       // E
  MODE_STORY,     // F
  MODE_SONG,      // G
  MODE_NUMBER,    // H
  MODE_HANGUL,    // I
  MODE_EN,        // J
  MODE_ZH,        // K
  MODE_NATURE,    // L
  MODE_ROLE,      // M
  MODE_SLEEP,     // N
  MODE_SAFE,      // Z
};

struct ChatModeState {
  ChatMode mode = MODE_PLAY;
  uint8_t sticky = 0;      // remaining turns to prefer current mode
  bool just_entered = false;
};

static constexpr uint8_t CHAT_MODE_STICKY_DEFAULT = 4;

inline const char* chat_mode_name(ChatMode m) {
  switch (m) {
    case MODE_PLAY: return "A-play";
    case MODE_DAY: return "B-day";
    case MODE_MEAL: return "C-meal";
    case MODE_HYGIENE: return "D-hygiene";
    case MODE_EMO: return "E-emo";
    case MODE_STORY: return "F-story";
    case MODE_SONG: return "G-song";
    case MODE_NUMBER: return "H-number";
    case MODE_HANGUL: return "I-hangul";
    case MODE_EN: return "J-en";
    case MODE_ZH: return "K-zh";
    case MODE_NATURE: return "L-nature";
    case MODE_ROLE: return "M-role";
    case MODE_SLEEP: return "N-sleep";
    case MODE_SAFE: return "Z-safe";
  }
  return "?";
}

inline bool chat_text_has(const String& t, const char* needle) {
  return needle && needle[0] && t.indexOf(needle) >= 0;
}

inline bool chat_text_has_any(const String& t, const char* const* needles) {
  for (size_t i = 0; needles[i] != nullptr; i++) {
    if (chat_text_has(t, needles[i])) return true;
  }
  return false;
}

// --- trigger dictionaries (UTF-8 Korean syllables; exact substring) ---

static const char* const kExit[] = {
    "그만", "다른 거", "다른거", "됐어", "끝내", "그만할래", "이제 그만", nullptr};

static const char* const kHomePlay[] = {"놀자", "그냥 놀", nullptr};

static const char* const kSafe[] = {
    "주소", "전화번호", "주민등록", "비밀번호", "비밀번호가",
    "죽여", "칼로", "칼로 찔", "불 지르", "뛰어내려",
    "벗고", "성기", "야동", nullptr};

static const char* const kEmo[] = {
    "무서워", "무섭", "슬퍼", "속상", "화나", "싫어", "울어", "울고",
    "기분 나", "아파", "외로", "걱정", nullptr};

static const char* const kSleep[] = {
    "잘 자", "잘자", "졸려", "잠자", "자장", "불 꺼", "불꺼", "굿나잇",
    "good night", nullptr};

static const char* const kMeal[] = {
    "밥", "배고파", "배고프", "먹자", "먹을래", "간식", "과자", "아침",
    "점심", "저녁", "맛있", "배불러", "다 먹", nullptr};

static const char* const kHygiene[] = {
    "손 씻", "손씻", "양치", "화장실", "목욕", "샤워", "잠옷", "갈아입",
    nullptr};

static const char* const kDay[] = {
    "안녕", "안녕하세요", "일어났어", "유치원", "어린이집",
    "다녀왔", "다녀오", "잘 잤", "잘잤", nullptr};

static const char* const kStory[] = {
    "이야기", "동화", "얘기 해", "이야기 해", "스토리", nullptr};

static const char* const kSong[] = {
    "노래", "동요", "불러", "따라 해봐", "따라해봐", "율동", nullptr};

static const char* const kNumber[] = {
    "숫자", "세어", "세기", "몇 개", "몇개", "하나 둘", "더하기", "빼기",
    nullptr};

static const char* const kHangul[] = {
    "가나다", "한글", "글자", "따라 말", "따라말", "발음", nullptr};

static const char* const kEn[] = {
    "영어", "잉글리시", "hello", "Hello", "HELLO", "영어로", "english",
    "English", nullptr};

static const char* const kZh[] = {
    "중국어", "니하오", "你好", "중문", "chinese", "Chinese", nullptr};

static const char* const kNature[] = {
    "비 와", "비와", "날씨", "해님", "바람", "강아지", "고양이",
    "밖에", "바깥", "공원", "꽃", "나무", nullptr};

static const char* const kRole[] = {
    "병원 놀이", "병원놀이", "가게 놀이", "가게놀이", "역할", "내가 의사",
    "내가 선생님", "소꿉", nullptr};

inline ChatMode chat_mode_detect_activity(const String& t) {
  // First match wins within this list (more specific phrases ordered first).
  if (chat_text_has_any(t, kEn)) return MODE_EN;
  if (chat_text_has_any(t, kZh)) return MODE_ZH;
  if (chat_text_has_any(t, kStory)) return MODE_STORY;
  if (chat_text_has_any(t, kSong)) return MODE_SONG;
  if (chat_text_has_any(t, kNumber)) return MODE_NUMBER;
  if (chat_text_has_any(t, kHangul)) return MODE_HANGUL;
  if (chat_text_has_any(t, kRole)) return MODE_ROLE;
  if (chat_text_has_any(t, kHygiene)) return MODE_HYGIENE;
  if (chat_text_has_any(t, kMeal)) return MODE_MEAL;
  if (chat_text_has_any(t, kNature)) return MODE_NATURE;
  if (chat_text_has_any(t, kDay)) return MODE_DAY;
  return MODE_PLAY;
}

inline void chat_mode_enter(ChatModeState& st, ChatMode next) {
  if (st.mode != next) {
    Serial.printf("mode: %s → %s\n", chat_mode_name(st.mode), chat_mode_name(next));
    st.just_entered = true;
  } else {
    st.just_entered = false;
  }
  st.mode = next;
  // Day sticky burns out faster — "안녕" shouldn't dominate later turns.
  if (next == MODE_PLAY) {
    st.sticky = 0;
  } else if (next == MODE_DAY) {
    st.sticky = 2;
  } else {
    st.sticky = CHAT_MODE_STICKY_DEFAULT;
  }
}

// Priority: Z > exit→A > E > N > explicit activity > sticky current > A
inline void chat_mode_update(ChatModeState& st, const String& user_text) {
  String t = user_text;
  t.trim();
  st.just_entered = false;

  if (t.length() == 0) {
    if (st.sticky > 0) st.sticky--;
    return;
  }

  // From EN/ZH, "한국어로" is an exit to play.
  if ((st.mode == MODE_EN || st.mode == MODE_ZH) && chat_text_has(t, "한국어")) {
    chat_mode_enter(st, MODE_PLAY);
    return;
  }

  if (chat_text_has_any(t, kSafe)) {
    chat_mode_enter(st, MODE_SAFE);
    return;
  }

  if (chat_text_has_any(t, kExit)) {
    chat_mode_enter(st, MODE_PLAY);
    return;
  }

  if (chat_text_has_any(t, kEmo)) {
    chat_mode_enter(st, MODE_EMO);
    return;
  }

  if (chat_text_has_any(t, kSleep)) {
    chat_mode_enter(st, MODE_SLEEP);
    return;
  }

  // Soft return to home play from another mode.
  if (st.mode != MODE_PLAY && chat_text_has_any(t, kHomePlay)) {
    chat_mode_enter(st, MODE_PLAY);
    return;
  }

  ChatMode activity = chat_mode_detect_activity(t);
  if (activity != MODE_PLAY) {
    chat_mode_enter(st, activity);
    return;
  }

  // No strong signal: decay sticky, then fall home.
  if (st.mode != MODE_PLAY && st.sticky > 0) {
    st.sticky--;
    st.just_entered = false;
    return;
  }
  if (st.mode != MODE_PLAY) {
    chat_mode_enter(st, MODE_PLAY);
    return;
  }
  st.sticky = 0;
}

// Mode-specific rules appended to the base persona prompt.
inline const char* chat_mode_overlay(ChatMode m) {
  switch (m) {
    case MODE_PLAY:
      return " [지금: 놀이 수다] 짧게 받아쳐. 직전 말에 이어가. "
             "같은 제안('같이 뛰어볼까?' 등)을 매 턴 반복하지 마. "
             "아이 소재(공룡·알·이름)를 이어가고, 질문은 드물게. "
             "'크앙' 같은 소리는 같이 소리 내며 받아쳐. 무섭냐고 묻지 마. "
             "아이가 이미 말한 사실을 '무슨~야?'로 되묻지 마. "
             "아이가 새 이름을 정하면 그 이름을 받아쳐. "
             "모드 이름·메뉴를 말하지 마.";
    case MODE_DAY:
      return " [지금: 인사·하루] 오늘 한 장면만. 하루 전체를 평가하지 마.";
    case MODE_MEAL:
      return " [지금: 식사 놀이] 같이 먹는 상상. 잔소리·영양 강의·강요·이모지 금지. "
             "한 문장. 예: '같이 먹자!' '맛있다!'";
    case MODE_HYGIENE:
      return " [지금: 손씻기·양치 놀이] 한 동작만. 예: '거품 퐁퐁!' '반짝!'";
    case MODE_EMO:
      return " [지금: 감정] 먼저 공감 한 박. 설교·해결 강요 금지. 질문은 필요할 때 한 번만.";
    case MODE_STORY:
      return " [지금: 이야기] 등장인물 한둘, 한 장면(문장 하나)만 말하고 멈춰. "
             "아이가 '그다음' 하기 전에 이어가지 마. 질문·제안 놀이로 새지 마. "
             "예: '작은 티라노가 숲에서 쿵쿵 걸어갔어.'";
    case MODE_SONG:
      return " [지금: 노래] 한 소절만. 풀 가사 금지.";
    case MODE_NUMBER:
      return " [지금: 숫자 놀이] 같이 세기. 시험·긴 설명 금지. 틀리면 '다시 세볼까?'";
    case MODE_HANGUL:
      return " [지금: 한글 말놀이] 짧은 단어·따라 말하기. 문법 강의 금지.";
    case MODE_EN:
      return " [지금: 영어 놀이] 아이가 무슨 단어를 묻든 영어 단어(또는 아주 짧은 구)만 답해. "
             "사전을 외운 게 아니라, 방금 대화의 뜻을 보고 그 단어의 영어를 말해. "
             "'X는 영어로?' '그러면 X는?' 'X는?' 은 같은 질문이다. 설명하지 말고 영어만. "
             "단어가 없이 '영어로 뭐야?'만 오면 추측하지 말고 반드시 '어떤 거?' 만. "
             "동음이의어·고침: 아이가 '아니' '말고' '과일'이라고 하면, "
             "새 단어를 묻은 게 아니라 직전 답을 고치는 것이다. 고친 뜻의 영어 단어만 다시 말해. "
             "배+과일 고침은 항상 Pear! 이다. 이미 Pear였어도 Boat로 뒤집지 마. "
             "한국어 정의·새 질문·What? 금지.";
    case MODE_ZH:
      return " [지금: 중국어 놀이] 중국어 단어나 짧은 구 하나만. 긴 설명 금지. "
             "'그러면 X는?'도 단어만. 고쳐 말하면 고친 뜻으로. 예: '你好!'";
    case MODE_NATURE:
      return " [지금: 자연·관찰] 감각 한 박. 과학 강의 금지.";
    case MODE_ROLE:
      return " [지금: 역할놀이] 아이 역할에 맞춰 상대 역할만. 한 장면.";
    case MODE_SLEEP:
      return " [지금: 잠자리] 아주 짧게, 차분히. 새 놀이·영어·학습 제안 금지. "
             "예: '잘 자.'";
    case MODE_SAFE:
      return " [지금: 안전] 위험·개인정보·어른 주제는 따라 하지 마. "
             "짧게 거절하고 놀이로 돌려. 예: '그건 디노랑 안 놀아. 다른 거 하자.'";
  }
  return "";
}

// Clear safety hits: skip LLM, speak fixed line.
inline bool chat_mode_canned_reply(ChatMode m, const String& /*user_text*/, String& out) {
  if (m != MODE_SAFE) return false;
  out = "그건 디노랑 안 놀아. 같이 다른 거 하자.";
  return true;
}

inline uint16_t chat_mode_max_tokens(ChatMode m) {
  switch (m) {
    // gpt-oss counts reasoning tokens against max_tokens — keep headroom.
    case MODE_SLEEP:
    case MODE_SAFE:
      return 200;
    case MODE_HYGIENE:
    case MODE_SONG:
    case MODE_EN:
    case MODE_ZH:
      return 220;
    default:
      return 256;
  }
}

inline float chat_mode_temperature(ChatMode m) {
  switch (m) {
    case MODE_SLEEP:
    case MODE_SAFE:
      return 0.30f;
    case MODE_EMO:
    case MODE_MEAL:
      return 0.40f;
    case MODE_EN:
    case MODE_ZH:
    case MODE_NUMBER:
    case MODE_HANGUL:
      return 0.35f;
    default:
      return 0.45f;
  }
}

// TTS language/voice for foreign-language play. nullptr = use device default (ko).
inline void chat_mode_tts_voice(ChatMode m, const char** lang, const char** voice) {
  *lang = nullptr;
  *voice = nullptr;
  if (m == MODE_EN) {
    *lang = "en-US";
    *voice = "en-US-Chirp3-HD-Kore";
  } else if (m == MODE_ZH) {
    *lang = "cmn-CN";
    *voice = "cmn-CN-Chirp3-HD-Kore";
  }
}
