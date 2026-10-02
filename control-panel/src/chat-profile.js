/**
 * Chatbot attribute catalog + persona_extra compiler.
 * Shared by Worker API (and mirrored choices in the control-panel UI).
 */

export const CHAT_PROFILE_VERSION = 1;

export const CHARACTER_KINDS = [
  { id: "baby_trex", label: "아기 티라노", prompt: "부드러운 아기 티라노사우루스 인형" },
  { id: "bunny", label: "토끼", prompt: "부드러운 토끼 인형" },
  { id: "bear", label: "곰돌이", prompt: "포근한 곰 인형" },
  { id: "puppy", label: "강아지", prompt: "귀여운 강아지 인형" },
  { id: "cat", label: "고양이", prompt: "부드러운 고양이 인형" },
  { id: "robot_friend", label: "로봇 친구", prompt: "다정한 장난감 로봇 친구(진짜 로봇·AI라고 하지 마)" },
  { id: "custom", label: "직접(이름만)", prompt: "다정한 인형 친구" },
];

export const AGE_BANDS = [
  { id: "3-4", label: "3–4세", prompt: "아주 짧은 말, 쉬운 단어" },
  { id: "5-7", label: "5–7세", prompt: "짧은 구어체, 아이 눈높이" },
];

export const PURPOSES = [
  { id: "play_friend", label: "놀이 친구", prompt: "같이 노는 친구" },
  { id: "learning_buddy", label: "배움 놀이", prompt: "가르치지 말고 같이 알아보는 놀이 친구" },
  { id: "language_focus", label: "말·외국어", prompt: "말놀이·짧은 외국어 놀이를 즐기는 친구" },
  { id: "daily_routine", label: "일과 도우미", prompt: "밥·양치·잠 같은 일과를 놀이로 돕는 친구" },
  { id: "bedtime", label: "잠자리", prompt: "잠들기 전 차분히 옆에 있어 주는 친구" },
];

export const TONES = [
  { id: "warm_short", label: "다정·짧게", prompt: "다정하고 짧게" },
  { id: "playful", label: "장난·활발", prompt: "장난스럽고 활발하게" },
  { id: "calm", label: "차분·포근", prompt: "차분하고 포근하게" },
];

export const TURN_STYLES = [
  { id: "one_beat", label: "한 턴 한 박 (권장)", prompt: "한 턴에 한 박만. 기본 한 문장, 최대 둘. 세 문장 금지" },
  { id: "soft_two", label: "가끔 두 문장", prompt: "대개 한 문장, 필요할 때만 둘" },
];

export const QUESTION_RATES = [
  { id: "rare", label: "거의 안 물어봄", prompt: "질문은 드물게" },
  { id: "sometimes", label: "가끔 (권장)", prompt: "질문은 세 번에 한 번 정도" },
  { id: "often", label: "자주 되묻기", prompt: "대화를 잇기 위해 가볍게 자주 되물어" },
];

/** Categories from docs/conversation-scenarios.md (Z safety is always on). */
export const CATEGORIES = [
  { id: "play", label: "놀이 수다", prompt: "놀이 수다" },
  { id: "day", label: "인사·하루", prompt: "인사·하루" },
  { id: "meal", label: "식사", prompt: "식사 놀이" },
  { id: "hygiene", label: "손씻기·양치", prompt: "손씻기·양치 놀이" },
  { id: "emo", label: "감정", prompt: "감정 듣기" },
  { id: "story", label: "이야기", prompt: "짧은 이야기" },
  { id: "song", label: "노래", prompt: "노래·율동" },
  { id: "number", label: "숫자", prompt: "숫자 세기" },
  { id: "hangul", label: "한글", prompt: "한글 말놀이" },
  { id: "en", label: "영어", prompt: "짧은 영어 놀이" },
  { id: "zh", label: "중국어", prompt: "짧은 중국어 놀이" },
  { id: "nature", label: "자연", prompt: "날씨·동물 관찰" },
  { id: "role", label: "역할놀이", prompt: "역할놀이" },
  { id: "sleep", label: "잠자리", prompt: "잠자리" },
];

export const DEFAULT_PROFILE = {
  v: CHAT_PROFILE_VERSION,
  character_name: "디노",
  character_kind: "baby_trex",
  age_band: "5-7",
  purpose: "play_friend",
  tone: "warm_short",
  turn_style: "one_beat",
  question_rate: "sometimes",
  categories: ["play", "day", "meal", "hygiene", "emo", "story", "song", "number", "hangul", "en", "nature", "role", "sleep"],
  custom_notes: "",
};

function pick(list, id, fallbackId) {
  return list.find((x) => x.id === id) || list.find((x) => x.id === fallbackId) || list[0];
}

export function normalizeProfile(raw) {
  const src = raw && typeof raw === "object" ? raw : {};
  const name = String(src.character_name || DEFAULT_PROFILE.character_name)
    .trim()
    .slice(0, 20) || "디노";
  const kind = pick(CHARACTER_KINDS, src.character_kind, DEFAULT_PROFILE.character_kind).id;
  const age = pick(AGE_BANDS, src.age_band, DEFAULT_PROFILE.age_band).id;
  const purpose = pick(PURPOSES, src.purpose, DEFAULT_PROFILE.purpose).id;
  const tone = pick(TONES, src.tone, DEFAULT_PROFILE.tone).id;
  const turn = pick(TURN_STYLES, src.turn_style, DEFAULT_PROFILE.turn_style).id;
  const q = pick(QUESTION_RATES, src.question_rate, DEFAULT_PROFILE.question_rate).id;
  const allowed = new Set(CATEGORIES.map((c) => c.id));
  let cats = Array.isArray(src.categories)
    ? src.categories.map(String).filter((id) => allowed.has(id))
    : [...DEFAULT_PROFILE.categories];
  if (!cats.includes("play")) cats = ["play", ...cats];
  cats = [...new Set(cats)];
  const custom_notes = String(src.custom_notes || "").trim().slice(0, 120);
  return {
    v: CHAT_PROFILE_VERSION,
    character_name: name,
    character_kind: kind,
    age_band: age,
    purpose: purpose,
    tone: tone,
    turn_style: turn,
    question_rate: q,
    categories: cats,
    custom_notes,
  };
}

export function parseProfileJson(text) {
  if (!text || typeof text !== "string" || !text.trim()) return normalizeProfile(DEFAULT_PROFILE);
  try {
    return normalizeProfile(JSON.parse(text));
  } catch {
    return normalizeProfile(DEFAULT_PROFILE);
  }
}

/** Compile structured profile → Korean instructions for doll persona_extra (≤360). */
export function compilePersonaExtra(profile, maxLen = 360) {
  const p = normalizeProfile(profile);
  const kind = pick(CHARACTER_KINDS, p.character_kind);
  const age = pick(AGE_BANDS, p.age_band);
  const purpose = pick(PURPOSES, p.purpose);
  const tone = pick(TONES, p.tone);
  const turn = pick(TURN_STYLES, p.turn_style);
  const q = pick(QUESTION_RATES, p.question_rate);

  const enabled = new Set(p.categories);
  const on = CATEGORIES.filter((c) => enabled.has(c.id)).map((c) => c.prompt);
  const off = CATEGORIES.filter((c) => !enabled.has(c.id)).map((c) => c.prompt);

  const parts = [
    `이름은 ${p.character_name}. ${kind.prompt}이야.`,
    `목적: ${purpose.prompt}.`,
    `말투: ${tone.prompt}. ${age.prompt}.`,
    `${turn.prompt}. ${q.prompt}.`,
  ];
  if (on.length) parts.push(`환영 놀이: ${on.join("·")}.`);
  if (off.length) parts.push(`하지 마: ${off.join("·")}.`);
  parts.push("메뉴처럼 고르라고 말하지 마. 아이 말에 맞춰 짧게 받아쳐.");
  if (p.custom_notes) parts.push(p.custom_notes);

  let out = parts.join(" ");
  if (out.length > maxLen) out = out.slice(0, maxLen);
  return out;
}

export function catalogPayload() {
  return {
    version: CHAT_PROFILE_VERSION,
    defaults: DEFAULT_PROFILE,
    character_kinds: CHARACTER_KINDS.map(({ id, label }) => ({ id, label })),
    age_bands: AGE_BANDS.map(({ id, label }) => ({ id, label })),
    purposes: PURPOSES.map(({ id, label }) => ({ id, label })),
    tones: TONES.map(({ id, label }) => ({ id, label })),
    turn_styles: TURN_STYLES.map(({ id, label }) => ({ id, label })),
    question_rates: QUESTION_RATES.map(({ id, label }) => ({ id, label })),
    categories: CATEGORIES.map(({ id, label }) => ({ id, label })),
  };
}

export function defaultBootPhrase(profile) {
  const p = normalizeProfile(profile);
  return `안녕, 난 ${p.character_name}야! 같이 놀자.`.slice(0, 90);
}
