#pragma once
// Compact conversation state for kids' doll (4–8). Activity ≠ topic.
// Sticky-turn expiry removed — leave only on explicit request, completion, or clear evidence.
// See docs/conversation-state.md.

#include <Arduino.h>

enum TalkState : uint8_t { STATE_LISTEN, STATE_PROCESS, STATE_SPEAK };

// Activity (what game/frame we're in). Topic is a separate string field.
enum Activity : uint8_t {
  ACT_FREE = 0,
  ACT_DAY,
  ACT_MEAL,
  ACT_HYGIENE,
  ACT_STORY,
  ACT_SONG,
  ACT_COUNT,
  ACT_HANGUL,
  ACT_EN,
  ACT_ZH,
  ACT_ROLE,
  ACT_SLEEP,
  ACT_SAFE,
  ACT_EMO,  // only when child expresses distress, not after "싫어" to a proposal
};

enum ExpectType : uint8_t {
  EXPECT_NONE = 0,
  EXPECT_YES_NO,   // proposal acceptance
  EXPECT_WORD,     // vocab / quiz answer
  EXPECT_NUMBER,
  EXPECT_CONT,     // story "그다음?"
  EXPECT_OPEN,
  EXPECT_CHOICE,
};

enum DialogueAct : uint8_t {
  DACT_ANSWER = 0,
  DACT_ACK,
  DACT_CONTINUE,
  DACT_HINT,
  DACT_CLARIFY,
  DACT_CHOICE,
  DACT_CORRECT,
  DACT_CHANGE,
  DACT_WAIT,
  DACT_ASK,
};

enum UtterIntent : uint8_t {
  UINTENT_OTHER = 0,
  UINTENT_ANSWER,
  UINTENT_QUESTION,
  UINTENT_CORRECTION,
  UINTENT_REJECT,
  UINTENT_HELP,
  UINTENT_CONTINUE,
  UINTENT_ACT_CHANGE,
  UINTENT_AMBIG,
};

enum ActivityPhase : uint8_t {
  PHASE_IDLE = 0,
  PHASE_OFFER,
  PHASE_PLAY,
  PHASE_ASK,
  PHASE_HINT,
  PHASE_DONE,
};

static constexpr size_t CONV_TOPIC_LEN = 28;
static constexpr size_t CONV_FACT_LEN = 24;
static constexpr size_t CONV_EXPECT_ANS_LEN = 28;
static constexpr size_t CONV_LEARN_ITEM_LEN = 28;
static constexpr uint8_t CONV_CLARIFY_MAX = 3;

struct ConvState {
  Activity activity = ACT_FREE;
  char topic[CONV_TOPIC_LEN] = {0};
  DialogueAct last_act = DACT_ACK;
  ExpectType expect = EXPECT_NONE;
  ActivityPhase phase = PHASE_IDLE;
  char fact[CONV_FACT_LEN] = {0};           // e.g. story character name
  char learn_item[CONV_LEARN_ITEM_LEN] = {0};
  char expect_ans[CONV_EXPECT_ANS_LEN] = {0};
  uint8_t hint_level = 0;                   // 0 = none yet
  uint8_t clarify_fails = 0;
  uint8_t recent_q_count = 0;               // questions asked in last few turns
  UtterIntent last_intent = UINTENT_OTHER;
  bool just_changed_activity = false;
};

// Legacy aliases so older call sites / logs still compile during transition.
using ChatMode = Activity;
enum : uint8_t {
  MODE_PLAY = ACT_FREE,
  MODE_DAY = ACT_DAY,
  MODE_MEAL = ACT_MEAL,
  MODE_HYGIENE = ACT_HYGIENE,
  MODE_EMO = ACT_EMO,
  MODE_STORY = ACT_STORY,
  MODE_SONG = ACT_SONG,
  MODE_NUMBER = ACT_COUNT,
  MODE_HANGUL = ACT_HANGUL,
  MODE_EN = ACT_EN,
  MODE_ZH = ACT_ZH,
  MODE_NATURE = ACT_FREE,  // nature words are TOPIC only, not an activity
  MODE_ROLE = ACT_ROLE,
  MODE_SLEEP = ACT_SLEEP,
  MODE_SAFE = ACT_SAFE,
};
struct ChatModeState {
  // Thin view used by older Serial prints; real state is ConvState.
  ChatMode mode = ACT_FREE;
  uint8_t sticky = 0;  // unused (kept 0); no sticky expiry
  bool just_entered = false;
};

inline const char* activity_name(Activity a) {
  switch (a) {
    case ACT_FREE: return "free";
    case ACT_DAY: return "day";
    case ACT_MEAL: return "meal";
    case ACT_HYGIENE: return "hygiene";
    case ACT_STORY: return "story";
    case ACT_SONG: return "song";
    case ACT_COUNT: return "count";
    case ACT_HANGUL: return "hangul";
    case ACT_EN: return "en";
    case ACT_ZH: return "zh";
    case ACT_ROLE: return "role";
    case ACT_SLEEP: return "sleep";
    case ACT_SAFE: return "safe";
    case ACT_EMO: return "emo";
  }
  return "?";
}

inline const char* chat_mode_name(ChatMode m) {
  // Keep letter prefixes for existing log greps / critique.
  switch (m) {
    case ACT_FREE: return "A-play";
    case ACT_DAY: return "B-day";
    case ACT_MEAL: return "C-meal";
    case ACT_HYGIENE: return "D-hygiene";
    case ACT_EMO: return "E-emo";
    case ACT_STORY: return "F-story";
    case ACT_SONG: return "G-song";
    case ACT_COUNT: return "H-number";
    case ACT_HANGUL: return "I-hangul";
    case ACT_EN: return "J-en";
    case ACT_ZH: return "K-zh";
    case ACT_ROLE: return "M-role";
    case ACT_SLEEP: return "N-sleep";
    case ACT_SAFE: return "Z-safe";
  }
  return "?";
}

inline const char* expect_name(ExpectType e) {
  switch (e) {
    case EXPECT_NONE: return "none";
    case EXPECT_YES_NO: return "yn";
    case EXPECT_WORD: return "word";
    case EXPECT_NUMBER: return "num";
    case EXPECT_CONT: return "cont";
    case EXPECT_OPEN: return "open";
    case EXPECT_CHOICE: return "choice";
  }
  return "?";
}

inline const char* dact_name(DialogueAct d) {
  switch (d) {
    case DACT_ANSWER: return "ans";
    case DACT_ACK: return "ack";
    case DACT_CONTINUE: return "cont";
    case DACT_HINT: return "hint";
    case DACT_CLARIFY: return "clar";
    case DACT_CHOICE: return "choice";
    case DACT_CORRECT: return "corr";
    case DACT_CHANGE: return "chg";
    case DACT_WAIT: return "wait";
    case DACT_ASK: return "ask";
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

inline void conv_set_str(char* dst, size_t n, const char* src) {
  if (!dst || n == 0) return;
  if (!src) {
    dst[0] = 0;
    return;
  }
  strncpy(dst, src, n - 1);
  dst[n - 1] = 0;
}

inline bool conv_child_hands_quiz_back(const String& t) {
  return chat_text_has(t, "니가") || chat_text_has(t, "네가") || chat_text_has(t, "너가") ||
         chat_text_has(t, "너 해") || chat_text_has(t, "니가 해");
}

inline void conv_clear_pending(ConvState& st) {
  st.expect = EXPECT_NONE;
  st.hint_level = 0;
  st.learn_item[0] = 0;
  st.expect_ans[0] = 0;
  if (st.phase == PHASE_ASK || st.phase == PHASE_HINT || st.phase == PHASE_OFFER) {
    st.phase = PHASE_PLAY;
  }
}

inline void conv_leave_activity(ConvState& st, Activity next) {
  Activity prev = st.activity;
  conv_clear_pending(st);
  st.topic[0] = 0;
  // Preserve fact only if still useful (story name across free chat is ok briefly).
  if (next != ACT_STORY && next != ACT_FREE && next != ACT_ROLE) {
    st.fact[0] = 0;
  }
  st.activity = next;
  st.phase = (next == ACT_FREE) ? PHASE_IDLE : PHASE_PLAY;
  st.just_changed_activity = (prev != next);
  if (prev != next) {
    Serial.printf("act: %s → %s\n", activity_name(prev), activity_name(next));
  }
}

// --- Deterministic trigger lists (explicit only; topic words do not steal activity) ---

static const char* const kExit[] = {
    "그만", "다른 거", "다른거", "됐어", "끝내", "그만할래", "이제 그만", nullptr};

static const char* const kSafe[] = {
    "주소", "전화번호", "주민등록", "비밀번호", "비밀번호가",
    "죽여", "칼로", "칼로 찔", "불 지르", "뛰어내려",
    "벗고", "성기", "야동", nullptr};

static const char* const kEmoDistress[] = {
    // Real distress — NOT proposal rejection ("싫어" alone is handled via expect).
    "무서워", "무섭", "슬퍼", "속상", "화나", "울어", "울고",
    "기분 나", "아파", "외로", "걱정", nullptr};

static const char* const kSleep[] = {
    "잘 자", "잘자", "졸려", "잠자", "자장", "불 꺼", "불꺼", "굿나잇",
    "good night", nullptr};

static const char* const kMealStart[] = {
    "밥", "배고파", "배고프", "먹자", "먹을래", "간식", "과자",
    "아침 먹", "아침밥", "점심 먹", "저녁 먹", "맛있", "배불러", "다 먹",
    nullptr};

static const char* const kHygieneStart[] = {
    "손 씻", "손씻", "양치", "화장실", "목욕", "샤워", "잠옷", "갈아입",
    nullptr};

static const char* const kDayStart[] = {
    "안녕", "안녕하세요", "일어났어", "유치원", "어린이집",
    "다녀왔", "다녀오", "잘 잤", "잘잤",
    "좋은 아침", "좋은아침", "굿모닝", "good morning", nullptr};

static const char* const kStoryStart[] = {
    "이야기", "동화", "얘기 해", "이야기 해", "스토리", "이야기 해줘", nullptr};

static const char* const kSongStart[] = {
    "노래", "동요", "불러", "따라 해봐", "따라해봐", "율동", nullptr};

static const char* const kCountStart[] = {
    "숫자", "세어", "세기", "몇 개", "몇개", "하나 둘", "더하기", "빼기",
    "세어볼까", "같이 세", nullptr};

static const char* const kHangulStart[] = {
    "가나다", "한글", "글자", "따라 말", "따라말", nullptr};

static const char* const kEnStart[] = {
    "영어 하자", "영어하자", "영어 공부", "영어공부", "영어놀이", "영어 놀이",
    "영어로", "잉글리시", "english", "English", "영어 게임", nullptr};

static const char* const kZhStart[] = {
    "중국어", "니하오", "你好", "중문", "chinese", "Chinese", nullptr};

static const char* const kRoleStart[] = {
    "병원 놀이", "병원놀이", "가게 놀이", "가게놀이", "역할", "내가 의사",
    "내가 선생님", "소꿉", nullptr};

static const char* const kYes[] = {
    // Avoid bare "어" — it matches inside 싫어/먹어/….
    "응", "네", "응응", "네네", "좋아", "그래", "웅", "ㅇㅇ", "yes", "Yeah", "yeah",
    "오케이", "ok", "OK", nullptr};

static const char* const kNoReject[] = {
    "싫어", "안 해", "안해", "아니야", "싫어해", "하지 마", "하지마", "no", nullptr};

static const char* const kHelp[] = {
    "몰라", "모르겠어", "모르겠어여", "힌트", "알려줘", "도와줘", "어려워", nullptr};

static const char* const kContinue[] = {
    "그다음", "그 다음", "계속", "다음", "그리고", nullptr};

static const char* const kCorrection[] = {
    "그게 아니라", "말고", "아니야 그게", "아니 과일", "아니 그거", "아니,", nullptr};

// Locked activities: topic keywords must not switch away.
inline bool conv_activity_locked(Activity a) {
  return a == ACT_EN || a == ACT_ZH || a == ACT_STORY || a == ACT_COUNT ||
         a == ACT_ROLE || a == ACT_HANGUL || a == ACT_SONG || a == ACT_MEAL ||
         a == ACT_HYGIENE || a == ACT_SLEEP || a == ACT_SAFE;
}

inline Activity conv_detect_explicit_activity(const String& t) {
  // More specific / intentional phrases first. No bare nature→activity.
  if (t == "영어" || chat_text_has_any(t, kEnStart)) return ACT_EN;
  if (chat_text_has_any(t, kZhStart)) return ACT_ZH;
  if (chat_text_has_any(t, kStoryStart)) return ACT_STORY;
  if (chat_text_has_any(t, kSongStart)) return ACT_SONG;
  if (chat_text_has_any(t, kCountStart)) return ACT_COUNT;
  if (chat_text_has_any(t, kHangulStart)) return ACT_HANGUL;
  if (chat_text_has_any(t, kRoleStart)) return ACT_ROLE;
  if (chat_text_has_any(t, kHygieneStart)) return ACT_HYGIENE;
  if (chat_text_has_any(t, kMealStart)) return ACT_MEAL;
  if (chat_text_has_any(t, kDayStart)) return ACT_DAY;
  return ACT_FREE;
}

// Soft topic extract for "X는?" / "그러면 X는?" while staying in activity.
inline bool conv_extract_topic_ellipsis(const String& t, char* out, size_t out_n) {
  String s = t;
  s.trim();
  s.replace("그러면 ", "");
  s.replace("그럼 ", "");
  s.replace("은?", "");
  s.replace("는?", "");
  s.replace("가?", "");
  s.replace("을?", "");
  s.replace("를?", "");
  s.replace("?", "");
  s.replace("영어로", "");
  s.replace("영어", "");
  s.replace("뭐야", "");
  s.trim();
  if (s.length() < 2 || s.length() >= (int)out_n) return false;
  // Reject if still looks like a full sentence.
  if (s.indexOf(' ') >= 0 && s.length() > 12) return false;
  conv_set_str(out, out_n, s.c_str());
  return true;
}

inline bool conv_text_is_mostly(const String& t, const char* const* needles) {
  String s = t;
  s.trim();
  s.replace(".", "");
  s.replace("!", "");
  s.replace("?", "");
  s.replace(" ", "");
  if (s.length() == 0) return false;
  for (size_t i = 0; needles[i]; i++) {
    if (s == needles[i]) return true;
  }
  // Also allow short strings that start with the needle (응응, 네네).
  for (size_t i = 0; needles[i]; i++) {
    if (s.startsWith(needles[i]) && s.length() <= (int)strlen(needles[i]) + 2) return true;
  }
  return false;
}

// Pre-LLM deterministic interpretation. Updates activity/topic/intent; does not expire by turn count.
inline UtterIntent conv_pre_update(ConvState& st, const String& user_text) {
  String t = user_text;
  t.trim();
  st.just_changed_activity = false;
  st.last_intent = UINTENT_OTHER;

  if (t.length() == 0) {
    st.last_intent = UINTENT_AMBIG;
    return st.last_intent;
  }

  if (chat_text_has_any(t, kSafe)) {
    conv_leave_activity(st, ACT_SAFE);
    st.last_intent = UINTENT_ACT_CHANGE;
    return st.last_intent;
  }

  if (chat_text_has_any(t, kExit)) {
    conv_leave_activity(st, ACT_FREE);
    st.last_intent = UINTENT_ACT_CHANGE;
    return st.last_intent;
  }
  // Soft exit from EN/ZH: "한국어로 하자" — but not "apple in Korean?" / "한국어로 뭐야".
  if ((st.activity == ACT_EN || st.activity == ACT_ZH) && chat_text_has(t, "한국어")) {
    bool ask_meaning = chat_text_has(t, "뭐") || chat_text_has(t, "뜻") ||
                       chat_text_has(t, "발음") || chat_text_has(t, "Korean");
    bool switch_lang = chat_text_has(t, "하자") || chat_text_has(t, "할래") ||
                       chat_text_has(t, "놀자") || t.endsWith("로");
    if (ask_meaning) {
      st.last_intent = UINTENT_QUESTION;
      return st.last_intent;
    }
    if (switch_lang || t == "한국어") {
      conv_leave_activity(st, ACT_FREE);
      st.last_intent = UINTENT_ACT_CHANGE;
      return st.last_intent;
    }
  }

  if (chat_text_has_any(t, kSleep)) {
    conv_leave_activity(st, ACT_SLEEP);
    st.last_intent = UINTENT_ACT_CHANGE;
    return st.last_intent;
  }

  // Pending YES/NO (proposal): 응/싫어 are answers, not emotion-mode / new activity.
  if (st.expect == EXPECT_YES_NO) {
    // Exact/mostly match only — substring "어" must not hit 싫어.
    if (conv_text_is_mostly(t, kYes)) {
      st.last_intent = UINTENT_ANSWER;
      st.phase = PHASE_PLAY;
      return st.last_intent;
    }
    if (conv_text_is_mostly(t, kNoReject) || t == "싫어" || t.startsWith("싫어")) {
      st.last_intent = UINTENT_REJECT;
      conv_clear_pending(st);
      // Stay in current activity or free — never jump to EMO for proposal reject.
      if (st.phase == PHASE_OFFER) {
        conv_leave_activity(st, ACT_FREE);
      }
      return st.last_intent;
    }
  }

  // Quiz / vocab help.
  if ((st.expect == EXPECT_WORD || st.expect == EXPECT_NUMBER || st.phase == PHASE_ASK) &&
      chat_text_has_any(t, kHelp)) {
    st.last_intent = UINTENT_HELP;
    st.phase = PHASE_HINT;
    if (st.hint_level < 3) st.hint_level++;
    return st.last_intent;
  }

  // Story continuation.
  if (st.activity == ACT_STORY && chat_text_has_any(t, kContinue)) {
    st.last_intent = UINTENT_CONTINUE;
    st.expect = EXPECT_CONT;
    return st.last_intent;
  }

  // "아니 네가 해" is handing the quiz back — not a vocab correction.
  if ((st.activity == ACT_EN || st.activity == ACT_ZH) && conv_child_hands_quiz_back(t)) {
    st.last_intent = UINTENT_OTHER;
    st.expect = EXPECT_OPEN;
    st.phase = PHASE_PLAY;
    return st.last_intent;
  }

  // Correction within current activity (배 → 과일 배).
  if (chat_text_has_any(t, kCorrection)) {
    st.last_intent = UINTENT_CORRECTION;
    return st.last_intent;
  }

  // Distress → emo (only clear distress words, not "싫어" alone).
  if (!conv_activity_locked(st.activity) && chat_text_has_any(t, kEmoDistress)) {
    conv_leave_activity(st, ACT_EMO);
    st.last_intent = UINTENT_ACT_CHANGE;
    return st.last_intent;
  }

  // Explicit activity change — only when not answering a pending quiz with a word that
  // happens to contain a trigger substring, or when child clearly requests a new frame.
  Activity want = conv_detect_explicit_activity(t);
  if (want != ACT_FREE) {
    // Inside locked activity: only switch on strong explicit request phrases.
    if (conv_activity_locked(st.activity) && want != st.activity) {
      bool strong =
          chat_text_has(t, "하자") || chat_text_has(t, "할래") || chat_text_has(t, "해줘") ||
          chat_text_has(t, "놀이") || chat_text_has(t, "게임") || chat_text_has(t, "공부");
      // Topic ellipsis "강아지는?" while in EN must NOT become nature/free via 강아지.
      bool ellipsis = (t.endsWith("?") || t.endsWith("？") || t.endsWith("는") ||
                       t.endsWith("은") || chat_text_has(t, "그러면") || chat_text_has(t, "그럼"));
      if (st.activity == ACT_EN || st.activity == ACT_ZH || st.activity == ACT_COUNT) {
        char topic_buf[CONV_TOPIC_LEN];
        if (ellipsis && conv_extract_topic_ellipsis(t, topic_buf, sizeof(topic_buf))) {
          conv_set_str(st.topic, sizeof(st.topic), topic_buf);
          st.last_intent = UINTENT_QUESTION;
          st.phase = PHASE_ASK;
          st.expect = (st.activity == ACT_COUNT) ? EXPECT_NUMBER : EXPECT_WORD;
          Serial.printf("act: keep %s topic=%s\n", activity_name(st.activity), st.topic);
          return st.last_intent;
        }
      }
      if (!strong) {
        st.last_intent = UINTENT_OTHER;
        return st.last_intent;
      }
    }
    if (want != st.activity) {
      conv_leave_activity(st, want);
      st.last_intent = UINTENT_ACT_CHANGE;
      if (want == ACT_STORY) st.expect = EXPECT_CONT;
      if (want == ACT_EN || want == ACT_ZH) {
        st.phase = PHASE_PLAY;
        st.expect = EXPECT_OPEN;
      }
      return st.last_intent;
    }
  }

  // Ellipsis topic update while staying in EN/ZH/COUNT.
  if (conv_activity_locked(st.activity) &&
      (st.activity == ACT_EN || st.activity == ACT_ZH || st.activity == ACT_COUNT)) {
    char topic_buf[CONV_TOPIC_LEN];
    if ((chat_text_has(t, "그러면") || chat_text_has(t, "그럼") || t.endsWith("?") ||
         t.endsWith("는?") || t.endsWith("은?")) &&
        conv_extract_topic_ellipsis(t, topic_buf, sizeof(topic_buf))) {
      conv_set_str(st.topic, sizeof(st.topic), topic_buf);
      st.last_intent = UINTENT_QUESTION;
      st.phase = PHASE_ASK;
      st.expect = (st.activity == ACT_COUNT) ? EXPECT_NUMBER : EXPECT_WORD;
      return st.last_intent;
    }
  }

  // Short yes after open offer in free.
  if (st.expect == EXPECT_NONE && conv_text_is_mostly(t, kYes)) {
    st.last_intent = UINTENT_ANSWER;
    return st.last_intent;
  }

  if (t.length() <= 2 && !chat_text_has_any(t, kYes)) {
    st.last_intent = UINTENT_AMBIG;
    return st.last_intent;
  }

  st.last_intent = UINTENT_OTHER;
  return st.last_intent;
}

inline void conv_sync_legacy(const ConvState& st, ChatModeState& legacy) {
  legacy.mode = st.activity;
  legacy.sticky = 0;
  legacy.just_entered = st.just_changed_activity;
}

inline void chat_mode_enter(ChatModeState& legacy, ChatMode next) {
  // Used by SAFE canned path — map onto a temporary view only.
  legacy.just_entered = (legacy.mode != next);
  legacy.mode = next;
  legacy.sticky = 0;
}

inline String conv_state_prompt_line(const ConvState& st) {
  String s = " [상태 activity=";
  s += activity_name(st.activity);
  s += " topic=";
  s += st.topic[0] ? st.topic : "-";
  s += " expect=";
  s += expect_name(st.expect);
  s += " phase=";
  s += (st.phase == PHASE_OFFER)   ? "offer"
       : (st.phase == PHASE_ASK)   ? "ask"
       : (st.phase == PHASE_HINT)  ? "hint"
       : (st.phase == PHASE_PLAY)  ? "play"
       : (st.phase == PHASE_DONE)  ? "done"
                                   : "idle";
  s += " hint=";
  s += String(st.hint_level);
  s += " intent=";
  switch (st.last_intent) {
    case UINTENT_ANSWER: s += "answer"; break;
    case UINTENT_QUESTION: s += "question"; break;
    case UINTENT_CORRECTION: s += "correction"; break;
    case UINTENT_REJECT: s += "reject"; break;
    case UINTENT_HELP: s += "help"; break;
    case UINTENT_CONTINUE: s += "continue"; break;
    case UINTENT_ACT_CHANGE: s += "act_change"; break;
    case UINTENT_AMBIG: s += "ambig"; break;
    default: s += "other"; break;
  }
  if (st.learn_item[0]) {
    s += " item=";
    s += st.learn_item;
  }
  if (st.expect_ans[0]) {
    s += " ans=";
    s += st.expect_ans;
  }
  if (st.fact[0]) {
    s += " fact=";
    s += st.fact;
  }
  s += " clarify=";
  s += String(st.clarify_fails);
  s += "] ";
  return s;
}

inline const char* chat_mode_overlay(ChatMode m) {
  switch (m) {
    case ACT_FREE:
      return " [활동:자유] 짧게 받아쳐. 직전 말에 이어가. "
             "같은 제안을 매 턴 반복하지 마. 질문은 필요할 때만 하나. "
             "아이 소재를 이어가. 모드 이름·메뉴 금지.";
    case ACT_DAY:
      return " [활동:인사] 오늘 한 장면만. 하루 전체를 평가하지 마.";
    case ACT_MEAL:
      return " [활동:식사놀이] 같이 먹는 상상. 잔소리·강요 금지. 한두 짧은 문장.";
    case ACT_HYGIENE:
      return " [활동:손씻기] 한 동작만. 예: '거품 퐁퐁!'";
    case ACT_EMO:
      return " [활동:감정] 먼저 공감. 설교 금지. 질문 필요하면 하나만.";
    case ACT_STORY:
      return " [활동:이야기] 한 장면(문장 하나)만 말하고 멈춰. "
             "'그다음' 전에 이어가지 마. 놀이 제안으로 새지 마.";
    case ACT_SONG:
      return " [활동:노래] 한 소절만.";
    case ACT_COUNT:
      return " [활동:세기] 같이 세기. 틀리면 힌트. 시험처럼 몰아붙이지 마.";
    case ACT_HANGUL:
      return " [활동:한글] 짧은 따라 말하기. 문법 강의 금지.";
    case ACT_EN:
      return " [활동:영어놀이] 한 턴에 영어 단어 하나. "
             "아이가 한글 단어를 말하면 영어 한 마디만. 아이가 영어를 말하면 한글 뜻 한 마디만. "
             "'문제 내'면 한글 단어 하나만 물어(예: 사과는?). "
             "보기 나열 금지. Apple, Train, Boat, Dog 를 한꺼번에 말하지 마. "
             "예시에 나온 단어를 문제로 다시 쓰지 마. 괄호·발음기호 금지. "
             "'아니 네가 해'면 영어 정답을 말하지 마. 출제를 넘기거나 한글 단어 하나만 물어. "
             "강아지·사과는 topic일 뿐 활동을 바꾸지 마.";
    case ACT_ZH:
      return " [활동:중국어놀이] 짧은 중국어 단어/구. 고치면 고친 뜻으로.";
    case ACT_ROLE:
      return " [활동:역할] 아이 역할에 맞춰 상대 역할만. 한 장면.";
    case ACT_SLEEP:
      return " [활동:잠] 아주 짧게 차분히. 새 놀이 제안 금지.";
    case ACT_SAFE:
      return " [활동:안전] 짧게 거절하고 놀이로.";
  }
  return "";
}

inline const char* CONV_ENVELOPE_RULES =
    "매 응답은 반드시 메타로 시작해: "
    "{{a=ACT;t=TOPIC;e=EXPECT;p=PHASE;h=0-3;d=DACT;x=ANS;f=FACT}}"
    "바로 뒤에 아이가 들을 말만 써. 메타·JSON·필드 이름을 말로 읽지 마. "
    "ACT: free|day|meal|hygiene|story|song|count|hangul|en|zh|role|sleep|emo "
    "EXPECT: none|yn|word|num|cont|open|choice "
    "PHASE: idle|offer|play|ask|hint|done "
    "DACT: ans|ack|cont|hint|clar|choice|corr|chg|wait|ask "
    "TOPIC/ANS/FACT는 짧게(한글·영문 단어). 없으면 비워(t=;x=;f=). "
    "활동을 바꿀 때만 ACT를 바꾸고, 주제만 바뀌면 t만 바꿔. "
    "아이가 거부(intent=reject)면 다른 걸 강요하지 마. "
    "intent=help 이면 정답을 바로 말하지 말고 짧은 힌트(d=hint,h 증가). "
    "intent=ambig 이고 clarify가 2 이상이면 '응?' 대신 짧은 선택 둘. "
    "choice로 영어 단어 네 개를 나열하지 마. "
    "항상 짧은 문장 딱 하나. 두 문장 이상 금지. 질문은 턴당 최대 하나. "
    "매 턴 질문으로 끝내지 마. 아이 말을 메아리치지 마. "
    "잘했어를 남발하지 말고 구체적 반응. 질문에는 먼저 답해. "
    "못 들은 경험·눈에 보이는 걸 꾸며내지 마. "
    "알아듣기 힘든 음절·오인식처럼 보이는 말은 뜻을 지어내지 마. 한 번만 짧게 되물어. "
    "사전처럼 설명하지 마. e=word 는 영어·중국어·세기·한글 놀이 퀴즈일 때만. "
    "메타({{…}})·필드 이름(a,e,ambig)을 절대 입으로 말하지 마.";

inline bool chat_mode_canned_reply(ChatMode m, const String& /*user_text*/, String& out) {
  if (m != ACT_SAFE) return false;
  out = "그건 디노랑 안 놀아. 같이 다른 거 하자.";
  return true;
}

inline uint16_t chat_mode_max_tokens(ChatMode m) {
  // Toy: one short sentence. gpt-oss still spends some budget on reasoning.
  switch (m) {
    case ACT_SLEEP:
    case ACT_SAFE:
      return 140;
    case ACT_HYGIENE:
    case ACT_SONG:
    case ACT_EN:
    case ACT_ZH:
      return 160;
    default:
      return 150;
  }
}

inline float chat_mode_temperature(ChatMode m) {
  switch (m) {
    case ACT_SLEEP:
    case ACT_SAFE:
      return 0.30f;
    case ACT_EMO:
    case ACT_MEAL:
      return 0.40f;
    case ACT_EN:
    case ACT_ZH:
    case ACT_COUNT:
    case ACT_HANGUL:
      return 0.35f;
    default:
      return 0.45f;
  }
}

inline void chat_mode_tts_voice(ChatMode /*m*/, const char** lang, const char** voice) {
  *lang = nullptr;
  *voice = nullptr;
}

// Parse {{a=en;t=dog;e=word;p=ask;h=1;d=hint;x=Apple;f=}}speak…
inline bool conv_parse_activity(const String& v, Activity& out) {
  if (v == "free" || v == "play") out = ACT_FREE;
  else if (v == "day") out = ACT_DAY;
  else if (v == "meal") out = ACT_MEAL;
  else if (v == "hygiene") out = ACT_HYGIENE;
  else if (v == "story") out = ACT_STORY;
  else if (v == "song") out = ACT_SONG;
  else if (v == "count" || v == "number") out = ACT_COUNT;
  else if (v == "hangul") out = ACT_HANGUL;
  else if (v == "en") out = ACT_EN;
  else if (v == "zh") out = ACT_ZH;
  else if (v == "role") out = ACT_ROLE;
  else if (v == "sleep") out = ACT_SLEEP;
  else if (v == "emo") out = ACT_EMO;
  else if (v == "safe") out = ACT_SAFE;
  else return false;
  return true;
}

inline bool conv_parse_expect(const String& v, ExpectType& out) {
  if (v.length() == 0 || v == "none" || v == "-" || v == "ambig" || v == "clarify") {
    out = EXPECT_NONE;
  } else if (v == "yn") out = EXPECT_YES_NO;
  else if (v == "word") out = EXPECT_WORD;
  else if (v == "num") out = EXPECT_NUMBER;
  else if (v == "cont") out = EXPECT_CONT;
  else if (v == "open") out = EXPECT_OPEN;
  else if (v == "choice") out = EXPECT_CHOICE;
  else {
    out = EXPECT_NONE;  // unknown (model invents e=ambig) — do not fail the envelope
  }
  return true;
}

inline bool conv_parse_phase(const String& v, ActivityPhase& out) {
  if (v.length() == 0 || v == "idle") out = PHASE_IDLE;
  else if (v == "offer") out = PHASE_OFFER;
  else if (v == "play") out = PHASE_PLAY;
  else if (v == "ask") out = PHASE_ASK;
  else if (v == "hint") out = PHASE_HINT;
  else if (v == "done") out = PHASE_DONE;
  else return false;
  return true;
}

inline bool conv_parse_dact(const String& v, DialogueAct& out) {
  if (v == "ans") out = DACT_ANSWER;
  else if (v == "ack") out = DACT_ACK;
  else if (v == "cont") out = DACT_CONTINUE;
  else if (v == "hint") out = DACT_HINT;
  else if (v == "clar") out = DACT_CLARIFY;
  else if (v == "choice") out = DACT_CHOICE;
  else if (v == "corr") out = DACT_CORRECT;
  else if (v == "chg") out = DACT_CHANGE;
  else if (v == "wait") out = DACT_WAIT;
  else if (v == "ask") out = DACT_ASK;
  else return false;
  return true;
}

inline String conv_field(const String& body, const char* key) {
  // Require key at start or after ';' so "ph=offer" does not match key "h".
  String k = String(key) + "=";
  int p = -1;
  if (body.startsWith(k)) {
    p = 0;
  } else {
    String needle = String(";") + k;
    int at = body.indexOf(needle);
    if (at >= 0) p = at + 1;
  }
  if (p < 0) return "";
  p += k.length();
  int end = body.indexOf(';', p);
  if (end < 0) end = body.length();
  String v = body.substring(p, end);
  v.trim();
  return v;
}

inline String conv_strip_envelope(const String& full) {
  int start = full.indexOf("{{");
  int end = full.indexOf("}}");
  if (start >= 0 && end > start) {
    String s = full.substring(end + 2);
    s.trim();
    int junk = s.indexOf("{{");
    if (junk >= 0) s = s.substring(0, junk);
    s.trim();
    return s;  // never return the {{…}} markup itself
  }
  if (start >= 0) return "";  // unclosed meta — do not speak it
  return full;
}

inline bool conv_expect_ok_for_activity(Activity a, ExpectType e) {
  if (e != EXPECT_WORD && e != EXPECT_NUMBER) return true;
  return a == ACT_EN || a == ACT_ZH || a == ACT_COUNT || a == ACT_HANGUL;
}

// Returns true if envelope applied. speak_out = text after }} (never the meta).
inline bool conv_apply_envelope(ConvState& st, const String& full, String& speak_out) {
  speak_out = conv_strip_envelope(full);
  int start = full.indexOf("{{");
  int end = full.indexOf("}}");
  if (start < 0 || end < 0 || end < start + 2) {
    return false;
  }
  String body = full.substring(start + 2, end);
  body.trim();

  Activity a = st.activity;
  ExpectType e = st.expect;
  ActivityPhase ph = st.phase;
  DialogueAct d = st.last_act;
  bool ok_a = true, ok_p = true, ok_d = true;

  String va = conv_field(body, "a");
  String vt = conv_field(body, "t");
  String ve = conv_field(body, "e");
  String vp = conv_field(body, "p");
  String vh = conv_field(body, "h");
  String vd = conv_field(body, "d");
  String vx = conv_field(body, "x");
  String vf = conv_field(body, "f");

  if (va.length()) ok_a = conv_parse_activity(va, a);
  if (ve.length()) conv_parse_expect(ve, e);
  if (vp.length()) ok_p = conv_parse_phase(vp, ph);
  if (vd.length()) ok_d = conv_parse_dact(vd, d);
  if (!ok_a || !ok_p || !ok_d) {
    Serial.println("conv: envelope invalid — state unchanged");
    return false;
  }
  if (!conv_expect_ok_for_activity(a, e)) e = EXPECT_NONE;

  if (a != st.activity) {
    // Model must not silently drop a locked activity unless it marks d=chg (or done).
    bool drop_locked = conv_activity_locked(st.activity) && a == ACT_FREE && d != DACT_CHANGE;
    if (!drop_locked) {
      conv_leave_activity(st, a);
    }
  }
  if (vt.length() && vt != "-" && vt != "topic" && vt != "TOPIC") {
    conv_set_str(st.topic, sizeof(st.topic), vt.c_str());
  }
  st.expect = e;
  if (!conv_expect_ok_for_activity(st.activity, st.expect)) st.expect = EXPECT_NONE;
  st.phase = ph;
  st.last_act = d;
  if (vh.length()) {
    int h = vh.toInt();
    if (h < 0) h = 0;
    if (h > 3) h = 3;
    st.hint_level = (uint8_t)h;
  }
  if (vx.length() && vx != "-") conv_set_str(st.expect_ans, sizeof(st.expect_ans), vx.c_str());
  if (vf.length() && vf != "-") conv_set_str(st.fact, sizeof(st.fact), vf.c_str());

  if (d == DACT_ASK || d == DACT_CHOICE || d == DACT_CLARIFY) {
    if (st.recent_q_count < 7) st.recent_q_count++;
  } else if (st.recent_q_count > 0) {
    st.recent_q_count--;
  }

  if (d == DACT_CLARIFY) {
    if (st.clarify_fails < CONV_CLARIFY_MAX) st.clarify_fails++;
  } else if (d != DACT_WAIT) {
    st.clarify_fails = 0;
  }

  if (ph == PHASE_DONE) {
    conv_leave_activity(st, ACT_FREE);
  }

  speak_out = conv_strip_envelope(full);
  Serial.printf("conv: a=%s t=%s e=%s p=%u h=%u d=%s\n", activity_name(st.activity),
                st.topic[0] ? st.topic : "-", expect_name(st.expect), (unsigned)st.phase,
                (unsigned)st.hint_level, dact_name(st.last_act));
  return true;
}
