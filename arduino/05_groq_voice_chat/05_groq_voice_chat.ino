/**
 * talkbot: Groq STT -> LLM -> Google TTS 음성 챗봇 (ESP32-S3 + ReSpeaker Lite)
 *
 * I2S 마이크 녹음 -> Groq Whisper STT(ko) -> Gemini Flash-Lite -> Google Cloud TTS(ko 여성) -> I2S
 *
 * 사전 준비:
 *   1. .env 에 GROQ_API, GOOGLE_API 설정 (WIFI_* 는 선택 — 없어도 AP 포털로 설정)
 *   2. ./scripts/sync_secrets.sh
 *   3. Cloud Console: Text-to-Speech + Generative Language API (Gemini)
 *   4. ./scripts/upload_sketch.sh 05_groq_voice_chat
 *
 * Wi-Fi: 부팅 시 SoftAP "Talkbot-Setup" + http://192.168.4.1
 *        최근 성공 SSID는 NVS에 저장되어 자동접속한다.
 */

#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "mbedtls/base64.h"
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Mini.h"
#include "Wire.h"
#include "wifi_portal.h"
#include "tts_stream.h"
#include "chat_modes.h"
#include "device_config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Run ./scripts/sync_secrets.sh after setting .env"
#endif

#ifndef CONTROL_PANEL_URL
#define CONTROL_PANEL_URL "https://talkbot-control-panel.nine-raptorex.workers.dev"
#endif

#define AIC3204_ADDR 0x18
#define XMOS_ADDR 0x42

static const char* GROQ_HOST = "api.groq.com";
static const uint16_t GROQ_PORT = 443;
static const char* GEMINI_HOST = "generativelanguage.googleapis.com";
static const uint16_t GEMINI_PORT = 443;
static const char* GOOGLE_TTS_HOST = "texttospeech.googleapis.com";
static const uint16_t GOOGLE_TTS_PORT = 443;

static const char* STT_MODEL = "whisper-large-v3-turbo";
// Google AI Studio free Flash-Lite. Same GOOGLE_API_KEY as Chirp3 TTS.
static const char* LLM_MODEL = "gemini-2.5-flash-lite";
static const char* GROQ_LLM_MODEL = "openai/gpt-oss-20b";
static const char* GROQ_LLM_REASONING = "low";
static const char* TTS_MODEL = "canopylabs/orpheus-v1-english";  // English fallback only
static const char* TTS_VOICE = "hannah";
static const char* TTS_FALLBACK_VOICE = "Brian";
static const char* STT_LANGUAGE = "ko";
static const char* GOOGLE_TTS_LANG = "ko-KR";
// Chirp3 HD — natural conversational; plain text (limited SSML). No pitch lift.
static const char* GOOGLE_TTS_VOICE = "ko-KR-Chirp3-HD-Kore";
static const bool TTS_BOOT_TEST = true;
// UTF-8 byte cap for toy turns (~60 Hangul); truncate_utf8 uses bytes not chars
static const size_t TTS_MAX_CHARS = 180;

static const size_t SAMPLE_RATE = 16000;       // mic + I2S device rate
static const uint32_t TTS_SAMPLE_RATE = 16000; // match device rate — no resample, less bandwidth
// Toy: short capture window. 5s ≈ 160KB PCM.
static const size_t MAX_RECORD_SEC = 5;
static const size_t MAX_SAMPLES = SAMPLE_RATE * MAX_RECORD_SEC;

// Adaptive energy VAD — close-talk doll: ignore room hiss / speaker echo
// (rms ~0.03–0.06). Near-mic child speech typically onsets ≥0.09.
static const float VAD_ABS_MIN = 0.050f;
static const float VAD_ONSET_SNR = 4.2f;
static const float VAD_ONSET_ABS_MIN = 0.072f;   // near-mic onset; blocks ambient
static const float VAD_ONSET_ABS_MAX = 0.130f;   // still rise when nf is high
static const float VAD_END_SNR = 1.8f;
static const float SPEECH_MIN_SNR = 2.8f;
static const float SPEECH_PEAK_SNR = 4.2f;
static const float SPEECH_ABS_MIN = 0.040f;
static const float SPEECH_PEAK_ABS = 0.072f;
static const float SPEECH_PEAK_ABS_MAX = 0.150f;
static const float SPEECH_AVG_ABS_CAP = 0.090f;  // cap only for very loud rooms
static const float SPEECH_MOD_MIN = 0.24f;
static const float ZCR_SPEECH_MIN = 0.04f;
static const float ZCR_SPEECH_MAX = 0.28f;
static const float NOISE_FLOOR_MIN = 0.0035f;
static const float NOISE_FLOOR_MAX = 0.028f;
static const float NOISE_EMA_FAST = 0.12f;
static const float NOISE_EMA_SLOW = 0.03f;
// End-of-utterance: short pauses for toy turn-taking.
static const uint32_t SILENCE_MS_SHORT = 450;
static const uint32_t SILENCE_MS_LONG = 550;
static const uint32_t SILENCE_ADAPT_AFTER_MS = 700;
// Ignore brief RMS spikes while already in silence (keyboard / AC blips).
static const uint32_t SILENCE_BLIP_IGNORE_MS = 100;
// If TTFT is slow, play a short filler; never block POST→SSE on opener.
static const uint32_t OPENER_DEFER_MS = 700;
static const uint32_t MAX_WAIT_SPEECH_MS = 4000;
static const size_t VAD_START_SAMPLES = 640;       // 40 ms-ish windows × hits
static const size_t VAD_END_SAMPLES = 320;
static const size_t VAD_START_HITS = 6;            // ~240 ms sustained near-mic
static const size_t NOISE_CALIB_SAMPLES = SAMPLE_RATE / 4;  // 250 ms
// Min duration: one-word still ok; block 0.3s hiss blips.
static const size_t MIN_SPEECH_SAMPLES = SAMPLE_RATE * 8 / 25;      // 0.32 s
static const size_t MIN_SPEECH_SAMPLES_FULL = SAMPLE_RATE / 2;      // 0.50 s for looser energy
static const size_t SPEECH_PEAK_WINDOWS_MIN = 3;
static const size_t SPEECH_VOICED_WINDOWS_MIN = 2;
static const size_t SPEECH_PEAK_WINDOWS_MIN_SHORT = 2;
static const size_t SPEECH_VOICED_WINDOWS_MIN_SHORT = 2;
static const size_t PREROLL_SAMPLES = SAMPLE_RATE / 4;  // 250 ms kept before onset
static const uint32_t POST_PLAY_FLUSH_MS = 900;    // drop mic echo after speaker off
static const uint32_t POST_PLAY_PAUSE_MS = 550;    // ignore mic briefly after TTS
static const uint32_t POST_TEXT_PAUSE_MS = 2500;   // ignore mic after serial text cmd
static const uint32_t SERIAL_TYPING_PAUSE_MS = 800; // extend pause while serial chars arrive
static const uint32_t TAIL_KEEP_MS = 80;           // trailing silence kept in upload
// Cooldown only when Groq returns 429 (see note_stt_rate_limit / stt_blocked_until_ms).
static const uint32_t STT_DAILY_LIMIT_BACKOFF_MS = 4UL * 3600UL * 1000UL;
// Zombie TLS sockets often still report connected(); refresh before reuse.
static const uint32_t TLS_STALE_REFRESH_MS = 6000;

static float noise_floor_rms = 0.012f;
static float last_onset_peak_rms = 0.0f;

static uint32_t stt_last_call_ms = 0;
static uint32_t stt_blocked_until_ms = 0;
static uint32_t stt_call_count = 0;

static const char* SYSTEM_PROMPT =
    "너는 디노야. 부드러운 아기 티라노사우루스 인형이고, 4~8살 아이 한 명과 단둘이 놀고 있어. "
    "AI·로봇·앱·검색기가 아니야. 인형 친구야. "
    "세계는 집·놀이방 같은 안전한 일상이야. 무섭고 잔인한 말은 부드럽게 바꿔. "
    "직전 대화와 [상태]를 보고 아이 말을 해석해: "
    "대기 중 질문에 대한 답 / 새 질문 / 교정 / 제안 거부 / 힌트·도움 / 이어가기 / 활동 바꾸기 / 애매함. "
    "'응'은 직전 제안·질문에 대한 수락일 수 있어. '몰라'는 퀴즈에서 힌트 요청이야. "
    "'싫어'는 제안 거부이지 감정 상담 모드가 아니야. "
    "'아니, 과일 배'는 같은 활동 안에서 뜻을 고치는 거야. "
    "'그다음?'은 이야기를 이어가. 활동(영어놀이 등)과 주제(강아지)를 섞지 마. "
    "듣지 않은 일·눈에 보이는 것·같이 안 한 경험을 꾸며내지 마. "
    "말이 애매하면 한 번만 짧게 되묻고, 같은 '응?'을 반복하지 마. 두 번 실패하면 선택지 둘. "
    "항상 짧은 문장 딱 하나. 두 문장 이상·나열·강의 금지. "
    "질문은 턴당 최대 하나. 매 턴 질문으로 끝내지 마. "
    "아이 말을 메아리치지 마. '잘했어!'만 반복하지 말고 구체적 반응. "
    "아이 질문에는 먼저 답한 뒤, 필요할 때만 제안을 해. 거부와 그만은 존중해. "
    "알아듣기 힘든 음절은 뜻을 지어내지 마. 사전처럼 설명하지 마. "
    "이야기 한 장면만 말하고 멈춰. "
    "기본은 한국어. 영어·중국어 놀이일 때만 짧은 외국어 단어 허용. "
    "마크다운·이모지·물결표(~)·특수기호·메타 설명 금지. "
    "웃을 땐 '하하' '히히'. 감탄사만 보내지 마. "
    "모드 이름·메뉴를 말하지 마.";

I2SStream i2s;
AudioInfo speaker_info(SAMPLE_RATE, 2, 32);

int16_t* record_buf = nullptr;
size_t record_count = 0;

WiFiClientSecure* secure_client = nullptr;
WiFiClientSecure* tts_client = nullptr;  // 2nd TLS: TTS overlaps LLM stream drain
// Ring-buffer history (avoid ArduinoJson wipe/OOM on serialize round-trips).
static const uint8_t CHAT_HIST_MAX = 20;
struct ChatHistMsg {
  char role;  // 'u' user / 'a' assistant
  String content;
};
static ChatHistMsg g_chat_hist[CHAT_HIST_MAX];
static uint8_t g_chat_hist_n = 0;
ConvState g_conv;
ChatModeState g_chat_mode;  // legacy view synced from g_conv for logs
// Optional TTS override (always null — one device voice).
static const char* tts_lang_override = nullptr;
static const char* tts_voice_override = nullptr;
static uint32_t g_turn_id = 0;

static const char* effective_tts_lang() {
  return tts_lang_override ? tts_lang_override : GOOGLE_TTS_LANG;
}
static const char* effective_tts_voice() {
  if (tts_voice_override) return tts_voice_override;
  const char* v = device_config_get().voice;
  if (v && v[0]) return v;
  return GOOGLE_TTS_VOICE;
}
static const char* active_groq_key() {
  const char* k = device_config_get().groq_api_key;
  if (k && k[0]) return k;
  return GROQ_API_KEY;
}
static const char* active_google_key() {
  const char* k = device_config_get().google_api_key;
  if (k && k[0]) return k;
  return GOOGLE_API_KEY;
}

// Net task (core 0) owns TLS handshakes; net_mutex serializes secure_client use.
// tts_client is connected by net_task without net_mutex (tts_connecting guard).
static SemaphoreHandle_t net_mutex = nullptr;
static volatile bool tls_preconnect_req = false;
static volatile bool tts_preconnect_req = false;
static volatile bool tls_force_reconnect = false;
static volatile bool tts_force_reconnect = false;
static volatile bool tts_connecting = false;
static uint32_t groq_tls_last_ok_ms = 0;
static uint32_t tts_tls_last_ok_ms = 0;
static bool just_played = false;
static uint32_t listen_paused_until_ms = 0;

// Keep I2S TX + speaker open across consecutive TTS sentences in one turn.
static bool speak_session_open = false;
static bool speak_i2s_open = false;
static bool speak_speaker_on = false;

// Local opener clip (PSRAM) — plays while LLM TTFT runs (no network).
static int16_t* opener_pcm = nullptr;
static size_t opener_samples = 0;

TalkState state = STATE_LISTEN;

void request_tls_preconnect();
void request_tts_preconnect();
void request_tls_refresh();
void request_tts_refresh();
bool ensure_groq_tls(bool force_reconnect);
bool ensure_gemini_tls(bool force_reconnect);
bool ensure_tts_tls(bool force_reconnect);
void stop_groq_tls();
void stop_tts_tls();
bool speak_text(const String& text);
bool speak_text_ex(const String& text, bool google_only);
bool looks_like_en_fewshot_dump(const String& s);
bool google_cloud_tts_stream_play(const String& text);
void end_turn_cleanup();
void play_filler_chirp();
bool play_opener_clip();
bool cache_opener_clip(const char* text);
void speak_session_begin();
void speak_session_end();
bool ensure_speak_i2s();
void poll_serial_commands();

int body_reader_read(BodyReader* br, uint8_t* dst, size_t want);
bool body_read_exact(BodyReader* br, uint8_t* dst, size_t n);

static const char* state_name(TalkState s) {
  switch (s) {
    case STATE_LISTEN: return "LISTEN";
    case STATE_PROCESS: return "PROCESS";
    case STATE_SPEAK: return "SPEAK";
  }
  return "?";
}

static void set_state(TalkState s, const char* why) {
  if (state == s && why == nullptr) return;
  TalkState prev = state;
  state = s;
  if (why) {
    Serial.printf("state: %s -> %s (%s)\n", state_name(prev), state_name(s), why);
  } else if (prev != s) {
    Serial.printf("state: %s -> %s\n", state_name(prev), state_name(s));
  }
}

// --- Codec / speaker ---

void aic3204_write_reg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(AIC3204_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

void xmos_write_1byte(uint8_t resid, uint8_t cmd, uint8_t value) {
  Wire.beginTransmission(XMOS_ADDR);
  Wire.write(resid);
  Wire.write(cmd);
  Wire.write(1);
  Wire.write(value);
  Wire.endTransmission();
}

void set_speaker(bool on) { xmos_write_1byte(0xF1, 0x10, on ? 1 : 0); }

// Push stereo zero frames into TX I2S so DMA drains to true silence (kills end click).
static void i2s_tx_write_silence_ms(uint32_t ms) {
  if (ms == 0) return;
  const size_t FRAMES = 256;
  int32_t zeros[FRAMES * 2];
  memset(zeros, 0, sizeof(zeros));
  size_t need = (size_t)SAMPLE_RATE * ms / 1000;
  size_t done = 0;
  while (done < need) {
    size_t n = need - done;
    if (n > FRAMES) n = FRAMES;
    uint8_t* p = (uint8_t*)zeros;
    size_t total = n * 2 * sizeof(int32_t);
    size_t off = 0;
    while (off < total) {
      size_t w = i2s.write(p + off, total - off);
      if (w == 0) delay(1);
      off += w;
    }
    done += n;
  }
}

// Order matters: silence while amp still on → mute → close I2S.
// Abrupt mute/end with leftover DMA is what sounds like "줄/툭".
static void i2s_tx_soft_stop(bool mute_speaker) {
  i2s_tx_write_silence_ms(150);  // ≥ ~4 DMA buffers @ 16 kHz / 512 frames
  if (mute_speaker) {
    set_speaker(false);
    delay(12);
  }
  i2s.end();
}

// AIC3204 HPL/LOL gain: 0x3A=-6dB (hardware floor) … 0x3F=-1dB, 0x00=0dB, …
// See https://wiki.seeedstudio.com/respeaker_volume/
static const uint8_t CODEC_GAIN = 0x3A;
// Digital playback scale (0.5 = half amplitude). Codec is already at min gain.
static const float SPEAKER_VOLUME = 0.5f;

static inline int32_t pcm16_to_i2s32(int16_t s) {
  int32_t v = (int32_t)((float)s * SPEAKER_VOLUME);
  return v << 16;
}

void init_codec() {
  Wire.begin(5, 6);
  set_speaker(false);
  aic3204_write_reg(0x00, 0x01);
  aic3204_write_reg(0x10, CODEC_GAIN);
  aic3204_write_reg(0x11, CODEC_GAIN);
  aic3204_write_reg(0x12, CODEC_GAIN);
  aic3204_write_reg(0x13, CODEC_GAIN);
}

bool begin_i2s_rx() {
  auto cfg = i2s.defaultConfig(RX_MODE);
  cfg.copyFrom(speaker_info);
  cfg.channels = 1;
  cfg.bits_per_sample = 32;
  cfg.pin_bck = 8;
  cfg.pin_ws = 7;
  cfg.pin_data_rx = 44;
  cfg.is_master = false;
  cfg.use_apll = false;
  cfg.buffer_size = 512;
  cfg.buffer_count = 4;
  return i2s.begin(cfg);
}

bool begin_i2s_tx() {
  auto cfg = i2s.defaultConfig(TX_MODE);
  cfg.copyFrom(speaker_info);
  cfg.pin_bck = 8;
  cfg.pin_ws = 7;
  cfg.pin_data = 43;
  cfg.pin_data_rx = 44;
  cfg.is_master = false;
  cfg.use_apll = false;
  cfg.buffer_size = 512;
  cfg.buffer_count = 4;
  return i2s.begin(cfg);
}

// --- Audio helpers ---

int16_t i32_to_i16(int32_t sample) {
  if (abs(sample) > 32767) {
    sample >>= 16;
  }
  if (sample > 32767) return 32767;
  if (sample < -32768) return -32768;
  return (int16_t)sample;
}

float sample_rms(const int16_t* data, size_t n) {
  if (n == 0) return 0.0f;
  double sum = 0.0;
  for (size_t i = 0; i < n; i++) {
    float v = data[i] / 32768.0f;
    sum += v * v;
  }
  return sqrt(sum / n);
}

float sample_zcr(const int16_t* data, size_t n) {
  if (n < 2) return 0.0f;
  size_t crossings = 0;
  for (size_t i = 1; i < n; i++) {
    int16_t a = data[i - 1];
    int16_t b = data[i];
    if ((a >= 0 && b < 0) || (a < 0 && b >= 0)) crossings++;
  }
  return (float)crossings / (float)(n - 1);
}

void clamp_noise_floor() {
  if (noise_floor_rms < NOISE_FLOOR_MIN) noise_floor_rms = NOISE_FLOOR_MIN;
  if (noise_floor_rms > NOISE_FLOOR_MAX) noise_floor_rms = NOISE_FLOOR_MAX;
}

void update_noise_floor(float rms, bool likely_silence) {
  float a = likely_silence ? NOISE_EMA_FAST : NOISE_EMA_SLOW;
  // Prefer quieter frames; ambient rise only when clearly silence.
  if (rms < noise_floor_rms) {
    noise_floor_rms = (1.0f - a) * noise_floor_rms + a * rms;
  } else if (likely_silence) {
    noise_floor_rms = (1.0f - NOISE_EMA_SLOW) * noise_floor_rms + NOISE_EMA_SLOW * rms;
  }
  clamp_noise_floor();
}

float vad_onset_threshold() {
  float t = noise_floor_rms * VAD_ONSET_SNR;
  if (t < VAD_ABS_MIN) t = VAD_ABS_MIN;
  if (t > VAD_ONSET_ABS_MAX) t = VAD_ONSET_ABS_MAX;
  return t;
}

float vad_end_threshold() {
  // Silence = below max(noise×SNR, peak×ratio). Ratio must be high enough that
  // post-speech ambient (~0.02–0.03) counts as silence — low ratio caused 5s max.
  float t = noise_floor_rms * VAD_END_SNR;
  float floor = 0.012f;
  if (t < floor) t = floor;
  if (last_onset_peak_rms > 0.0f) {
    float relative = last_onset_peak_rms * 0.42f;
    // Quiet-but-real speech (onset ~0.055): keep end_th above typical ambient.
    if (last_onset_peak_rms >= 0.050f && relative < 0.028f) relative = 0.028f;
    if (relative > t) t = relative;
    // Don't treat ongoing speech as silence.
    float cap = last_onset_peak_rms * 0.55f;
    if (t > cap) t = cap;
  }
  return t;
}

bool window_looks_like_speech(float rms, float zcr) {
  if (rms < VAD_ONSET_ABS_MIN) return false;
  if (rms < vad_onset_threshold()) return false;
  if (zcr < ZCR_SPEECH_MIN || zcr > ZCR_SPEECH_MAX) return false;
  return true;
}

// Light HPF (~90 Hz) + soft noise gate before STT upload.
void denoise_recording() {
  if (record_count < 2) return;

  const float alpha = 0.965f;  // ~90 Hz @ 16 kHz
  float prev_x = record_buf[0] / 32768.0f;
  float prev_y = 0.0f;
  float gate = noise_floor_rms * 2.8f;
  if (gate < 0.010f) gate = 0.010f;

  for (size_t i = 0; i < record_count; i++) {
    float x = record_buf[i] / 32768.0f;
    float y = alpha * (prev_y + x - prev_x);
    prev_x = x;
    prev_y = y;

    float ay = fabsf(y);
    if (ay < gate) {
      float g = ay / gate;
      y *= g * g;
    }

    long s = lroundf(y * 32768.0f);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    record_buf[i] = (int16_t)s;
  }
}

uint32_t groq_retry_wait_ms(const String& err) {
  int idx = err.indexOf("Please try again in ");
  uint32_t ms = 60000;
  if (idx >= 0) {
    String rest = err.substring(idx + 20);
    int mpos = rest.indexOf('m');
    int spos = rest.indexOf('s');
    if (mpos >= 0 && (spos < 0 || mpos < spos)) {
      ms = (uint32_t)(rest.substring(0, mpos).toFloat() * 60000.0f);
      ms += (uint32_t)(rest.substring(mpos + 1).toFloat() * 1000.0f);
    } else {
      ms = (uint32_t)(rest.toFloat() * 1000.0f);
    }
  }
  if (err.indexOf("per day") >= 0 || err.indexOf("(TPD)") >= 0 || err.indexOf("(RPD)") >= 0) {
    if (ms < 120000) ms = 120000;
  }
  if (ms < 3000) ms = 3000;
  if (ms > 6UL * 3600UL * 1000UL) ms = 6UL * 3600UL * 1000UL;
  return ms + 1500;
}

bool stt_cooldown_active() {
  return millis() < stt_blocked_until_ms;
}

uint32_t stt_cooldown_remaining_ms() {
  uint32_t now = millis();
  if (now >= stt_blocked_until_ms) return 0;
  return stt_blocked_until_ms - now;
}

void note_stt_rate_limit(int http_status, const uint8_t* body, size_t body_len) {
  stt_last_call_ms = millis();
  stt_call_count++;

  if (http_status != 429 || !body || body_len == 0) return;

  String err((const char*)body, min(body_len, (size_t)512));
  uint32_t wait_ms = groq_retry_wait_ms(err);
  stt_blocked_until_ms = millis() + wait_ms;
  Serial.printf("stt: rate limited — wait %us (calls=%u)\n", wait_ms / 1000, stt_call_count);
}

bool recording_has_speech() {
  const float dur = record_count / (float)SAMPLE_RATE;
  if (record_count < MIN_SPEECH_SAMPLES) {
    Serial.printf("listen: reject short (%.2fs < %.2fs min)\n", dur,
                  MIN_SPEECH_SAMPLES / (float)SAMPLE_RATE);
    return false;
  }
  const bool short_utt = record_count < MIN_SPEECH_SAMPLES_FULL;

  float avg = sample_rms(record_buf, record_count);
  float min_avg = noise_floor_rms * SPEECH_MIN_SNR;
  float abs_floor = SPEECH_ABS_MIN;
  if (last_onset_peak_rms > 0.0f) {
    // Peak-relative floor must stay below typical speech averages (silence gaps).
    float adaptive = last_onset_peak_rms * 0.20f;
    if (adaptive > abs_floor) abs_floor = adaptive;
  }
  if (min_avg < abs_floor) min_avg = abs_floor;
  // High ambient nf used to push need≈0.11 and reject real speech — cap it.
  if (min_avg > SPEECH_AVG_ABS_CAP) min_avg = SPEECH_AVG_ABS_CAP;
  // Short replies (응/네): allow slightly lower average if onset peak was clear.
  if (short_utt) min_avg *= 0.90f;
  if (avg < min_avg) {
    Serial.printf("listen: reject quiet (avg=%.3f need=%.3f onset=%.3f nf=%.3f dur=%.2fs)\n",
                  avg, min_avg, last_onset_peak_rms, noise_floor_rms, dur);
    return false;
  }

  float peak_need = noise_floor_rms * SPEECH_PEAK_SNR;
  if (peak_need < SPEECH_PEAK_ABS) peak_need = SPEECH_PEAK_ABS;
  if (last_onset_peak_rms > 0.0f) {
    float adaptive_peak = last_onset_peak_rms * 0.55f;
    if (peak_need > adaptive_peak) peak_need = adaptive_peak;
  }
  if (peak_need > SPEECH_PEAK_ABS_MAX) peak_need = SPEECH_PEAK_ABS_MAX;

  float peak = 0.0f;
  size_t peak_windows = 0;
  size_t voiced_windows = 0;
  double sum_w = 0.0;
  double sum_w2 = 0.0;
  size_t n_w = 0;
  const size_t win = 320;

  for (size_t i = 0; i + win <= record_count; i += win / 2) {
    float w = sample_rms(record_buf + i, win);
    float z = sample_zcr(record_buf + i, win);
    sum_w += w;
    sum_w2 += (double)w * (double)w;
    n_w++;
    if (w > peak) peak = w;
    if (w >= peak_need) peak_windows++;
    if (w >= peak_need * 0.92f && z >= ZCR_SPEECH_MIN && z <= ZCR_SPEECH_MAX) {
      voiced_windows++;
    }
  }

  if (n_w < 2) {
    Serial.printf("listen: reject short_windows (n=%u dur=%.2fs)\n", (unsigned)n_w, dur);
    return false;
  }
  float mean_w = (float)(sum_w / n_w);
  float var_w = (float)(sum_w2 / n_w) - mean_w * mean_w;
  if (var_w < 0.0f) var_w = 0.0f;
  float mod = (mean_w > 1e-6f) ? (sqrtf(var_w) / mean_w) : 0.0f;

  size_t need_peak = short_utt ? SPEECH_PEAK_WINDOWS_MIN_SHORT : SPEECH_PEAK_WINDOWS_MIN;
  size_t need_voiced = short_utt ? SPEECH_VOICED_WINDOWS_MIN_SHORT : SPEECH_VOICED_WINDOWS_MIN;
  float need_mod = short_utt ? (SPEECH_MOD_MIN * 0.7f) : SPEECH_MOD_MIN;

  bool ok = peak >= peak_need && peak_windows >= need_peak && voiced_windows >= need_voiced &&
            mod >= need_mod;

  if (!ok) {
    Serial.printf(
        "listen: reject noise (avg=%.3f peak=%.3f mod=%.2f voiced=%u/%u dur=%.2fs short=%d nf=%.3f)\n",
        avg, peak, mod, (unsigned)voiced_windows, (unsigned)need_voiced, dur, short_utt ? 1 : 0,
        noise_floor_rms);
  } else {
    Serial.printf("listen: accept speech (dur=%.2fs short=%d peak=%.3f)\n", dur, short_utt ? 1 : 0,
                  peak);
  }
  return ok;
}

bool is_whisper_hallucination(const String& text) {
  String lower = text;
  lower.toLowerCase();

  static const char* blocked[] = {
      "다음 영상에서 만나요",
      "다음 영상에서 뵙",
      "다음 영상에서",
      "구독과 좋아요",
      "시청해주셔서 감사",
      "시청해 주셔서 감사",
      "시청해줘서 고마",
      "시청해줘서 감사",
      "시청해 주셔서 고마",
      "봐주셔서 감사",
      "thank you for watching",
      "thanks for watching",
      "thanks for listening",
      "see you in the next video",
      "see you next time",
      "subscribe",
      "subtitle",
      "amara.org",
      "mbc",
      "뉴스",
      "자막",
      "자막 제공",
      "채널",
      "편집",
      "협찬",
      nullptr,
  };
  for (int i = 0; blocked[i]; i++) {
    String b = blocked[i];
    b.toLowerCase();
    if (lower.indexOf(b) >= 0) return true;
  }
  return false;
}

bool is_valid_transcript(const String& text) {
  String t = text;
  t.trim();
  if (t.length() < 2) return false;
  if (is_whisper_hallucination(t)) return false;

  int letters = 0;
  for (size_t i = 0; i < t.length();) {
    uint8_t c = (uint8_t)t[i];
    if (c < 0x80) {
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        letters++;
      }
      i++;
    } else {
      letters++;
      if ((c & 0xF0) == 0xF0) i += 4;
      else if ((c & 0xE0) == 0xE0) i += 3;
      else if ((c & 0xC0) == 0xC0) i += 2;
      else i++;
    }
  }
  return letters >= 1;
}

size_t write_wav_header(uint8_t* out, size_t pcm_bytes, uint32_t rate, uint16_t channels) {
  size_t data_size = pcm_bytes;
  size_t file_size = 36 + data_size;
  out[0] = 'R';
  out[1] = 'I';
  out[2] = 'F';
  out[3] = 'F';
  out[4] = (file_size)&0xFF;
  out[5] = (file_size >> 8) & 0xFF;
  out[6] = (file_size >> 16) & 0xFF;
  out[7] = (file_size >> 24) & 0xFF;
  out[8] = 'W';
  out[9] = 'A';
  out[10] = 'V';
  out[11] = 'E';
  out[12] = 'f';
  out[13] = 'm';
  out[14] = 't';
  out[15] = ' ';
  out[16] = 16;
  out[17] = 0;
  out[18] = 0;
  out[19] = 0;
  out[20] = 1;
  out[21] = 0;
  out[22] = channels & 0xFF;
  out[23] = (channels >> 8) & 0xFF;
  out[24] = rate & 0xFF;
  out[25] = (rate >> 8) & 0xFF;
  out[26] = (rate >> 16) & 0xFF;
  out[27] = (rate >> 24) & 0xFF;
  uint32_t byte_rate = rate * channels * 2;
  out[28] = byte_rate & 0xFF;
  out[29] = (byte_rate >> 8) & 0xFF;
  out[30] = (byte_rate >> 16) & 0xFF;
  out[31] = (byte_rate >> 24) & 0xFF;
  out[32] = (channels * 2) & 0xFF;
  out[33] = 0;
  out[34] = 16;
  out[35] = 0;
  out[36] = 'd';
  out[37] = 'a';
  out[38] = 't';
  out[39] = 'a';
  out[40] = data_size & 0xFF;
  out[41] = (data_size >> 8) & 0xFF;
  out[42] = (data_size >> 16) & 0xFF;
  out[43] = (data_size >> 24) & 0xFF;
  return 44;
}

// G.711 μ-law WAV (fmt=7) — half the bytes of PCM16; Whisper accepts via ffmpeg.
size_t write_wav_header_ulaw(uint8_t* out, size_t ulaw_bytes, uint32_t rate, uint16_t channels) {
  size_t data_size = ulaw_bytes;
  size_t file_size = 50 + data_size;  // 18-byte fmt + fact chunk
  out[0] = 'R'; out[1] = 'I'; out[2] = 'F'; out[3] = 'F';
  out[4] = (file_size)&0xFF;
  out[5] = (file_size >> 8) & 0xFF;
  out[6] = (file_size >> 16) & 0xFF;
  out[7] = (file_size >> 24) & 0xFF;
  out[8] = 'W'; out[9] = 'A'; out[10] = 'V'; out[11] = 'E';
  out[12] = 'f'; out[13] = 'm'; out[14] = 't'; out[15] = ' ';
  out[16] = 18; out[17] = 0; out[18] = 0; out[19] = 0;  // fmt chunk size
  out[20] = 7; out[21] = 0;   // WAVE_FORMAT_MULAW
  out[22] = channels & 0xFF;
  out[23] = (channels >> 8) & 0xFF;
  out[24] = rate & 0xFF;
  out[25] = (rate >> 8) & 0xFF;
  out[26] = (rate >> 16) & 0xFF;
  out[27] = (rate >> 24) & 0xFF;
  uint32_t byte_rate = rate * channels;
  out[28] = byte_rate & 0xFF;
  out[29] = (byte_rate >> 8) & 0xFF;
  out[30] = (byte_rate >> 16) & 0xFF;
  out[31] = (byte_rate >> 24) & 0xFF;
  out[32] = channels & 0xFF; out[33] = 0;  // block align
  out[34] = 8; out[35] = 0;                // bits
  out[36] = 0; out[37] = 0;                // cbSize
  out[38] = 'f'; out[39] = 'a'; out[40] = 'c'; out[41] = 't';
  out[42] = 4; out[43] = 0; out[44] = 0; out[45] = 0;
  uint32_t samples = (uint32_t)ulaw_bytes / channels;
  out[46] = samples & 0xFF;
  out[47] = (samples >> 8) & 0xFF;
  out[48] = (samples >> 16) & 0xFF;
  out[49] = (samples >> 24) & 0xFF;
  out[50] = 'd'; out[51] = 'a'; out[52] = 't'; out[53] = 'a';
  out[54] = data_size & 0xFF;
  out[55] = (data_size >> 8) & 0xFF;
  out[56] = (data_size >> 16) & 0xFF;
  out[57] = (data_size >> 24) & 0xFF;
  return 58;
}

bool record_utterance() {
  record_count = 0;
  if (!begin_i2s_rx()) {
    Serial.println("ERR: I2S RX init failed");
    return false;
  }

  Serial.println("listen: speak now");
  Serial.printf("dbg: wifi=%s heap=%u psram=%u\n",
                WiFi.status() == WL_CONNECTED ? "ok" : "down",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  int32_t raw[128];

  // Speaker echo tail right after playback inflates the noise floor — drop it.
  if (just_played) {
    just_played = false;
    size_t drop_samples = SAMPLE_RATE * POST_PLAY_FLUSH_MS / 1000;
    size_t dropped = 0;
    uint32_t flush_start = millis();
    while (dropped < drop_samples && millis() - flush_start < 1000) {
      size_t b = i2s.readBytes((uint8_t*)raw, sizeof(raw));
      if (b == 0) delay(1);
      dropped += b / sizeof(int32_t);
    }
  }

  bool started = false;
  bool calibrated = false;
  size_t start_hits = 0;
  float onset_peak_rms = 0.0f;
  float calib_min = 1.0f;
  size_t calib_n = 0;
  uint32_t silence_start = 0;
  uint32_t noise_blip_start = 0;
  uint32_t wait_start = millis();
  uint32_t last_vad_log_ms = 0;
  float last_rms = 0.0f;

  while (record_count < MAX_SAMPLES) {
    // Serial typing (KOTEST) must not be picked up as speech by the mic.
    if (Serial.available() > 0) {
      poll_serial_commands();
      listen_paused_until_ms = millis() + SERIAL_TYPING_PAUSE_MS;
      start_hits = 0;
      onset_peak_rms = 0.0f;
      if (started) {
        started = false;
        record_count = 0;
        silence_start = 0;
        noise_blip_start = 0;
        Serial.println("listen: cancelled (serial typing)");
      }
      if (state != STATE_LISTEN) {
        i2s.end();
        return false;
      }
      size_t drain = i2s.readBytes((uint8_t*)raw, sizeof(raw));
      (void)drain;
      continue;
    }

    size_t bytes = i2s.readBytes((uint8_t*)raw, sizeof(raw));
    if (bytes == 0) {
      delay(1);
      continue;
    }
    size_t n_samples = bytes / sizeof(int32_t);
    for (size_t i = 0; i < n_samples && record_count < MAX_SAMPLES; i++) {
      record_buf[record_count++] = i32_to_i16(raw[i]);
    }

    // Ambient calibration before accepting speech onset.
    if (!calibrated) {
      if (record_count >= VAD_END_SAMPLES) {
        float w = sample_rms(record_buf + record_count - VAD_END_SAMPLES, VAD_END_SAMPLES);
        if (w < calib_min) calib_min = w;
        calib_n++;
      }
      if (record_count >= NOISE_CALIB_SAMPLES) {
        if (calib_n > 0) {
          // Quietest window + margin so onset stays above residual hiss.
          noise_floor_rms = calib_min * 1.85f;
          clamp_noise_floor();
        }
        calibrated = true;
        record_count = 0;
        Serial.printf("listen: noise floor=%.4f onset=%.4f end_th=%.4f\n",
                      noise_floor_rms, vad_onset_threshold(), vad_end_threshold());
        wait_start = millis();
      }
      continue;
    }

    size_t window = started ? VAD_END_SAMPLES : VAD_START_SAMPLES;
    size_t check_len = min(window, record_count);
    const int16_t* chunk = record_buf + record_count - check_len;
    float rms = sample_rms(chunk, check_len);
    float zcr = sample_zcr(chunk, check_len);
    last_rms = rms;

    if (!started) {
      bool speechish = window_looks_like_speech(rms, zcr);
      if (speechish) {
        if (rms > onset_peak_rms) onset_peak_rms = rms;
        start_hits++;
        if (start_hits >= VAD_START_HITS) {
          if (rms < VAD_ONSET_ABS_MIN || onset_peak_rms < VAD_ONSET_ABS_MIN) {
            Serial.printf("listen: reject impulsive (rms=%.3f peak=%.3f)\n",
                          rms, onset_peak_rms);
            start_hits = 0;
            onset_peak_rms = 0.0f;
            continue;
          }
          started = true;
          silence_start = 0;
          noise_blip_start = 0;
          last_onset_peak_rms = onset_peak_rms;
          Serial.printf("listen: voice detected (rms=%.3f peak=%.3f zcr=%.2f end_th=%.3f)\n",
                        rms, onset_peak_rms, zcr, vad_end_threshold());
          // Force-refresh stale sockets — zombie connected() skipped preconnect before.
          request_tls_refresh();
          request_tts_refresh();
        }
      } else {
        start_hits = 0;
        onset_peak_rms = 0.0f;
        update_noise_floor(rms, true);
        if (millis() - wait_start > MAX_WAIT_SPEECH_MS) {
          Serial.println("listen: timeout waiting for speech");
          last_onset_peak_rms = 0.0f;
          i2s.end();
          return false;
        }
        // Keep a pre-roll so the first syllable isn't clipped at onset.
        if (record_count > PREROLL_SAMPLES) {
          memmove(record_buf, record_buf + record_count - PREROLL_SAMPLES,
                  PREROLL_SAMPLES * sizeof(int16_t));
          record_count = PREROLL_SAMPLES;
        }
        continue;
      }
    }

    // Track running peak so end_th rises with louder syllables.
    if (started && rms > last_onset_peak_rms) {
      last_onset_peak_rms = rms;
    }

    float end_th = vad_end_threshold();
    if (started && millis() - last_vad_log_ms >= 400) {
      last_vad_log_ms = millis();
      uint32_t sil_ms = silence_start ? (millis() - silence_start) : 0;
      Serial.printf("listen: vad rms=%.3f end_th=%.3f peak=%.3f silence=%ums rec=%.1fs\n",
                    rms, end_th, last_onset_peak_rms, (unsigned)sil_ms,
                    record_count / (float)SAMPLE_RATE);
    }

    if (rms < end_th) {
      noise_blip_start = 0;
      if (silence_start == 0) silence_start = millis();
      uint32_t silence_elapsed = millis() - silence_start;
      uint32_t speech_ms = (uint32_t)(record_count * 1000UL / SAMPLE_RATE);
      uint32_t silence_needed =
          (speech_ms >= SILENCE_ADAPT_AFTER_MS) ? SILENCE_MS_SHORT : SILENCE_MS_LONG;

      if (silence_elapsed >= silence_needed) {
        // Trim most of the trailing silence — less to upload, less to decode.
        size_t tail_keep = SAMPLE_RATE * TAIL_KEEP_MS / 1000;
        size_t tail_have = SAMPLE_RATE * silence_needed / 1000;
        if (tail_have > tail_keep && record_count > tail_have - tail_keep) {
          record_count -= (tail_have - tail_keep);
        }
        Serial.printf(
            "listen: end_utt (%.1fs silence_need=%ums reason=silence rms=%.3f end_th=%.3f peak=%.3f)\n",
            record_count / (float)SAMPLE_RATE, (unsigned)silence_needed, rms, end_th,
            last_onset_peak_rms);
        break;
      }
    } else if (silence_start != 0) {
      // Already in silence — ignore brief spikes so ambient blips don't reset.
      if (noise_blip_start == 0) noise_blip_start = millis();
      else if (millis() - noise_blip_start >= SILENCE_BLIP_IGNORE_MS) {
        silence_start = 0;
        noise_blip_start = 0;
      }
    } else {
      silence_start = 0;
      noise_blip_start = 0;
    }
  }

  if (record_count >= MAX_SAMPLES) {
    uint32_t sil_ms = silence_start ? (millis() - silence_start) : 0;
    Serial.printf(
        "listen: end_utt (%.1fs reason=max_record rms=%.3f end_th=%.3f peak=%.3f silence=%ums)\n",
        record_count / (float)SAMPLE_RATE, last_rms, vad_end_threshold(), last_onset_peak_rms,
        (unsigned)sil_ms);
  }

  i2s.end();
  if (!recording_has_speech()) {
    // Use quiet parts of rejected capture to refine floor for next turn.
    float trailing = sample_rms(record_buf, min(record_count, (size_t)VAD_START_SAMPLES));
    update_noise_floor(trailing, true);
    last_onset_peak_rms = 0.0f;
    Serial.println("listen: no speech captured");
    return false;
  }

  // Soft noise gate attenuates residual hiss before Whisper (cuts hallucination clips).
  denoise_recording();
  return true;
}

// --- HTTP / Groq ---

static const uint32_t GROQ_TLS_MAX_IDLE_MS = 12000;
static const uint8_t TLS_HOST_NONE = 0;
static const uint8_t TLS_HOST_GROQ = 1;
static const uint8_t TLS_HOST_GEMINI = 2;
static uint8_t secure_tls_host = TLS_HOST_NONE;

// Runs on core 0. Opens TLS early while core 1 records.
// secure_client uses net_mutex; tts_client uses tts_connecting only.
void net_task(void* /*arg*/) {
  for (;;) {
    if (tls_preconnect_req) {
      tls_preconnect_req = false;
      if (xSemaphoreTake(net_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        bool force = tls_force_reconnect;
        tls_force_reconnect = false;
        bool stale = secure_client && groq_tls_last_ok_ms > 0 &&
                     (millis() - groq_tls_last_ok_ms > TLS_STALE_REFRESH_MS);
        if (secure_client && WiFi.status() == WL_CONNECTED) {
          if ((force || stale) && secure_client->connected()) {
            secure_client->stop();
            groq_tls_last_ok_ms = 0;
          }
          if (!secure_client->connected()) {
            uint32_t t0 = millis();
            if (secure_client->connect(GROQ_HOST, GROQ_PORT)) {
              groq_tls_last_ok_ms = millis();
              secure_tls_host = TLS_HOST_GROQ;
              Serial.printf("tls: preconnected in %ums (overlapped with speech)\n",
                            (unsigned)(millis() - t0));
            } else {
              Serial.println("tls: preconnect failed");
              secure_tls_host = TLS_HOST_NONE;
            }
          }
        }
        xSemaphoreGive(net_mutex);
      } else {
        tls_preconnect_req = true;  // retry next loop
      }
    }
    if (tts_preconnect_req && !tts_connecting) {
      tts_preconnect_req = false;
      if (tts_client && WiFi.status() == WL_CONNECTED) {
        bool force = tts_force_reconnect;
        tts_force_reconnect = false;
        bool stale = tts_tls_last_ok_ms > 0 &&
                     (millis() - tts_tls_last_ok_ms > TLS_STALE_REFRESH_MS);
        if ((force || stale) && tts_client->connected()) {
          tts_client->stop();
          tts_tls_last_ok_ms = 0;
        }
        if (!tts_client->connected()) {
          tts_connecting = true;
          uint32_t t0 = millis();
          if (tts_client->connect(GOOGLE_TTS_HOST, GOOGLE_TTS_PORT)) {
            tts_tls_last_ok_ms = millis();
            Serial.printf("tts-tls: preconnected in %ums\n", (unsigned)(millis() - t0));
          } else {
            Serial.println("tts-tls: preconnect failed");
            tts_client->stop();
            tts_tls_last_ok_ms = 0;
          }
          tts_connecting = false;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void request_tls_preconnect() {
  tls_preconnect_req = true;
}

void request_tts_preconnect() {
  tts_preconnect_req = true;
}

void request_tls_refresh() {
  tls_force_reconnect = true;
  tls_preconnect_req = true;
}

void request_tts_refresh() {
  tts_force_reconnect = true;
  tts_preconnect_req = true;
}

void stop_groq_tls() {
  if (secure_client && secure_client->connected()) {
    secure_client->stop();
  }
  groq_tls_last_ok_ms = 0;
  secure_tls_host = TLS_HOST_NONE;
}

void stop_tts_tls() {
  tts_preconnect_req = false;
  uint32_t t0 = millis();
  while (tts_connecting && millis() - t0 < 3000) delay(5);
  tts_connecting = true;
  if (tts_client && tts_client->connected()) {
    tts_client->stop();
  }
  tts_tls_last_ok_ms = 0;
  tts_connecting = false;
}

bool ensure_groq_tls(bool force_reconnect) {
  if (!connect_wifi(true)) return false;

  bool wrong_host = secure_tls_host != TLS_HOST_GROQ;
  bool stale = groq_tls_last_ok_ms > 0 &&
               (millis() - groq_tls_last_ok_ms > TLS_STALE_REFRESH_MS);
  if (secure_client->connected() && !force_reconnect && !wrong_host && !stale &&
      groq_tls_last_ok_ms > 0 && millis() - groq_tls_last_ok_ms < GROQ_TLS_MAX_IDLE_MS) {
    return true;
  }
  if (secure_client->connected()) {
    secure_client->stop();
    secure_tls_host = TLS_HOST_NONE;
  }

  Serial.println(force_reconnect || stale || wrong_host ? "tls: reconnect groq..." : "tls: connect groq...");
  uint32_t t0 = millis();
  if (!secure_client->connect(GROQ_HOST, GROQ_PORT)) {
    Serial.println("ERR: TLS connect failed");
    groq_tls_last_ok_ms = 0;
    secure_tls_host = TLS_HOST_NONE;
    return false;
  }
  Serial.printf("tls: groq connected in %ums\n", (unsigned)(millis() - t0));
  groq_tls_last_ok_ms = millis();
  secure_tls_host = TLS_HOST_GROQ;
  return true;
}

bool ensure_gemini_tls(bool force_reconnect) {
  if (!connect_wifi(true)) return false;
  tls_preconnect_req = false;  // do not steal socket back to Groq mid-stream

  bool wrong_host = secure_tls_host != TLS_HOST_GEMINI;
  bool stale = groq_tls_last_ok_ms > 0 &&
               (millis() - groq_tls_last_ok_ms > TLS_STALE_REFRESH_MS);
  if (secure_client->connected() && !force_reconnect && !wrong_host && !stale &&
      groq_tls_last_ok_ms > 0 && millis() - groq_tls_last_ok_ms < GROQ_TLS_MAX_IDLE_MS) {
    return true;
  }
  if (secure_client->connected()) {
    secure_client->stop();
    secure_tls_host = TLS_HOST_NONE;
  }

  Serial.println(force_reconnect || stale || wrong_host ? "tls: reconnect gemini..."
                                                       : "tls: connect gemini...");
  uint32_t t0 = millis();
  if (!secure_client->connect(GEMINI_HOST, GEMINI_PORT)) {
    Serial.println("ERR: Gemini TLS connect failed");
    groq_tls_last_ok_ms = 0;
    secure_tls_host = TLS_HOST_NONE;
    return false;
  }
  Serial.printf("tls: gemini connected in %ums\n", (unsigned)(millis() - t0));
  groq_tls_last_ok_ms = millis();
  secure_tls_host = TLS_HOST_GEMINI;
  return true;
}

bool ensure_tts_tls(bool force_reconnect) {
  if (!connect_wifi(true)) return false;
  // Own the socket: cancel background preconnect so core0 cannot race connect().
  tts_preconnect_req = false;
  uint32_t t0 = millis();
  while (tts_connecting && millis() - t0 < 5000) delay(5);

  tts_connecting = true;
  bool stale = tts_tls_last_ok_ms > 0 &&
               (millis() - tts_tls_last_ok_ms > TLS_STALE_REFRESH_MS);
  if (tts_client->connected() && !force_reconnect && !stale && tts_tls_last_ok_ms > 0 &&
      millis() - tts_tls_last_ok_ms < GROQ_TLS_MAX_IDLE_MS) {
    tts_connecting = false;
    return true;
  }
  if (tts_client->connected()) tts_client->stop();

  Serial.println(force_reconnect || stale ? "tts-tls: reconnect..." : "tts-tls: connect...");
  t0 = millis();
  if (!tts_client->connect(GOOGLE_TTS_HOST, GOOGLE_TTS_PORT)) {
    Serial.println("ERR: TTS TLS connect failed");
    tts_client->stop();  // reset mbedtls after failed handshake
    tts_tls_last_ok_ms = 0;
    tts_connecting = false;
    return false;
  }
  Serial.printf("tts-tls: connected in %ums\n", (unsigned)(millis() - t0));
  tts_tls_last_ok_ms = millis();
  tts_connecting = false;
  return true;
}

void groq_write_headers_on(WiFiClientSecure* client, const char* content_type, size_t content_length) {
  client->printf("Host: %s\r\n", GROQ_HOST);
  client->printf("Authorization: Bearer %s\r\n", active_groq_key());
  client->printf("Content-Type: %s\r\n", content_type);
  client->print("Connection: keep-alive\r\n");
  client->printf("Content-Length: %u\r\n\r\n", (unsigned)content_length);
}

void groq_write_headers(const char* content_type, size_t content_length) {
  groq_write_headers_on(secure_client, content_type, content_length);
}

void gemini_write_headers(size_t content_length) {
  secure_client->printf("Host: %s\r\n", GEMINI_HOST);
  secure_client->printf("x-goog-api-key: %s\r\n", active_google_key());
  secure_client->print("Content-Type: application/json\r\n");
  secure_client->print("Connection: close\r\n");
  secure_client->printf("Content-Length: %u\r\n\r\n", (unsigned)content_length);
}

void groq_after_response(bool ok, bool keep_alive) {
  if (!ok || !keep_alive) {
    stop_groq_tls();
  } else {
    groq_tls_last_ok_ms = millis();
  }
}

void init_wifi() { wifi_portal_begin(); }

bool connect_wifi(bool blocking) { return wifi_portal_ensure(blocking); }

void maintain_wifi() { wifi_portal_maintain(); }

bool read_chunked_body(WiFiClient* client, uint8_t** body, size_t* body_len) {
  size_t capacity = 8192;
  size_t total = 0;
  *body = (uint8_t*)heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!*body) *body = (uint8_t*)malloc(capacity);
  if (!*body) return false;

  uint32_t start = millis();
  while (millis() - start < 60000) {
    String chunk_size_line = client->readStringUntil('\n');
    chunk_size_line.trim();
    if (chunk_size_line.length() == 0) {
      if (!client->connected() && !client->available()) break;
      continue;
    }
    size_t chunk_size = (size_t)strtoul(chunk_size_line.c_str(), nullptr, 16);
    if (chunk_size == 0) break;

    while (total + chunk_size > capacity) {
      capacity *= 2;
      uint8_t* bigger = (uint8_t*)heap_caps_realloc(*body, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!bigger) bigger = (uint8_t*)realloc(*body, capacity);
      if (!bigger) {
        free(*body);
        *body = nullptr;
        return false;
      }
      *body = bigger;
    }

    size_t got = 0;
    while (got < chunk_size && millis() - start < 60000) {
      int n = client->read(*body + total + got, chunk_size - got);
      if (n > 0) got += n;
      else delay(1);
    }
    if (got != chunk_size) {
      free(*body);
      *body = nullptr;
      return false;
    }
    total += got;
  }

  *body_len = total;
  return total > 0;
}

bool read_http_response(WiFiClient* client, int* status_code, uint8_t** body, size_t* body_len,
                        bool* keep_alive_out) {
  String status_line = client->readStringUntil('\n');
  status_line.trim();
  if (status_line.length() == 0) return false;
  if (!status_line.startsWith("HTTP/")) {
    Serial.printf("http: bad status line: %.40s\n", status_line.c_str());
    return false;
  }
  *status_code = status_line.substring(9, 12).toInt();
  if (*status_code < 100 || *status_code > 599) {
    Serial.printf("http: bad status code in: %.40s\n", status_line.c_str());
    return false;
  }

  int content_length = -1;
  bool chunked = false;
  bool keep_alive = true;  // HTTP/1.1 default
  while (client->connected() || client->available()) {
    String line = client->readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
    if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
      content_length = line.substring(15).toInt();
    } else if (line.startsWith("Transfer-Encoding:") || line.startsWith("transfer-encoding:")) {
      if (line.indexOf("chunked") >= 0) chunked = true;
    } else if (line.startsWith("Connection:") || line.startsWith("connection:")) {
      String lower = line;
      lower.toLowerCase();
      if (lower.indexOf("close") >= 0) keep_alive = false;
    }
  }

  if (keep_alive_out) *keep_alive_out = keep_alive;

  if (chunked) {
    return read_chunked_body(client, body, body_len);
  }

  if (content_length <= 0) {
    *body = nullptr;
    *body_len = 0;
    return true;
  }

  *body = (uint8_t*)heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!*body) *body = (uint8_t*)malloc(content_length);
  if (!*body) return false;

  size_t got = 0;
  uint32_t start = millis();
  while (got < (size_t)content_length && millis() - start < 60000) {
    int n = client->read(*body + got, content_length - got);
    if (n > 0) got += n;
    else delay(1);
  }
  *body_len = got;
  return got == (size_t)content_length;
}

String url_encode(const String& s) {
  String out;
  out.reserve(s.length() * 3);
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~') {
      out += c;
    } else if (c == ' ') {
      out += "%20";
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

bool https_get(const char* host, const char* path, uint8_t** body, size_t* body_len) {
  if (!connect_wifi(true)) return false;
  stop_groq_tls();  // shared client — drop keep-alive before other hosts
  if (!secure_client->connect(host, 443)) return false;

  secure_client->printf("GET %s HTTP/1.1\r\n", path);
  secure_client->printf("Host: %s\r\n", host);
  secure_client->print("Connection: close\r\n\r\n");

  int status = 0;
  if (!read_http_response(secure_client, &status, body, body_len, nullptr)) {
    secure_client->stop();
    return false;
  }
  secure_client->stop();
  if (status != 200) {
    Serial.printf("ERR: GET %s HTTP %d\n", host, status);
    if (*body) {
      free(*body);
      *body = nullptr;
      *body_len = 0;
    }
    return false;
  }
  return true;
}

bool base64_decode(const char* in, size_t in_len, uint8_t** out, size_t* out_len) {
  size_t needed = 0;
  if (mbedtls_base64_decode(nullptr, 0, &needed, (const unsigned char*)in, in_len) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    return false;
  }
  *out = (uint8_t*)heap_caps_malloc(needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!*out) *out = (uint8_t*)malloc(needed);
  if (!*out) return false;
  if (mbedtls_base64_decode(*out, needed, out_len, (const unsigned char*)in, in_len) != 0) {
    free(*out);
    *out = nullptr;
    *out_len = 0;
    return false;
  }
  return true;
}

// ITU-T G.711 μ-law -> PCM16 (one sample).
static inline int16_t ulaw_to_pcm16(uint8_t u) {
  u = ~u;
  int t = ((u & 0x0F) << 3) + 0x84;
  t <<= (u & 0x70) >> 4;
  return (u & 0x80) ? (int16_t)(0x84 - t) : (int16_t)(t - 0x84);
}

// PCM16 -> G.711 μ-law (for compact STT upload).
static inline uint8_t pcm16_to_ulaw(int16_t pcm) {
  const uint16_t BIAS = 0x84;
  const uint16_t CLIP = 32635;
  uint8_t sign = (pcm < 0) ? 0x80 : 0;
  if (pcm < 0) pcm = -pcm;
  if (pcm > (int16_t)CLIP) pcm = (int16_t)CLIP;
  pcm = (int16_t)(pcm + BIAS);
  int exp = 7;
  for (int mask = 0x4000; (pcm & mask) == 0 && exp > 0; mask >>= 1) exp--;
  int mantissa = (pcm >> (exp + 3)) & 0x0F;
  return (uint8_t)(~(sign | (exp << 4) | mantissa));
}

// Expand raw μ-law bytes to little-endian PCM16. Caller frees *pcm_out.
bool ulaw_bytes_to_pcm16(const uint8_t* ulaw, size_t n, uint8_t** pcm_out, size_t* pcm_len) {
  *pcm_out = nullptr;
  *pcm_len = 0;
  if (!ulaw || n == 0) return false;
  size_t out_len = n * 2;
  uint8_t* pcm = (uint8_t*)heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pcm) pcm = (uint8_t*)malloc(out_len);
  if (!pcm) return false;
  for (size_t i = 0; i < n; i++) {
    int16_t s = ulaw_to_pcm16(ulaw[i]);
    pcm[i * 2] = (uint8_t)(s & 0xFF);
    pcm[i * 2 + 1] = (uint8_t)((s >> 8) & 0xFF);
  }
  *pcm_out = pcm;
  *pcm_len = out_len;
  return true;
}

// Build μ-law WAV from record_buf. Caller frees *wav_out. Returns false on OOM.
bool build_ulaw_wav(uint8_t** wav_out, size_t* wav_len) {
  *wav_out = nullptr;
  *wav_len = 0;
  if (record_count == 0) return false;
  size_t hdr = 58;
  size_t ulaw_n = record_count;
  size_t total = hdr + ulaw_n;
  uint8_t* wav = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!wav) wav = (uint8_t*)malloc(total);
  if (!wav) return false;
  write_wav_header_ulaw(wav, ulaw_n, SAMPLE_RATE, 1);
  for (size_t i = 0; i < ulaw_n; i++) {
    wav[hdr + i] = pcm16_to_ulaw(record_buf[i]);
  }
  *wav_out = wav;
  *wav_len = total;
  return true;
}

bool groq_post_multipart_stt_once(const uint8_t* wav, size_t wav_len, String& transcript) {
  if (!ensure_groq_tls(false)) return false;

  const char* boundary = "----TalkbotBoundary7MA4YWxk";
  String part_model = String("--") + boundary +
                      "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + STT_MODEL + "\r\n";
  String part_lang = String("--") + boundary +
                     "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\n" + STT_LANGUAGE + "\r\n";
  String part_fmt = String("--") + boundary +
                    "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\ntext\r\n";
  String head = String("--") + boundary +
                "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
                "Content-Type: audio/wav\r\n\r\n";
  String tail = "\r\n" + part_model + part_lang + part_fmt + "--" + boundary + "--\r\n";
  size_t total = head.length() + wav_len + tail.length();

  String ctype = String("multipart/form-data; boundary=") + boundary;
  secure_client->printf("POST /openai/v1/audio/transcriptions HTTP/1.1\r\n");
  groq_write_headers(ctype.c_str(), total);
  secure_client->print(head);
  secure_client->write(wav, wav_len);
  secure_client->print(tail);

  int status = 0;
  uint8_t* body = nullptr;
  size_t body_len = 0;
  bool keep_alive = false;
  bool ok = read_http_response(secure_client, &status, &body, &body_len, &keep_alive);
  if (!ok) {
    groq_after_response(false, false);
    return false;
  }

  if (status != 200) {
    Serial.printf("ERR: STT HTTP %d\n", status);
    if (body) {
      Serial.write(body, min(body_len, (size_t)512));
      Serial.println();
      note_stt_rate_limit(status, body, body_len);
      free(body);
    }
    groq_after_response(false, false);
    return false;
  }

  groq_after_response(true, keep_alive);
  stt_last_call_ms = millis();
  stt_call_count++;
  transcript = String((const char*)body, body_len);
  transcript.trim();
  free(body);
  if (transcript.length() == 0) return false;
  Serial.printf("stt #%u: %s\n", stt_call_count, transcript.c_str());
  return true;
}

bool groq_post_multipart_stt(const uint8_t* wav, size_t wav_len, String& transcript) {
  if (groq_post_multipart_stt_once(wav, wav_len, transcript)) return true;
  Serial.println("stt: retry after fresh TLS...");
  stop_groq_tls();
  if (!ensure_groq_tls(true)) return false;
  return groq_post_multipart_stt_once(wav, wav_len, transcript);
}

void append_chat_message(const char* role, const char* content) {
  if (strcmp(role, "system") == 0) return;
  if (!content) content = "";
  char r = (strcmp(role, "assistant") == 0) ? 'a' : 'u';
  if (g_chat_hist_n >= CHAT_HIST_MAX) {
    for (uint8_t i = 1; i < CHAT_HIST_MAX; i++) {
      g_chat_hist[i - 1].role = g_chat_hist[i].role;
      g_chat_hist[i - 1].content = g_chat_hist[i].content;
    }
    g_chat_hist_n = CHAT_HIST_MAX - 1;
  }
  g_chat_hist[g_chat_hist_n].role = r;
  g_chat_hist[g_chat_hist_n].content = content;
  g_chat_hist_n++;
}

// Undo last append when LLM request fails after user was already recorded.
static void pop_last_chat_message_if_role(const char* role) {
  if (g_chat_hist_n == 0) return;
  char want = (strcmp(role, "assistant") == 0) ? 'a' : 'u';
  if (g_chat_hist[g_chat_hist_n - 1].role != want) return;
  g_chat_hist[g_chat_hist_n - 1].content = "";
  g_chat_hist_n--;
}

// Find end of first complete sentence in UTF-8 text. Returns byte index after
// terminator, or 0 if none yet. Ignores .!? inside quotes.
static size_t find_sentence_end(const String& s) {
  size_t n = s.length();
  if (n < 4) return 0;
  bool in_quote = false;
  char quote_ch = 0;
  size_t content = 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!in_quote && (c == '"' || c == '\'')) {
      in_quote = true;
      quote_ch = c;
      content++;
      continue;
    }
    if (in_quote) {
      if (c == quote_ch) in_quote = false;
      content++;
      continue;
    }
    if (c == '.' || c == '!' || c == '?') {
      if (content < 3) continue;
      // Don't split on trailing ? after a closing quote already consumed —
      // require the next non-space to not be lone punctuation-only leftovers:
      // absorb closing quotes/parens after the terminator.
      size_t end = i + 1;
      while (end < n && (s[end] == ' ' || s[end] == '\n' || s[end] == '"' ||
                         s[end] == '\'' || s[end] == ')' || s[end] == '?')) {
        end++;
      }
      return end;
    }
    if (c != ' ' && c != '\n' && c != '\t') content++;
  }
  return 0;
}

// True if text has at least one letter/digit/Hangul syllable (not punct-only).
static bool tts_has_speakable_content(const String& s) {
  for (size_t i = 0; i < s.length();) {
    uint8_t c = (uint8_t)s[i];
    size_t n = 1;
    if ((c & 0xF0) == 0xF0) n = 4;
    else if ((c & 0xE0) == 0xE0) n = 3;
    else if ((c & 0xC0) == 0xC0) n = 2;
    if (i + n > s.length()) break;
    if (n == 1) {
      if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        return true;
      }
    } else if (n == 3 && c >= 0xEA && c <= 0xED) {
      return true;  // Hangul
    } else if (n == 2 || (n == 3 && c != 0xE2)) {
      // Latin extended / CJK / etc. Skip E2… general punctuation.
      return true;
    }
    i += n;
  }
  return false;
}

static String extract_json_quoted_field(const String& payload, const char* key, int key_len) {
  int k = payload.indexOf(key);
  if (k < 0) return "";
  int colon = payload.indexOf(':', k + key_len);
  if (colon < 0) return "";
  String after = payload.substring(colon + 1);
  after.trim();
  if (!after.startsWith("\"")) return "";  // skip objects / null
  int q1 = payload.indexOf('"', colon + 1);
  if (q1 < 0) return "";
  String out;
  out.reserve(32);
  for (size_t i = (size_t)q1 + 1; i < payload.length(); i++) {
    char c = payload[i];
    if (c == '"') break;
    if (c == '\\' && i + 1 < payload.length()) {
      char n = payload[++i];
      if (n == 'n') out += '\n';
      else if (n == 't') out += '\t';
      else if (n == 'r') out += '\r';
      else if (n == '"' || n == '\\' || n == '/') out += n;
      else if (n == 'u' && i + 4 < payload.length()) {
        i += 4;
      } else {
        out += n;
      }
    } else {
      out += c;
    }
  }
  return out;
}

static String extract_sse_delta_content(const String& data_line) {
  // Gemini SSE uses "text"; Groq fallback uses "content".
  String payload = data_line;
  if (payload.startsWith("data:")) {
    payload = payload.substring(5);
    payload.trim();
  }
  if (payload.length() == 0 || payload == "[DONE]") return "";
  String t = extract_json_quoted_field(payload, "\"text\"", 6);
  if (t.length() > 0) return t;
  return extract_json_quoted_field(payload, "\"content\"", 9);
}

// SSE chat: parse optional {{…}} state envelope, then speak sentences as they complete.
// History stores spoken text only (never control markers). API failure does not mutate state.

// Model sometimes recites few-shot nouns as a multiple-choice quiz.
bool looks_like_en_fewshot_dump(const String& s) {
  int n = 0;
  if (s.indexOf("Apple") >= 0) n++;
  if (s.indexOf("Train") >= 0) n++;
  if (s.indexOf("Boat") >= 0) n++;
  if (s.indexOf("Dog") >= 0) n++;
  if (s.indexOf("Banana") >= 0) n++;
  return n >= 2;
}

bool groq_chat(const String& user_text, String& reply) {
  reply = "";
  g_turn_id++;
  const uint32_t turn = g_turn_id;

  // Snapshot for rollback if LLM fails after we only did deterministic pre-update.
  ConvState conv_snapshot = g_conv;

  UtterIntent intent = conv_pre_update(g_conv, user_text);
  conv_sync_legacy(g_conv, g_chat_mode);
  Serial.printf(
      "conv: turn=%u act=%s topic=%s expect=%s intent=%u clarify=%u\n", (unsigned)turn,
      activity_name(g_conv.activity), g_conv.topic[0] ? g_conv.topic : "-",
      expect_name(g_conv.expect), (unsigned)intent, (unsigned)g_conv.clarify_fails);

  chat_mode_tts_voice(g_chat_mode.mode, &tts_lang_override, &tts_voice_override);

  String canned;
  if (chat_mode_canned_reply(g_chat_mode.mode, user_text, canned)) {
    append_chat_message("user", user_text.c_str());
    reply = canned;
    append_chat_message("assistant", reply.c_str());
    speak_session_begin();
    bool ok = speak_text_ex(reply, false);
    speak_session_end();
    tts_lang_override = nullptr;
    tts_voice_override = nullptr;
    conv_leave_activity(g_conv, ACT_FREE);
    conv_sync_legacy(g_conv, g_chat_mode);
    Serial.printf("conv: canned safe spoke=%d\n", ok ? 1 : 0);
    return ok;
  }

  if ((g_conv.activity == ACT_EN || g_conv.activity == ACT_ZH) &&
      conv_child_hands_quiz_back(user_text)) {
    append_chat_message("user", user_text.c_str());
    reply = "그럼 네가 말해 봐!";
    append_chat_message("assistant", reply.c_str());
    speak_session_begin();
    bool ok = speak_text_ex(reply, true);
    speak_session_end();
    Serial.printf("conv: hands-back canned spoke=%d\n", ok ? 1 : 0);
    return ok;
  }

  if (!ensure_gemini_tls(false)) {
    g_conv = conv_snapshot;  // network fail — do not keep partial intent side-effects as "heard"
    conv_sync_legacy(g_conv, g_chat_mode);
    tts_lang_override = nullptr;
    tts_voice_override = nullptr;
    Serial.println("conv: llm tls fail — state restored");
    return false;
  }

  append_chat_message("user", user_text.c_str());

  String system_prompt = String(SYSTEM_PROMPT);
  const char* persona_extra = device_config_get().persona_extra;
  if (persona_extra && persona_extra[0]) {
    system_prompt += " ";
    system_prompt += persona_extra;
  }
  system_prompt += CONV_ENVELOPE_RULES;
  system_prompt += conv_state_prompt_line(g_conv);
  system_prompt += chat_mode_overlay(g_chat_mode.mode);

  JsonDocument doc;
  JsonObject si = doc["systemInstruction"].to<JsonObject>();
  JsonArray si_parts = si["parts"].to<JsonArray>();
  JsonObject si_p = si_parts.add<JsonObject>();
  si_p["text"] = system_prompt;

  JsonArray contents = doc["contents"].to<JsonArray>();
  auto add_turn = [&](const char* role, const char* text) {
    JsonObject o = contents.add<JsonObject>();
    o["role"] = role;
    JsonArray parts = o["parts"].to<JsonArray>();
    JsonObject p = parts.add<JsonObject>();
    p["text"] = text;
  };

  // Pattern few-shots with envelope (not a lexicon).
  if (g_conv.activity == ACT_EN) {
    add_turn("user", "문제 내줘");
    add_turn("model", "{{a=en;t=사과;e=word;p=ask;h=0;d=ask;x=Apple;f=}}사과는 영어로 뭐야?");
    add_turn("user", "사과");
    add_turn("model", "{{a=en;t=사과;e=word;p=ask;h=0;d=ans;x=Apple;f=}}Apple!");
    add_turn("user", "배는 영어로 뭐야");
    add_turn("model", "{{a=en;t=배;e=word;p=ask;h=0;d=ans;x=Boat;f=}}Boat!");
    add_turn("user", "아니 과일 배");
    add_turn("model", "{{a=en;t=배;e=word;p=ask;h=0;d=corr;x=Pear;f=}}Pear!");
    add_turn("user", "아니 니가 해");
    add_turn("model", "{{a=en;t=;e=open;p=play;h=0;d=ask;x=;f=}}그럼 네가 말해. 뭐를 영어로 할까?");
    add_turn("user", "몰라");
    add_turn("model", "{{a=en;t=사과;e=word;p=hint;h=1;d=hint;x=Apple;f=}}에, 에이로 시작해.");
    add_turn("user", "apple 한국말로?");
    add_turn("model", "{{a=en;t=apple;e=word;p=ask;h=0;d=ans;x=;f=}}사과!");
  } else if (g_conv.activity == ACT_ZH) {
    add_turn("user", "안녕이 중국어로 뭐야");
    add_turn("model", "{{a=zh;t=안녕;e=word;p=ask;h=0;d=ans;x=;f=}}你好!");
    add_turn("user", "그러면 고마워는?");
    add_turn("model", "{{a=zh;t=고마워;e=word;p=ask;h=0;d=ans;x=;f=}}谢谢!");
  }

  for (uint8_t i = 0; i < g_chat_hist_n; i++) {
    add_turn((g_chat_hist[i].role == 'a') ? "model" : "user", g_chat_hist[i].content.c_str());
  }
  Serial.printf("hist: %u msgs\n", (unsigned)g_chat_hist_n);

  JsonObject gc = doc["generationConfig"].to<JsonObject>();
  gc["maxOutputTokens"] = chat_mode_max_tokens(g_chat_mode.mode);
  gc["temperature"] = chat_mode_temperature(g_chat_mode.mode);
  JsonObject think = gc["thinkingConfig"].to<JsonObject>();
  think["thinkingBudget"] = 0;

  String body;
  serializeJson(doc, body);
  Serial.printf("llm: request model=%s act=%s user=\"%s\"\n", LLM_MODEL,
                activity_name(g_conv.activity), user_text.c_str());

  secure_client->printf(
      "POST /v1beta/models/%s:streamGenerateContent?alt=sse HTTP/1.1\r\n", LLM_MODEL);
  gemini_write_headers(body.length());
  secure_client->print(body);
  uint32_t t_req_sent = millis();

  request_tts_preconnect();
  Serial.printf("opener: deferred (play if TTFT > %ums)\n", (unsigned)OPENER_DEFER_MS);

  String status_line = secure_client->readStringUntil('\n');
  status_line.trim();
  if (!status_line.startsWith("HTTP/")) {
    Serial.printf("ERR: LLM bad status line: %s\n", status_line.c_str());
    pop_last_chat_message_if_role("user");
    g_conv = conv_snapshot;
    conv_sync_legacy(g_conv, g_chat_mode);
    groq_after_response(false, false);
    tts_lang_override = nullptr;
    tts_voice_override = nullptr;
    return false;
  }
  int status = status_line.substring(9, 12).toInt();
  bool chunked = false;
  bool keep_alive = true;
  int content_length = -1;
  auto read_llm_headers = [&]() {
    chunked = false;
    keep_alive = true;
    content_length = -1;
    while (secure_client->connected() || secure_client->available()) {
      String line = secure_client->readStringUntil('\n');
      line.trim();
      if (line.length() == 0) break;
      if (line.startsWith("Transfer-Encoding:") || line.startsWith("transfer-encoding:")) {
        if (line.indexOf("chunked") >= 0) chunked = true;
      } else if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
        content_length = line.substring(15).toInt();
      } else if (line.startsWith("Connection:") || line.startsWith("connection:")) {
        String lower = line;
        lower.toLowerCase();
        if (lower.indexOf("close") >= 0) keep_alive = false;
      }
    }
  };
  read_llm_headers();
  Serial.printf("llm: headers in %ums (HTTP %d)\n", (unsigned)(millis() - t_req_sent), status);

  if (status != 200) {
    Serial.printf("ERR: LLM HTTP %d\n", status);
    String err_body;
    err_body.reserve(512);
    uint32_t t0 = millis();
    while (secure_client->available() && millis() - t0 < 2000) {
      char c = (char)secure_client->read();
      Serial.write(c);
      if (err_body.length() < 500) err_body += c;
    }
    Serial.println();

    bool groq_ok = false;
    if (status == 403) {
      Serial.println("llm: Gemini API off — Groq fallback");
      groq_after_response(false, false);
      if (ensure_groq_tls(true)) {
        JsonDocument gdoc;
        JsonArray messages = gdoc["messages"].to<JsonArray>();
        auto add_oa = [&](const char* role, const char* text) {
          JsonObject o = messages.add<JsonObject>();
          o["role"] = role;
          o["content"] = text;
        };
        add_oa("system", system_prompt.c_str());
        if (g_conv.activity == ACT_EN) {
          add_oa("user", "문제 내줘");
          add_oa("assistant", "{{a=en;t=사과;e=word;p=ask;h=0;d=ask;x=Apple;f=}}사과는 영어로 뭐야?");
          add_oa("user", "사과");
          add_oa("assistant", "{{a=en;t=사과;e=word;p=ask;h=0;d=ans;x=Apple;f=}}Apple!");
          add_oa("user", "배는 영어로 뭐야");
          add_oa("assistant", "{{a=en;t=배;e=word;p=ask;h=0;d=ans;x=Boat;f=}}Boat!");
          add_oa("user", "아니 과일 배");
          add_oa("assistant", "{{a=en;t=배;e=word;p=ask;h=0;d=corr;x=Pear;f=}}Pear!");
          add_oa("user", "아니 니가 해");
          add_oa("assistant", "{{a=en;t=;e=open;p=play;h=0;d=ask;x=;f=}}그럼 네가 말해. 뭐를 영어로 할까?");
          add_oa("user", "몰라");
          add_oa("assistant", "{{a=en;t=사과;e=word;p=hint;h=1;d=hint;x=Apple;f=}}에, 에이로 시작해.");
          add_oa("user", "apple 한국말로?");
          add_oa("assistant", "{{a=en;t=apple;e=word;p=ask;h=0;d=ans;x=;f=}}사과!");
        } else if (g_conv.activity == ACT_ZH) {
          add_oa("user", "안녕이 중국어로 뭐야");
          add_oa("assistant", "{{a=zh;t=안녕;e=word;p=ask;h=0;d=ans;x=;f=}}你好!");
          add_oa("user", "그러면 고마워는?");
          add_oa("assistant", "{{a=zh;t=고마워;e=word;p=ask;h=0;d=ans;x=;f=}}谢谢!");
        }
        for (uint8_t i = 0; i < g_chat_hist_n; i++) {
          add_oa((g_chat_hist[i].role == 'a') ? "assistant" : "user", g_chat_hist[i].content.c_str());
        }
        gdoc["model"] = GROQ_LLM_MODEL;
        gdoc["max_tokens"] = chat_mode_max_tokens(g_chat_mode.mode);
        gdoc["temperature"] = chat_mode_temperature(g_chat_mode.mode);
        gdoc["reasoning_effort"] = GROQ_LLM_REASONING;
        gdoc["stream"] = true;
        String gbody;
        serializeJson(gdoc, gbody);
        Serial.printf("llm: fallback model=%s\n", GROQ_LLM_MODEL);
        secure_client->printf("POST /openai/v1/chat/completions HTTP/1.1\r\n");
        groq_write_headers("application/json", gbody.length());
        secure_client->print(gbody);
        t_req_sent = millis();
        status_line = secure_client->readStringUntil('\n');
        status_line.trim();
        if (status_line.startsWith("HTTP/")) {
          status = status_line.substring(9, 12).toInt();
          read_llm_headers();
          Serial.printf("llm: groq headers in %ums (HTTP %d)\n",
                        (unsigned)(millis() - t_req_sent), status);
          groq_ok = (status == 200);
        }
        if (!groq_ok) {
          Serial.printf("ERR: Groq fallback HTTP %d\n", status);
          t0 = millis();
          while (secure_client->available() && millis() - t0 < 1500) {
            Serial.write((char)secure_client->read());
          }
          Serial.println();
        }
      }
    }

    if (!groq_ok) {
      pop_last_chat_message_if_role("user");
      g_conv = conv_snapshot;
      conv_sync_legacy(g_conv, g_chat_mode);
      groq_after_response(false, false);
      tts_lang_override = nullptr;
      tts_voice_override = nullptr;
      uint32_t wait_ms = (status == 429) ? groq_retry_wait_ms(err_body) : 20000;
      stt_blocked_until_ms = millis() + wait_ms;
      Serial.printf("llm: unavailable HTTP %d — wait %us, speaking pause\n", status,
                    wait_ms / 1000);
      reply = "잠깐만. 조금 이따 하자.";
      speak_session_begin();
      speak_text_ex(reply, true);
      speak_session_end();
      return true;
    }
  }

  BodyReader br = {secure_client, chunked,
                   chunked ? 0 : (content_length > 0 ? (long)content_length : 100000000L),
                   false};
  String pending;
  String full;
  full.reserve(320);
  pending.reserve(160);
  bool spoke_any = false;
  String spoken_out;
  spoken_out.reserve(160);
  uint32_t t_first_tok = 0;
  uint32_t t_start = t_req_sent;
  bool opener_played = false;
  uint32_t opener_ms = 0;
  bool envelope_done = false;
  bool envelope_applied = false;
  String speak_buf;

  speak_session_begin();

  String line_buf;
  line_buf.reserve(256);
  uint8_t tmp[256];
  uint32_t idle_start = millis();

  // Toy: one short sentence per turn — avoids multi-TTS TLS churn + latency.
  auto flush_speakable = [&](bool force_tail) {
    if (turn != g_turn_id) return;  // stale turn guard
    if (spoke_any) {
      speak_buf = "";
      return;
    }
    size_t cut = find_sentence_end(speak_buf);
    if (cut > 0) {
      String sentence = speak_buf.substring(0, cut);
      sentence.trim();
      speak_buf = "";  // drop remainder — do not speak sentence 2+
        if (sentence.length() > 0) {
          sentence = conv_strip_envelope(sentence);
          sentence.trim();
        }
        if (looks_like_en_fewshot_dump(sentence)) {
          sentence = "사과는 영어로 뭐야?";
        }
        if (sentence.length() > 0 && sentence.indexOf("{{") < 0) {
        Serial.printf("llm: speak sentence: %s\n", sentence.c_str());
        if (speak_text_ex(sentence, true)) {
          spoke_any = true;
          spoken_out = sentence;
        }
      }
      return;
    }
    if (force_tail && !spoke_any) {
      speak_buf.trim();
      if (speak_buf.length() > 0) {
        speak_buf = conv_strip_envelope(speak_buf);
        speak_buf.trim();
      }
      if (speak_buf.length() > 0 && speak_buf.indexOf("{{") < 0) {
        if (looks_like_en_fewshot_dump(speak_buf)) {
          speak_buf = "사과는 영어로 뭐야?";
        }
        Serial.printf("llm: speak tail: %s\n", speak_buf.c_str());
        if (speak_text_ex(speak_buf, true)) {
          spoke_any = true;
          spoken_out = speak_buf;
        }
        speak_buf = "";
      }
    }
  };

  while (!br.done && millis() - t_start < 20000) {
    // After first spoken sentence, stop waiting on the rest of the stream.
    if (spoke_any) break;

    int n = body_reader_read(&br, tmp, sizeof(tmp));
    if (n <= 0) {
      if (br.done) break;
      if (millis() - idle_start > 12000) break;
      if (!opener_played && t_first_tok == 0 && millis() - t_start >= OPENER_DEFER_MS) {
        opener_played = true;
        uint32_t t_op = millis();
        Serial.println("opener: late filler (TTFT slow)");
        play_filler_chirp();
        opener_ms = millis() - t_op;
        Serial.printf("opener: filler done in %ums\n", (unsigned)opener_ms);
        idle_start = millis();
      }
      delay(1);
      continue;
    }
    idle_start = millis();
    for (int i = 0; i < n; i++) {
      char c = (char)tmp[i];
      if (c == '\r') continue;
      if (c == '\n') {
        if (line_buf.length() > 0) {
          String delta = extract_sse_delta_content(line_buf);
          if (delta.length() > 0) {
            if (t_first_tok == 0) {
              t_first_tok = millis() - t_start;
              Serial.printf("llm: first token in %ums (from POST, opener=%s)\n",
                            (unsigned)t_first_tok, opener_played ? "played" : "skipped");
            }
            full += delta;
            pending += delta;

            if (!envelope_done) {
              int close = pending.indexOf("}}");
              int open = pending.indexOf("{{");
              if (open == 0 && close > 0) {
                String speak_part;
                envelope_applied = conv_apply_envelope(g_conv, pending, speak_part);
                if (!envelope_applied && speak_part.length() == 0) {
                  // Malformed — do not corrupt state; speak nothing from meta.
                  speak_part = conv_strip_envelope(pending);
                }
                conv_sync_legacy(g_conv, g_chat_mode);
                envelope_done = true;
                pending = "";
                speak_buf = speak_part;
                flush_speakable(false);
              } else if (open < 0 && pending.length() > 24) {
                // Model skipped envelope — speak as plain text, keep pre-LLM state.
                Serial.println("conv: no envelope — keep prior state");
                envelope_done = true;
                speak_buf = pending;
                pending = "";
                flush_speakable(false);
              } else if (open > 0) {
                // Junk before envelope — drop leading junk once {{ arrives.
                pending = pending.substring(open);
              }
            } else {
              speak_buf += delta;
              flush_speakable(false);
            }
          }
        }
        line_buf = "";
      } else {
        if (line_buf.length() < 1500) line_buf += c;
      }
    }
  }

  if (!spoke_any) {
    if (!envelope_done) {
      String speak_part;
      if (pending.indexOf("{{") >= 0 && pending.indexOf("}}") > 0) {
        envelope_applied = conv_apply_envelope(g_conv, pending, speak_part);
        conv_sync_legacy(g_conv, g_chat_mode);
        speak_buf += speak_part;
      } else {
        Serial.println("conv: stream end without envelope — keep prior state");
        speak_buf += pending;
      }
      envelope_done = true;
    } else if (pending.length()) {
      speak_buf += pending;
    }
    flush_speakable(true);
  }

  speak_session_end();
  // Gemini socket cannot be reused for Groq STT — always drop, then preconnect Groq.
  stop_groq_tls();
  request_tls_preconnect();

  tts_lang_override = nullptr;
  tts_voice_override = nullptr;

  if (spoken_out.length() == 0) {
    spoken_out = conv_strip_envelope(full);
    spoken_out.trim();
    // Keep only the first sentence in history if model over-generated.
    size_t cut = find_sentence_end(spoken_out);
    if (cut > 0) spoken_out = spoken_out.substring(0, cut);
    spoken_out.trim();
  }
  reply = spoken_out;
  if (reply.length() == 0) {
    Serial.printf("llm: empty speakable — fallback (first_tok=%u env=%d)\n",
                  (unsigned)t_first_tok, envelope_applied ? 1 : 0);
    reply = "응?";
    speak_session_begin();
    speak_text_ex(reply, true);
    speak_session_end();
  }
  // History = what the child could hear (spoken), not raw envelope.
  append_chat_message("assistant", reply.c_str());
  Serial.printf("llm: %s (spoke=%d, %ums, ttft=%ums opener_ms=%u env=%d)\n", reply.c_str(),
                spoke_any ? 1 : 0, (unsigned)(millis() - t_start), (unsigned)t_first_tok,
                (unsigned)opener_ms, envelope_applied ? 1 : 0);
  return true;
}

String english_for_tts(const String& text) {
  String out;
  out.reserve(text.length());
  bool prev_space = true;
  for (size_t i = 0; i < text.length(); i++) {
    unsigned char c = (unsigned char)text[i];
    if (c >= 32 && c <= 126) {
      out += (char)c;
      prev_space = false;
    } else if ((c == '\n' || c == '\t' || c == ' ') && !prev_space) {
      out += ' ';
      prev_space = true;
    }
  }
  out.trim();
  if (out.length() == 0) {
    out = "Sorry, I did not catch that.";
  }
  return out;
}

String truncate_utf8(const String& text, size_t max_bytes) {
  if (text.length() <= max_bytes) return text;
  size_t i = 0;
  size_t last = 0;
  while (i < text.length()) {
    uint8_t c = (uint8_t)text[i];
    size_t n = 1;
    if ((c & 0xF0) == 0xF0) n = 4;
    else if ((c & 0xE0) == 0xE0) n = 3;
    else if ((c & 0xC0) == 0xC0) n = 2;
    if (i + n > max_bytes) break;
    last = i + n;
    i += n;
  }
  return text.substring(0, last);
}

String ssml_escape(const String& text) {
  String out;
  out.reserve(text.length() + 16);
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

static void replace_all_ci(String& s, const char* from, const char* to) {
  String lower = s;
  lower.toLowerCase();
  String needle = from;
  needle.toLowerCase();
  int from_len = (int)strlen(from);
  int pos = 0;
  while ((pos = lower.indexOf(needle, pos)) >= 0) {
    s = s.substring(0, pos) + to + s.substring(pos + from_len);
    lower = s;
    lower.toLowerCase();
    pos += (int)strlen(to);
  }
}

// Collapse runs of ㅋ/ㅎ into a speakable laugh, and map emoji/symbols → vocal SFX.
// Never leave glyphs Chirp3 would read aloud (물결표, 별표, …).
String vocalize_for_tts(const String& text) {
  String s = text;

  // Explicit emoji / symbol → Korean vocalizations Chirp3 can speak.
  static const char* pairs[][2] = {
      {"😂", " 하하하 "}, {"🤣", " 하하하 "}, {"😆", " 히히 "}, {"😄", " 히히 "},
      {"😊", " 히히 "},   {"😁", " 히히 "},   {"😃", " 하하 "}, {"🙂", " "},
      {"😉", " 히히 "},   {"😍", " 헤헤 "},   {"🥰", " 헤헤 "}, {"😘", " 쪽 "},
      {"❤️", " "},        {"💕", " "},        {"💖", " "},      {"💗", " "},
      {"😢", " 흑 "},     {"😭", " 흑흑 "},   {"😔", " 음 "},   {"😞", " 음 "},
      {"😮", " 어? "},    {"😯", " 어? "},    {"😲", " 우와 "}, {"🤩", " 우와 "},
      {"🤔", " 음 "},     {"😴", " 쿨쿨 "},   {"💤", " 쿨쿨 "}, {"🔥", " "},
      {"⭐", " "},        {"✨", " "},        {"🎉", " 예이 "}, {"👏", " 짝짝 "},
      {"👍", " "},        {"👎", " "},
      {"♡", " 헤헤 "},    {"♥", " 헤헤 "},    {"♪", " 랄라 "},  {"♫", " 랄라 "},
      {"★", " "},        {"☆", " "},
      // Stage directions in paren → spoken affect (then strip remaining brackets later).
      {"(웃음)", " 하하 "}, {"(웃)", " 하하 "}, {"(히히)", " 히히 "},
      {"(울음)", " 흑 "}, {"(울)", " 흑 "}, {"(감탄)", " 우와 "},
      {nullptr, nullptr},
  };
  for (int i = 0; pairs[i][0]; i++) {
    s.replace(pairs[i][0], pairs[i][1]);
  }

  // Chat emoticons / tears (before dropping ^ _ etc.).
  static const char* emo_from[] = {
      "^_^", "^-^", "^^", "ㅠㅠ", "ㅜㅜ", "ㅠㅜ", "ㅜㅠ", "ㅡㅡ", "T_T", "t_t",
      nullptr,
  };
  static const char* emo_to[] = {
      " 히히 ", " 히히 ", " 히히 ", " 흑흑 ", " 흑흑 ", " 흑 ", " 흑 ", " 흥 ", " 흑 ", " 흑 ",
  };
  for (int i = 0; emo_from[i]; i++) {
    while (s.indexOf(emo_from[i]) >= 0) s.replace(emo_from[i], emo_to[i]);
  }

  // Tildes / wave dashes — TTS reads these as "물결표". Soften → drop (cute trailing tone).
  // Also fullwidth ～ (EF BC 9E), wave 〜 (E3 80 9C), tilde operator ∼ (E2 88 BC).
  while (s.indexOf('~') >= 0) s.replace("~", "");
  s.replace("\xEF\xBC\x9E", "");  // ～
  s.replace("\xE3\x80\x9C", "");  // 〜
  s.replace("\xE2\x88\xBC", "");  // ∼

  // Text laughs / chat slang → spoken onomatopoeia (not "ㅋ" spelled out).
  // Longest first.
  const char* laughs_from[] = {
      "ㅋㅋㅋㅋㅋ", "ㅋㅋㅋㅋ", "ㅋㅋㅋ", "ㅋㅋ", "ㅋ",
      "ㅎㅎㅎㅎㅎ", "ㅎㅎㅎㅎ", "ㅎㅎㅎ", "ㅎㅎ", "ㅎ",
      "하하하하", "하하하", "히히히", "호호호", "푸하하", "깔깔깔",
      "hahaha", "hahah", "haha", "hehehe", "hehe", "lolol", "lmao", "lol",
      nullptr,
  };
  const char* laughs_to[] = {
      " 하하하 ", " 하하하 ", " 하하 ", " 히히 ", " 히 ",
      " 히히 ", " 히히 ", " 히히 ", " 히히 ", " 히 ",
      " 하하하 ", " 하하하 ", " 히히 ", " 호호 ", " 푸하하 ", " 깔깔 ",
      " 하하하 ", " 하하하 ", " 하하 ", " 히히 ", " 히히 ", " 하하 ", " 하하하 ", " 하하 ",
  };
  for (int i = 0; laughs_from[i]; i++) {
    while (s.indexOf(laughs_from[i]) >= 0) {
      s.replace(laughs_from[i], laughs_to[i]);
    }
  }
  replace_all_ci(s, "hahaha", " 하하하 ");
  replace_all_ci(s, "haha", " 하하 ");
  replace_all_ci(s, "hehe", " 히히 ");
  replace_all_ci(s, "lol", " 하하 ");
  replace_all_ci(s, "lmao", " 하하하 ");

  // Drop leftover emoji / decorative symbols. Keep letters, Hangul, and prosody .!?,'-
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length();) {
    uint8_t c = (uint8_t)s[i];
    size_t n = 1;
    if ((c & 0xF0) == 0xF0) n = 4;
    else if ((c & 0xE0) == 0xE0) n = 3;
    else if ((c & 0xC0) == 0xC0) n = 2;
    if (i + n > s.length()) break;

    bool drop = false;
    if (n == 1) {
      // Decorative ASCII — never speak name of mark.
      if (c == '`' || c == '^' || c == '*' || c == '_' || c == '#' || c == '@' ||
          c == '$' || c == '%' || c == '&' || c == '|' || c == '\\' || c == '/' ||
          c == '=' || c == '+' || c == '<' || c == '>' || c == '[' || c == ']' ||
          c == '{' || c == '}' || c == '(' || c == ')' || c == '"' || c == ';' ||
          c == ':' || c == '~') {
        drop = true;
      }
    } else if (n == 4) {
      drop = true;  // emoji / rare symbols
    } else if (n == 3) {
      uint8_t b1 = (uint8_t)s[i + 1];
      uint8_t b2 = (uint8_t)s[i + 2];
      // General punctuation / dingbats (E2…), CJK symbols (E3 80 xx except ideographs),
      // fullwidth punct often EF BC / EF BD.
      if (c == 0xE2) {
        drop = true;  // … — • ✓ ✗ ♪ etc. (ellipsis handled earlier as ", ")
      } else if (c == 0xE3 && b1 == 0x80) {
        // Ideographic space/punct: 、。〈〉《》「」『』〜 etc.
        if (!(b2 == 0x81 || b2 == 0x82)) drop = true;  // keep 、。 as soft pause below
        if (b2 == 0x81 || b2 == 0x82) {
          // map to ASCII pause instead of raw
          out += ", ";
          i += n;
          continue;
        }
      } else if (c == 0xEF && (b1 == 0xBC || b1 == 0xBD)) {
        // Fullwidth ASCII block — drop decorative; keep fullwidth !?． if useful
        // EF BC 81=！ EF BC 9F=？ EF BC 8E=． EF BC 8C=，
        if (b1 == 0xBC && (b2 == 0x81 || b2 == 0x9F)) {
          out += (b2 == 0x81) ? '!' : '?';
          i += n;
          continue;
        }
        if (b1 == 0xBC && (b2 == 0x8E || b2 == 0x8C)) {
          out += (b2 == 0x8E) ? '.' : ',';
          i += n;
          continue;
        }
        drop = true;
      }
    }

    if (!drop) {
      for (size_t k = 0; k < n; k++) out += s[i + k];
    } else {
      out += ' ';
    }
    i += n;
  }

  while (out.indexOf("  ") >= 0) out.replace("  ", " ");
  out.trim();
  return out;
}

String sanitize_tts_text(const String& text) {
  String out = conv_strip_envelope(text);
  out = vocalize_for_tts(out);
  out.replace("\r", " ");
  out.replace("\n", " ");
  out.replace("\t", " ");
  // Don't let TTS spell punctuation clusters awkwardly.
  while (out.indexOf("!!") >= 0) out.replace("!!", "!");
  while (out.indexOf("??") >= 0) out.replace("??", "?");
  while (out.indexOf("…") >= 0) out.replace("…", ", ");
  while (out.indexOf("...") >= 0) out.replace("...", ", ");
  // Any leftover wave/tilde variants
  while (out.indexOf('~') >= 0) out.replace("~", "");
  out.replace("\xEF\xBC\x9E", "");
  out.replace("\xE3\x80\x9C", "");
  out.replace("\xE2\x88\xBC", "");
  while (out.indexOf("  ") >= 0) out.replace("  ", " ");
  out.trim();
  // Empty / punct-only → caller skips TTS (don't speak lone "?" or emoji tails).
  if (out.length() == 0 || !tts_has_speakable_content(out)) return String();
  return truncate_utf8(out, TTS_MAX_CHARS);
}

int body_reader_read(BodyReader* br, uint8_t* dst, size_t want) {
  if (br->done) return 0;
  uint32_t start = millis();
  while (millis() - start < 15000) {
    if (br->chunked && br->remaining == 0) {
      String line = br->client->readStringUntil('\n');
      line.trim();
      if (line.length() == 0) {
        if (!br->client->connected() && !br->client->available()) {
          br->done = true;
          return 0;
        }
        continue;  // CRLF between chunks
      }
      long sz = strtol(line.c_str(), nullptr, 16);
      if (sz <= 0) {
        br->done = true;
        return 0;
      }
      br->remaining = sz;
    }
    if (!br->chunked && br->remaining <= 0) {
      br->done = true;
      return 0;
    }
    size_t take = want;
    if ((long)take > br->remaining) take = (size_t)br->remaining;
    int n = br->client->read(dst, take);
    if (n > 0) {
      br->remaining -= n;
      return n;
    }
    if (!br->client->connected() && !br->client->available()) {
      br->done = true;
      return 0;
    }
    delay(1);
  }
  br->done = true;
  return 0;
}

bool body_read_exact(BodyReader* br, uint8_t* dst, size_t n) {
  size_t got = 0;
  while (got < n) {
    int r = body_reader_read(br, dst + got, n - got);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return true;
}

void close_tts_or_groq(WiFiClientSecure* client) {
  if (client == tts_client) stop_tts_tls();
  else stop_groq_tls();
}

// Returns 1 = played fully, 0 = failed before any audio (fallback ok),
// -1 = failed mid-playback (do NOT fall back, audio partially spoken).
int groq_tts_stream_play(const String& text) {
  // tts_client is reserved for Google; use primary Groq socket for Orpheus fallback.
  WiFiClientSecure* client = secure_client;
  if (!ensure_groq_tls(false)) return 0;
  uint32_t t_req = millis();

  JsonDocument doc;
  doc["model"] = TTS_MODEL;
  doc["voice"] = TTS_VOICE;
  doc["input"] = text;
  doc["response_format"] = "wav";
  String body;
  serializeJson(doc, body);

  client->printf("POST /openai/v1/audio/speech HTTP/1.1\r\n");
  groq_write_headers_on(client, "application/json", body.length());
  client->print(body);

  String status_line = client->readStringUntil('\n');
  status_line.trim();
  if (!status_line.startsWith("HTTP/")) {
    if (client == tts_client) stop_tts_tls();
    else stop_groq_tls();
    return 0;
  }
  int status = status_line.substring(9, 12).toInt();
  int content_length = -1;
  bool chunked = false;
  while (client->connected() || client->available()) {
    String line = client->readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
    if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
      content_length = line.substring(15).toInt();
    } else if (line.startsWith("Transfer-Encoding:") || line.startsWith("transfer-encoding:")) {
      if (line.indexOf("chunked") >= 0) chunked = true;
    }
  }
  if (status != 200) {
    Serial.printf("ERR: Groq TTS(stream) HTTP %d\n", status);
    if (client == tts_client) stop_tts_tls();
    else stop_groq_tls();
    return 0;
  }

  BodyReader br = {client, chunked, chunked ? 0 : (long)content_length, false};

  uint8_t hdr[12];
  if (!body_read_exact(&br, hdr, 12) || memcmp(hdr, "RIFF", 4) != 0 ||
      memcmp(hdr + 8, "WAVE", 4) != 0) {
    if (client == tts_client) stop_tts_tls();
    else stop_groq_tls();
    return 0;
  }

  uint32_t src_rate = SAMPLE_RATE;
  uint16_t channels = 1;
  for (;;) {
    uint8_t ch[8];
    if (!body_read_exact(&br, ch, 8)) {
      close_tts_or_groq(client);
      return 0;
    }
    uint32_t sz = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
    if (memcmp(ch, "fmt ", 4) == 0) {
      uint8_t fmt[64];
      if (sz > sizeof(fmt) || !body_read_exact(&br, fmt, sz)) {
        close_tts_or_groq(client);
        return 0;
      }
      channels = fmt[2] | (fmt[3] << 8);
      src_rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
      if (channels == 0) channels = 1;
    } else if (memcmp(ch, "data", 4) == 0) {
      break;
    } else {
      uint8_t skip[64];
      while (sz > 0) {
        size_t take = min((size_t)sz, sizeof(skip));
        if (!body_read_exact(&br, skip, take)) {
          close_tts_or_groq(client);
          return 0;
        }
        sz -= take;
      }
    }
  }

  if (!begin_i2s_tx()) {
    close_tts_or_groq(client);
    return 0;
  }

  const size_t SRC_BYTES = 4096;
  const size_t OUT_FRAMES = 2048;
  uint8_t* src = (uint8_t*)malloc(SRC_BYTES);
  int32_t* out = (int32_t*)malloc(OUT_FRAMES * 2 * sizeof(int32_t));
  if (!src || !out) {
    if (src) free(src);
    if (out) free(out);
    i2s.end();
    close_tts_or_groq(client);
    return 0;
  }

  const size_t frame_bytes = 2 * channels;
  // 20-bit fixed-point resample position, carried across buffers
  uint32_t step = (uint32_t)(((uint64_t)src_rate << 12) / SAMPLE_RATE);
  uint32_t pos = 0;
  size_t carry = 0;
  bool speaker_started = false;
  bool net_error = false;

  for (;;) {
    int n = body_reader_read(&br, src + carry, SRC_BYTES - carry);
    if (n <= 0) {
      if (!br.done) net_error = true;
      break;
    }
    size_t have = carry + (size_t)n;
    size_t frames = have / frame_bytes;
    if (frames == 0) {
      carry = have;
      continue;
    }

    const int16_t* samples = (const int16_t*)src;
    while ((pos >> 12) < frames) {
      size_t out_n = 0;
      while ((pos >> 12) < frames && out_n < OUT_FRAMES) {
        int16_t s = samples[(pos >> 12) * channels];
        int32_t v = pcm16_to_i2s32(s);
        out[out_n * 2] = v;
        out[out_n * 2 + 1] = v;
        out_n++;
        pos += step;
      }
      if (!speaker_started) {
        set_speaker(true);
        speaker_started = true;
        Serial.printf("tts: first audio in %ums (streamed, %uHz ch=%u)\n",
                      (unsigned)(millis() - t_req), (unsigned)src_rate, channels);
      }
      uint8_t* p = (uint8_t*)out;
      size_t total_bytes = out_n * 2 * sizeof(int32_t);
      size_t off = 0;
      while (off < total_bytes) {
        size_t w = i2s.write(p + off, total_bytes - off);
        if (w == 0) delay(1);
        off += w;
      }
    }
    pos -= ((uint32_t)frames << 12);

    size_t used = frames * frame_bytes;
    carry = have - used;
    if (carry > 0) memmove(src, src + used, carry);
  }

  free(src);
  free(out);
  i2s_tx_soft_stop(speaker_started);

  if (net_error) {
    Serial.println("WARN: TTS stream interrupted");
    close_tts_or_groq(client);
    return speaker_started ? -1 : 0;
  }

  if (client == tts_client) {
    tts_tls_last_ok_ms = millis();
  } else {
    groq_after_response(true, true);
  }
  Serial.printf("tts: stream done (%ums total)\n", (unsigned)(millis() - t_req));
  return 1;
}

bool groq_tts(const String& text, uint8_t** wav_out, size_t* wav_len) {
  if (!ensure_groq_tls(false)) return false;

  JsonDocument doc;
  doc["model"] = TTS_MODEL;
  doc["voice"] = TTS_VOICE;
  doc["input"] = text;
  doc["response_format"] = "wav";

  String body;
  serializeJson(doc, body);

  secure_client->printf("POST /openai/v1/audio/speech HTTP/1.1\r\n");
  groq_write_headers("application/json", body.length());
  secure_client->print(body);

  int status = 0;
  uint8_t* resp = nullptr;
  size_t resp_len = 0;
  bool keep_alive = false;
  bool ok = read_http_response(secure_client, &status, &resp, &resp_len, &keep_alive);
  if (!ok) {
    groq_after_response(false, false);
    return false;
  }

  if (status != 200 || resp_len == 0) {
    Serial.printf("ERR: Groq TTS HTTP %d (%u bytes)\n", status, (unsigned)resp_len);
    if (resp) {
      Serial.write(resp, min(resp_len, (size_t)512));
      Serial.println();
      free(resp);
    }
    groq_after_response(false, false);
    return false;
  }

  groq_after_response(true, keep_alive);
  *wav_out = resp;
  *wav_len = resp_len;
  Serial.printf("tts: %u bytes\n", (unsigned)resp_len);
  return true;
}

bool extract_json_string_value(const uint8_t* body, size_t body_len, const char* key, char** out,
                               size_t* out_len) {
  if (!body || !key || !out || !out_len) return false;
  *out = nullptr;
  *out_len = 0;

  const char* begin = (const char*)body;
  const char* end = begin + body_len;
  String needle = String("\"") + key + "\"";
  size_t nlen = needle.length();
  const char* p = nullptr;
  for (const char* s = begin; s + nlen <= end; s++) {
    if (memcmp(s, needle.c_str(), nlen) == 0) {
      p = s;
      break;
    }
  }
  if (!p) return false;
  p += nlen;
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
  if (p >= end || *p != '"') return false;
  p++;  // opening quote

  const char* q = p;
  while (q < end && *q != '"') q++;
  if (q >= end) return false;

  size_t n = (size_t)(q - p);
  char* buf = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (char*)malloc(n + 1);
  if (!buf) return false;
  memcpy(buf, p, n);
  buf[n] = 0;
  *out = buf;
  *out_len = n;
  return true;
}

// Stream Google Cloud TTS: Chirp3 HD LINEAR16 @ device rate, stream-play to I2S.
bool google_cloud_tts_stream_play(const String& text) {
  if (strlen(active_google_key()) == 0) return false;
  if (!ensure_tts_tls(false)) return false;

  WiFiClientSecure* client = tts_client;

  // Chirp3 HD: plain text (SSML prosody unsupported / degraded). No pitch lift.
  JsonDocument doc;
  doc["input"]["text"] = text;
  doc["voice"]["languageCode"] = effective_tts_lang();
  doc["voice"]["name"] = effective_tts_voice();
  doc["audioConfig"]["audioEncoding"] = "LINEAR16";
  doc["audioConfig"]["sampleRateHertz"] = (int)TTS_SAMPLE_RATE;

  String body;
  serializeJson(doc, body);
  String path = String("/v1/text:synthesize?key=") + active_google_key();

  uint32_t t_req = millis();
  client->printf("POST %s HTTP/1.1\r\n", path.c_str());
  client->print("Host: texttospeech.googleapis.com\r\n");
  client->print("Content-Type: application/json\r\n");
  client->print("Connection: keep-alive\r\n");
  client->printf("Content-Length: %u\r\n\r\n", (unsigned)body.length());
  if (client->print(body) != body.length()) {
    stop_tts_tls();
    return false;
  }

  String status_line = client->readStringUntil('\n');
  status_line.trim();
  if (!status_line.startsWith("HTTP/")) {
    stop_tts_tls();
    return false;
  }
  int status = status_line.substring(9, 12).toInt();
  int content_length = -1;
  bool chunked = false;
  while (client->connected() || client->available()) {
    String line = client->readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
    if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
      content_length = line.substring(15).toInt();
    } else if (line.startsWith("Transfer-Encoding:") || line.startsWith("transfer-encoding:")) {
      if (line.indexOf("chunked") >= 0) chunked = true;
    }
  }
  if (status != 200) {
    Serial.printf("ERR: Google TTS(stream) HTTP %d\n", status);
    stop_tts_tls();
    return false;
  }

  BodyReader br = {client, chunked,
                   chunked ? 0 : (content_length > 0 ? (long)content_length : 100000000L),
                   false};

  const char* needle = "\"audioContent\"";
  size_t ni = 0;
  bool in_b64 = false;
  bool found_key = false;
  B64Stream b64 = {{0}, 0};

  // LINEAR16 usually arrives as WAV (RIFF). Buffer header, then stream PCM16.
  uint8_t hdr_buf[256];
  size_t hdr_n = 0;
  bool header_done = false;
  uint32_t src_rate = TTS_SAMPLE_RATE;
  uint16_t channels = 1;

  uint8_t pcm_carry[4];
  size_t pcm_carry_n = 0;
  uint8_t raw_buf[512];
  size_t raw_n = 0;

  bool got_audio = false;
  const size_t OUT_FRAMES = 512;
  int32_t* out = (int32_t*)malloc(OUT_FRAMES * 2 * sizeof(int32_t));
  if (!out) {
    stop_tts_tls();
    return false;
  }

  // 12-bit fixed-point resample position (TTS_SAMPLE_RATE -> SAMPLE_RATE)
  uint32_t step = (uint32_t)(((uint64_t)TTS_SAMPLE_RATE << 12) / SAMPLE_RATE);
  uint32_t pos = 0;

  auto write_out_frames = [&](size_t frames) {
    if (!speak_speaker_on) {
      set_speaker(true);
      speak_speaker_on = true;
      Serial.printf("tts: first audio in %ums (chirp3 LINEAR16 %uHz)\n",
                    (unsigned)(millis() - t_req), (unsigned)src_rate);
    }
    got_audio = true;
    uint8_t* p = (uint8_t*)out;
    size_t total_bytes = frames * 2 * sizeof(int32_t);
    size_t woff = 0;
    while (woff < total_bytes) {
      size_t w = i2s.write(p + woff, total_bytes - woff);
      if (w == 0) delay(1);
      woff += w;
    }
  };

  auto flush_pcm16 = [&](const uint8_t* data, size_t n) {
    // Prepend odd-byte carry so we always process whole samples.
    uint8_t tmp_pcm[520];
    size_t have = 0;
    if (pcm_carry_n > 0) {
      memcpy(tmp_pcm, pcm_carry, pcm_carry_n);
      have = pcm_carry_n;
      pcm_carry_n = 0;
    }
    memcpy(tmp_pcm + have, data, n);
    have += n;
    size_t usable = have & ~((size_t)1);
    if (have > usable) {
      pcm_carry[0] = tmp_pcm[usable];
      pcm_carry_n = 1;
    }
    if (usable < 2) return;

    const int16_t* samples = (const int16_t*)tmp_pcm;
    size_t frames = usable / 2;
    if (channels > 1) frames /= channels;

    while ((pos >> 12) < frames) {
      size_t out_n = 0;
      while ((pos >> 12) < frames && out_n < OUT_FRAMES) {
        size_t si = (size_t)(pos >> 12);
        int16_t s = samples[si * channels];
        int32_t v = pcm16_to_i2s32(s);
        out[out_n * 2] = v;
        out[out_n * 2 + 1] = v;
        out_n++;
        pos += step;
      }
      write_out_frames(out_n);
    }
    pos -= ((uint32_t)frames << 12);
  };

  auto feed_decoded = [&](const uint8_t* chunk, size_t n) -> bool {
    size_t off = 0;
    while (off < n) {
      if (!header_done) {
        size_t take = min(n - off, sizeof(hdr_buf) - hdr_n);
        memcpy(hdr_buf + hdr_n, chunk + off, take);
        hdr_n += take;
        off += take;

        if (hdr_n < 12) continue;
        if (memcmp(hdr_buf, "RIFF", 4) != 0) {
          // Raw LINEAR16 — treat entire buffer as PCM.
          header_done = true;
          if (!ensure_speak_i2s()) return false;
          flush_pcm16(hdr_buf, hdr_n);
          hdr_n = 0;
          continue;
        }
        // Parse WAV chunks until "data"
        size_t cursor = 12;
        while (cursor + 8 <= hdr_n) {
          const char* id = (const char*)(hdr_buf + cursor);
          uint32_t sz = hdr_buf[cursor + 4] | (hdr_buf[cursor + 5] << 8) |
                        (hdr_buf[cursor + 6] << 16) | ((uint32_t)hdr_buf[cursor + 7] << 24);
          cursor += 8;
          if (memcmp(id, "fmt ", 4) == 0 && cursor + sz <= hdr_n && sz >= 16) {
            channels = hdr_buf[cursor + 2] | (hdr_buf[cursor + 3] << 8);
            src_rate = hdr_buf[cursor + 4] | (hdr_buf[cursor + 5] << 8) |
                       (hdr_buf[cursor + 6] << 16) | ((uint32_t)hdr_buf[cursor + 7] << 24);
            if (channels == 0) channels = 1;
            if (src_rate == 0) src_rate = TTS_SAMPLE_RATE;
            step = (uint32_t)(((uint64_t)src_rate << 12) / SAMPLE_RATE);
            cursor += sz + (sz & 1);
          } else if (memcmp(id, "data", 4) == 0) {
            header_done = true;
            if (!ensure_speak_i2s()) return false;
            size_t pcm_in_hdr = hdr_n - cursor;
            if (pcm_in_hdr > 0) flush_pcm16(hdr_buf + cursor, pcm_in_hdr);
            hdr_n = 0;
            break;
          } else {
            if (cursor + sz > hdr_n) break;  // need more header bytes
            cursor += sz + (sz & 1);
          }
        }
        if (!header_done && hdr_n >= sizeof(hdr_buf)) {
          Serial.println("ERR: Google TTS WAV header too large");
          return false;
        }
      } else {
        flush_pcm16(chunk + off, n - off);
        off = n;
      }
    }
    return true;
  };

  bool b64_done = false;
  uint8_t tmp[256];
  uint32_t t0 = millis();
  while (!br.done && !b64_done && millis() - t0 < 30000) {
    int n = body_reader_read(&br, tmp, sizeof(tmp));
    if (n <= 0) {
      if (br.done) break;
      delay(1);
      continue;
    }
    for (int i = 0; i < n; i++) {
      char c = (char)tmp[i];
      if (!found_key) {
        if (c == needle[ni]) {
          ni++;
          if (needle[ni] == 0) {
            found_key = true;
            ni = 0;
          }
        } else {
          ni = (c == needle[0]) ? 1 : 0;
        }
        continue;
      }
      if (!in_b64) {
        if (c == '"') in_b64 = true;
        continue;
      }
      if (c == '"') {
        if (raw_n > 0) {
          if (!feed_decoded(raw_buf, raw_n)) {
            free(out);
            if (!speak_session_open) speak_session_end();
            stop_tts_tls();
            return false;
          }
          raw_n = 0;
        }
        b64_done = true;
        break;
      }
      uint8_t decoded[3];
      int dn = b64_feed(&b64, c, decoded);
      if (dn > 0) {
        for (int d = 0; d < dn; d++) {
          raw_buf[raw_n++] = decoded[d];
          if (raw_n >= sizeof(raw_buf)) {
            if (!feed_decoded(raw_buf, raw_n)) {
              free(out);
              if (!speak_session_open) speak_session_end();
              stop_tts_tls();
              return false;
            }
            raw_n = 0;
          }
        }
      }
    }
  }

  if (raw_n > 0) {
    if (!feed_decoded(raw_buf, raw_n)) {
      free(out);
      if (!speak_session_open) speak_session_end();
      stop_tts_tls();
      return false;
    }
  }

  free(out);

  // Fully drain leftover JSON (`}`) so we don't leave unread bytes.
  uint32_t drain_t = millis();
  while (!br.done && millis() - drain_t < 2000) {
    uint8_t junk[128];
    int n = body_reader_read(&br, junk, sizeof(junk));
    if (n <= 0) {
      if (!client->connected() && !client->available()) break;
      delay(1);
      continue;
    }
  }

  if (!got_audio) {
    if (!speak_session_open) speak_session_end();
    stop_tts_tls();
    return false;
  }

  // Chirp3 often leaves the socket unusable for the next POST — always close.
  // Never background-preconnect while a speak session may call ensure_tts_tls
  // immediately (core0/core1 double-connect → heap corruption).
  stop_tts_tls();
  if (!speak_session_open) {
    request_tts_preconnect();
    delay(30);
    speak_session_end();
  }

  Serial.printf("tts: google stream done (%ums)\n", (unsigned)(millis() - t_req));
  return true;
}

bool google_cloud_tts(const String& text, uint8_t** audio_out, size_t* audio_len, bool* is_mp3) {
  if (is_mp3) *is_mp3 = false;
  if (strlen(active_google_key()) == 0) {
    Serial.println("ERR: GOOGLE_API empty — set in .env / control panel");
    return false;
  }
  if (!ensure_tts_tls(false)) {
    Serial.println("ERR: Google TTS TLS failed");
    return false;
  }

  WiFiClientSecure* client = tts_client;

  // Chirp3 HD: plain text + LINEAR16 @ device rate (no pitch/SSML).
  JsonDocument doc;
  doc["input"]["text"] = text;
  doc["voice"]["languageCode"] = effective_tts_lang();
  doc["voice"]["name"] = effective_tts_voice();
  doc["audioConfig"]["audioEncoding"] = "LINEAR16";
  doc["audioConfig"]["sampleRateHertz"] = (int)TTS_SAMPLE_RATE;

  String body;
  serializeJson(doc, body);
  String path = String("/v1/text:synthesize?key=") + active_google_key();

  uint32_t t_req = millis();
  client->printf("POST %s HTTP/1.1\r\n", path.c_str());
  client->print("Host: texttospeech.googleapis.com\r\n");
  client->print("Content-Type: application/json\r\n");
  client->print("Connection: keep-alive\r\n");
  client->printf("Content-Length: %u\r\n\r\n", (unsigned)body.length());
  if (client->print(body) != body.length()) {
    Serial.println("ERR: Google TTS body write incomplete");
    stop_tts_tls();
    return false;
  }

  int status = 0;
  uint8_t* resp = nullptr;
  size_t resp_len = 0;
  if (!read_http_response(client, &status, &resp, &resp_len, nullptr)) {
    Serial.println("ERR: Google TTS HTTP read failed");
    stop_tts_tls();
    return false;
  }
  if (status != 200) {
    Serial.printf("ERR: Google TTS HTTP %d (%u bytes)\n", status, (unsigned)resp_len);
    if (resp && resp_len > 0) {
      size_t show = resp_len < 320 ? resp_len : 320;
      Serial.write(resp, show);
      Serial.println();
    }
    if (resp) free(resp);
    stop_tts_tls();
    return false;
  }
  Serial.printf("tts: google http %ums (%u bytes)\n", (unsigned)(millis() - t_req),
                (unsigned)resp_len);

  // Avoid ArduinoJson on the huge response — it OOMs / fails on ESP32.
  char* b64 = nullptr;
  size_t b64_len = 0;
  if (!extract_json_string_value(resp, resp_len, "audioContent", &b64, &b64_len) || b64_len == 0) {
    Serial.println("ERR: Google TTS missing audioContent");
    if (resp && resp_len > 0) {
      size_t show = resp_len < 200 ? resp_len : 200;
      Serial.write(resp, show);
      Serial.println();
    }
    if (resp) free(resp);
    stop_tts_tls();
    return false;
  }
  free(resp);
  tts_tls_last_ok_ms = millis();

  uint8_t* raw = nullptr;
  size_t raw_len = 0;
  if (!base64_decode(b64, b64_len, &raw, &raw_len) || raw_len == 0) {
    Serial.println("ERR: Google TTS base64 decode failed");
    free(b64);
    if (raw) free(raw);
    return false;
  }
  free(b64);

  // LINEAR16: usually a WAV wrapper. Raw PCM gets wrapped for play_wav().
  if (raw_len >= 44 && memcmp(raw, "RIFF", 4) == 0) {
    *audio_out = raw;
    *audio_len = raw_len;
    if (is_mp3) *is_mp3 = false;
    Serial.printf("tts: google ready in %ums (%u wav bytes, LINEAR16)\n",
                  (unsigned)(millis() - t_req), (unsigned)raw_len);
    return true;
  }

  // Raw PCM16 — wrap as WAV at TTS_SAMPLE_RATE.
  size_t wav_len = 44 + raw_len;
  uint8_t* wav = (uint8_t*)heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!wav) wav = (uint8_t*)malloc(wav_len);
  if (!wav) {
    free(raw);
    return false;
  }
  write_wav_header(wav, raw_len, TTS_SAMPLE_RATE, 1);
  memcpy(wav + 44, raw, raw_len);
  free(raw);
  *audio_out = wav;
  *audio_len = wav_len;
  if (is_mp3) *is_mp3 = false;
  Serial.printf("tts: google ready in %ums (%u wav bytes, raw LINEAR16@%uHz)\n",
                (unsigned)(millis() - t_req), (unsigned)wav_len, (unsigned)TTS_SAMPLE_RATE);
  return true;
}

bool streamelements_tts(const String& text, uint8_t** mp3_out, size_t* mp3_len) {
  String path = String("/kappa/v2/speech?voice=") + TTS_FALLBACK_VOICE + "&text=" + url_encode(text);
  return https_get("api.streamelements.com", path.c_str(), mp3_out, mp3_len);
}

bool play_mp3(const uint8_t* mp3, size_t mp3_len) {
  i2s.end();
  if (!begin_i2s_tx()) return false;
  set_speaker(true);

  MemoryStream source(const_cast<uint8_t*>(mp3), mp3_len);
  MP3DecoderMini decoder;
  EncodedAudioStream out(&i2s, &decoder);
  StreamCopy copier(out, source);

  source.begin();
  out.begin();
  while (source.available() > 0) {
    copier.copy();
  }
  decoder.end();
  i2s_tx_soft_stop(true);
  return true;
}

// --- WAV playback ---

bool parse_wav_pcm(const uint8_t* wav, size_t len, const uint8_t** pcm, size_t* pcm_len, uint32_t* rate,
                   uint16_t* channels) {
  if (len < 44 || memcmp(wav, "RIFF", 4) != 0) return false;
  *rate = wav[24] | (wav[25] << 8) | (wav[26] << 16) | (wav[27] << 24);
  *channels = wav[22] | (wav[23] << 8);
  size_t offset = 12;
  while (offset + 8 <= len) {
    const char* id = (const char*)(wav + offset);
    uint32_t chunk_size = wav[offset + 4] | (wav[offset + 5] << 8) | (wav[offset + 6] << 16) |
                          (wav[offset + 7] << 24);
    offset += 8;
    if (memcmp(id, "data", 4) == 0) {
      *pcm = wav + offset;
      *pcm_len = min((size_t)chunk_size, len - offset);
      return true;
    }
    offset += chunk_size;
  }
  return false;
}

void play_wav(const uint8_t* wav, size_t len) {
  const uint8_t* pcm = nullptr;
  size_t pcm_len = 0;
  uint32_t src_rate = SAMPLE_RATE;
  uint16_t channels = 1;
  if (!parse_wav_pcm(wav, len, &pcm, &pcm_len, &src_rate, &channels)) {
    Serial.println("ERR: WAV parse failed");
    return;
  }

  const int16_t* samples = (const int16_t*)pcm;
  size_t sample_count = pcm_len / sizeof(int16_t);
  if (channels > 1) sample_count /= channels;

  size_t out_count = sample_count;
  if (src_rate != SAMPLE_RATE) {
    out_count = (size_t)((uint64_t)sample_count * SAMPLE_RATE / src_rate);
  }

  int32_t* out = (int32_t*)heap_caps_malloc(out_count * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!out) out = (int32_t*)malloc(out_count * 2 * sizeof(int32_t));
  if (!out) return;

  for (size_t i = 0; i < out_count; i++) {
    size_t src_i = src_rate == SAMPLE_RATE ? i : (size_t)((uint64_t)i * src_rate / SAMPLE_RATE);
    if (src_i >= sample_count) src_i = sample_count - 1;
    int16_t s = samples[src_i * channels];
    int32_t v = pcm16_to_i2s32(s);
    out[i * 2] = v;
    out[i * 2 + 1] = v;
  }

  if (!ensure_speak_i2s()) {
    free(out);
    return;
  }

  if (!speak_speaker_on) {
    set_speaker(true);
    speak_speaker_on = true;
  }
  MemoryStream playbackSource((uint8_t*)out, out_count * 2 * sizeof(int32_t));
  StreamCopy playbackCopier(i2s, playbackSource);
  playbackSource.begin(speaker_info);

  while (playbackSource) {
    playbackCopier.copy();
  }

  if (!speak_session_open) {
    speak_session_end();
  }
  free(out);
  Serial.println("speak: done");
}

String prepare_tts_text(const String& text) {
  return sanitize_tts_text(text);
}

// google_only: when true, skip Groq/StreamElements fallbacks (needed while
// secure_client is mid-SSE for LLM streaming).
bool speak_text_ex(const String& text, bool google_only) {
  String tts_text = prepare_tts_text(text);
  if (tts_text.length() == 0) {
    Serial.println("tts: skip empty/emoji-only");
    return true;
  }
  Serial.printf("tts text: %s\n", tts_text.c_str());

  uint8_t* audio = nullptr;
  size_t audio_len = 0;
  bool ok = false;
  set_state(STATE_SPEAK, "tts");

  bool google_mp3 = false;
  if (google_cloud_tts_stream_play(tts_text)) {
    Serial.printf("tts provider: Google Cloud stream (%s)\n", effective_tts_voice());
    ok = true;
  } else if (tts_lang_override || tts_voice_override) {
    // EN/ZH voice missing or rejected — fall back to Korean Chirp.
    Serial.println("tts: override failed, fallback ko-KR");
    tts_lang_override = nullptr;
    tts_voice_override = nullptr;
    if (google_cloud_tts_stream_play(tts_text)) {
      Serial.printf("tts provider: Google Cloud stream (%s)\n", effective_tts_voice());
      ok = true;
    } else if (google_cloud_tts(tts_text, &audio, &audio_len, &google_mp3)) {
      Serial.printf("tts provider: Google Cloud (%s)\n", effective_tts_voice());
      if (google_mp3) {
        play_mp3(audio, audio_len);
      } else {
        play_wav(audio, audio_len);
      }
      free(audio);
      ok = true;
    }
  } else if (google_cloud_tts(tts_text, &audio, &audio_len, &google_mp3)) {
    Serial.printf("tts provider: Google Cloud (%s)\n", effective_tts_voice());
    if (google_mp3) {
      play_mp3(audio, audio_len);
    } else {
      play_wav(audio, audio_len);
    }
    free(audio);
    ok = true;
  } else if (!google_only) {
    // English-only Orpheus / StreamElements fallback
    String en = english_for_tts(tts_text);
    if (en.length() > TTS_MAX_CHARS) en = en.substring(0, TTS_MAX_CHARS);
    Serial.printf("tts fallback text: %s\n", en.c_str());

    int stream_res = groq_tts_stream_play(en);
    if (stream_res != 0) {
      Serial.println("tts provider: Groq Orpheus (streamed)");
      ok = true;
    } else if (groq_tts(en, &audio, &audio_len)) {
      Serial.println("tts provider: Groq Orpheus (buffered)");
      play_wav(audio, audio_len);
      free(audio);
      ok = true;
    } else {
      uint8_t* mp3 = nullptr;
      size_t mp3_len = 0;
      if (streamelements_tts(en, &mp3, &mp3_len)) {
        Serial.println("tts provider: StreamElements fallback");
        play_mp3(mp3, mp3_len);
        free(mp3);
        ok = true;
      }
    }
  }

  if (!ok) {
    Serial.println(google_only ? "ERR: Google TTS failed (streaming LLM holds Groq socket)"
                               : "ERR: TTS failed (GOOGLE_API / Groq / StreamElements)");
  }
  return ok;
}

bool speak_text(const String& text) { return speak_text_ex(text, false); }

bool http_get_raw(const char* host, uint16_t port, const char* path, uint8_t** body, size_t* body_len) {
  if (!connect_wifi(true)) return false;
  WiFiClient client;
  if (!client.connect(host, port)) {
    Serial.printf("ERR: HTTP connect %s:%u failed\n", host, (unsigned)port);
    return false;
  }
  client.printf("GET %s HTTP/1.1\r\n", path);
  client.printf("Host: %s\r\n", host);
  client.print("Connection: close\r\n\r\n");

  int status = 0;
  bool ok = read_http_response(&client, &status, body, body_len, nullptr);
  client.stop();
  if (!ok) {
    Serial.println("ERR: HTTP read failed");
    return false;
  }
  if (status != 200) {
    Serial.printf("ERR: HTTP %s:%u %s -> %d\n", host, (unsigned)port, path, status);
    if (*body) {
      free(*body);
      *body = nullptr;
      *body_len = 0;
    }
    return false;
  }
  return true;
}

bool run_wav_pipeline(const uint8_t* wav, size_t wav_len) {
  if (!wav || wav_len < 44) return false;
  state = STATE_PROCESS;
  xSemaphoreTake(net_mutex, portMAX_DELAY);

  String transcript;
  uint32_t t_stt = millis();
  bool ok = groq_post_multipart_stt(wav, wav_len, transcript);
  uint32_t stt_ms = millis() - t_stt;
  if (!ok) {
    Serial.println("stt: skipped (request failed or empty)");
    xSemaphoreGive(net_mutex);
    state = STATE_LISTEN;
    return true;
  }
  if (!is_valid_transcript(transcript)) {
    Serial.printf("stt: skipped (rejected: %s)\n", transcript.c_str());
    xSemaphoreGive(net_mutex);
    state = STATE_LISTEN;
    return true;
  }

  String reply;
  uint32_t t_llm = millis();
  ok = groq_chat(transcript, reply);  // speaks sentences as they stream
  if (ok) {
    Serial.printf("timing: stt=%ums llm+tts=%ums\n", (unsigned)stt_ms,
                  (unsigned)(millis() - t_llm));
    end_turn_cleanup();
  } else {
    Serial.println("pipeline failed — llm");
    stop_groq_tls();
    stop_tts_tls();
    state = STATE_LISTEN;
  }
  xSemaphoreGive(net_mutex);
  state = STATE_LISTEN;
  return ok;
}

bool run_text_pipeline(const String& transcript) {
  String t = transcript;
  t.trim();
  if (!is_valid_transcript(t)) {
    Serial.printf("stt: skipped (rejected: %s)\n", t.c_str());
    return true;
  }

  Serial.printf("stt #(text): %s\n", t.c_str());
  state = STATE_PROCESS;
  xSemaphoreTake(net_mutex, portMAX_DELAY);

  String reply;
  uint32_t t_llm = millis();
  bool ok = groq_chat(t, reply);  // speaks sentences as they stream
  if (ok) {
    Serial.printf("timing: stt=0ms(text) llm+tts=%ums\n", (unsigned)(millis() - t_llm));
    end_turn_cleanup();
  } else {
    Serial.println("pipeline failed — llm");
    stop_groq_tls();
    stop_tts_tls();
    state = STATE_LISTEN;
  }

  listen_paused_until_ms = millis() + POST_TEXT_PAUSE_MS;
  xSemaphoreGive(net_mutex);
  state = STATE_LISTEN;
  return ok;
}

void poll_serial_commands() {
  static String line;
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      if (line.startsWith("KOTEST ")) {
        String payload = line.substring(7);
        payload.trim();
        Serial.printf("cmd: KOTEST (%u chars)\n", (unsigned)payload.length());
        run_text_pipeline(payload);
      } else if (line.equalsIgnoreCase("PAIR") || line.equalsIgnoreCase("SYNC")) {
        Serial.printf("cmd: %s\n", line.c_str());
        device_config_sync();
        device_config_print_pair_info();
      } else if (line.startsWith("STTFETCH ")) {
        // STTFETCH 192.168.0.10:8000/talkbot_ko.wav
        String spec = line.substring(9);
        spec.trim();
        int slash = spec.indexOf('/');
        int colon = spec.indexOf(':');
        if (slash > 0 && colon > 0 && colon < slash) {
          String host = spec.substring(0, colon);
          uint16_t port = (uint16_t)spec.substring(colon + 1, slash).toInt();
          String path = spec.substring(slash);
          Serial.printf("cmd: STTFETCH %s:%u%s\n", host.c_str(), (unsigned)port, path.c_str());
          uint8_t* wav = nullptr;
          size_t wav_len = 0;
          if (http_get_raw(host.c_str(), port, path.c_str(), &wav, &wav_len)) {
            Serial.printf("stt fetch: %u bytes\n", (unsigned)wav_len);
            run_wav_pipeline(wav, wav_len);
            free(wav);
          }
        } else {
          Serial.println("ERR: STTFETCH host:port/path");
        }
      }
      line = "";
    } else if (line.length() < 240) {
      line += c;
    } else {
      line = "";
    }
  }
}

void end_turn_cleanup() {
  // Keep TLS sockets alive across turns — reconnect cost is 0.4~0.8s.
  // Idle sockets are refreshed by ensure_*_tls / onset refresh when stale.
  speak_session_end();
  just_played = true;
  listen_paused_until_ms = millis() + POST_PLAY_PAUSE_MS;
  noise_floor_rms = 0.012f;
  last_onset_peak_rms = 0.0f;
  request_tts_preconnect();  // warm TTS for next turn (listen owns the mic now)
  set_state(STATE_LISTEN, "turn end");
}

bool ensure_speak_i2s() {
  if (speak_i2s_open) return true;
  if (!begin_i2s_tx()) return false;
  speak_i2s_open = true;
  return true;
}

void speak_session_begin() {
  speak_session_open = true;
}

void speak_session_end() {
  speak_session_open = false;
  if (speak_i2s_open) {
    i2s_tx_write_silence_ms(150);
  }
  if (speak_speaker_on) {
    set_speaker(false);
    speak_speaker_on = false;
    delay(12);
  }
  if (speak_i2s_open) {
    i2s.end();
    speak_i2s_open = false;
  }
}

bool cache_opener_clip(const char* text) {
  // Fresh socket — boot stream TTS just closed / invalidated keep-alive.
  stop_tts_tls();
  uint8_t* audio = nullptr;
  size_t audio_len = 0;
  bool is_mp3 = false;
  if (!google_cloud_tts(text, &audio, &audio_len, &is_mp3) || is_mp3 || !audio) {
    if (audio) free(audio);
    return false;
  }
  const uint8_t* pcm = nullptr;
  size_t pcm_len = 0;
  uint32_t src_rate = SAMPLE_RATE;
  uint16_t channels = 1;
  if (!parse_wav_pcm(audio, audio_len, &pcm, &pcm_len, &src_rate, &channels)) {
    free(audio);
    return false;
  }
  size_t n = pcm_len / sizeof(int16_t);
  if (channels > 1) n /= channels;
  size_t out_n = n;
  if (src_rate != SAMPLE_RATE) {
    out_n = (size_t)((uint64_t)n * SAMPLE_RATE / src_rate);
  }
  int16_t* buf = (int16_t*)heap_caps_malloc(out_n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (int16_t*)malloc(out_n * sizeof(int16_t));
  if (!buf) {
    free(audio);
    return false;
  }
  const int16_t* samples = (const int16_t*)pcm;
  for (size_t i = 0; i < out_n; i++) {
    size_t src_i = src_rate == SAMPLE_RATE ? i : (size_t)((uint64_t)i * src_rate / SAMPLE_RATE);
    if (src_i >= n) src_i = n - 1;
    buf[i] = samples[src_i * channels];
  }
  free(audio);
  if (opener_pcm) free(opener_pcm);
  opener_pcm = buf;
  opener_samples = out_n;
  Serial.printf("opener: cached \"%s\" (%u samples, %.0fms)\n", text, (unsigned)out_n,
                out_n * 1000.0f / SAMPLE_RATE);
  return true;
}

bool play_opener_clip() {
  if (!opener_pcm || opener_samples == 0) {
    play_filler_chirp();
    return false;
  }
  // Standalone short play — does not join the multi-sentence speak session.
  if (!begin_i2s_tx()) return false;
  set_speaker(true);
  const size_t FRAMES = 256;
  int32_t out[FRAMES * 2];
  size_t i = 0;
  while (i < opener_samples) {
    size_t n = opener_samples - i;
    if (n > FRAMES) n = FRAMES;
    for (size_t k = 0; k < n; k++) {
      int32_t v = pcm16_to_i2s32(opener_pcm[i + k]);
      out[k * 2] = v;
      out[k * 2 + 1] = v;
    }
    uint8_t* p = (uint8_t*)out;
    size_t total = n * 2 * sizeof(int32_t);
    size_t off = 0;
    while (off < total) {
      size_t w = i2s.write(p + off, total - off);
      if (w == 0) delay(1);
      off += w;
    }
    i += n;
  }
  i2s_tx_soft_stop(true);
  Serial.println("opener: played");
  return true;
}

// Short procedural "thinking" chirp fallback if opener cache missing (~200 ms).
void play_filler_chirp() {
  const size_t n = SAMPLE_RATE * 20 / 100;  // 200 ms
  int32_t* out = (int32_t*)malloc(n * 2 * sizeof(int32_t));
  if (!out) return;
  if (!begin_i2s_tx()) {
    free(out);
    return;
  }
  set_speaker(true);
  for (size_t i = 0; i < n; i++) {
    float t = (float)i / (float)SAMPLE_RATE;
    float env = 1.0f;
    if (i < SAMPLE_RATE / 50) env = (float)i / (SAMPLE_RATE / 50.0f);
    else if (i > n - SAMPLE_RATE / 25) env = (float)(n - i) / (SAMPLE_RATE / 25.0f);
    float freq = 420.0f + 260.0f * t / 0.20f;
    float s = sinf(2.0f * 3.14159265f * freq * t) * env * 0.22f;
    int16_t pcm = (int16_t)(s * 32767.0f);
    int32_t v = pcm16_to_i2s32(pcm);
    out[i * 2] = v;
    out[i * 2 + 1] = v;
  }
  uint8_t* p = (uint8_t*)out;
  size_t total = n * 2 * sizeof(int32_t);
  size_t off = 0;
  while (off < total) {
    size_t w = i2s.write(p + off, total - off);
    if (w == 0) delay(1);
    off += w;
  }
  i2s_tx_soft_stop(true);
  free(out);
  Serial.println("filler: chirp played");
}

bool run_pipeline_inner() {
  set_state(STATE_PROCESS, "pipeline start");
  uint32_t t_pipe = millis();
  Serial.printf("pipe: recorded %.2fs (%u samples)\n",
                record_count / (float)SAMPLE_RATE, (unsigned)record_count);

  uint8_t* wav = nullptr;
  size_t wav_len = 0;
  uint32_t t_wav = millis();
  if (!build_ulaw_wav(&wav, &wav_len)) {
    size_t pcm_bytes = record_count * sizeof(int16_t);
    wav_len = 44 + pcm_bytes;
    wav = (uint8_t*)heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) wav = (uint8_t*)malloc(wav_len);
    if (!wav) {
      Serial.println("ERR: wav alloc failed");
      return false;
    }
    write_wav_header(wav, pcm_bytes, SAMPLE_RATE, 1);
    memcpy(wav + 44, record_buf, pcm_bytes);
    Serial.println("stt: upload PCM16 (ulaw build failed)");
  } else {
    Serial.printf("stt: upload μ-law wav %u bytes (pcm would be %u) build=%ums\n",
                  (unsigned)wav_len, (unsigned)(44 + record_count * 2),
                  (unsigned)(millis() - t_wav));
  }

  String transcript;
  uint32_t t_stt = millis();
  Serial.println("pipe: STT …");
  if (!groq_post_multipart_stt(wav, wav_len, transcript)) {
    free(wav);
    Serial.println("pipe: STT failed/empty — back to listen");
    return true;
  }
  free(wav);
  uint32_t stt_ms = millis() - t_stt;

  if (!is_valid_transcript(transcript)) {
    Serial.printf("pipe: STT rejected \"%s\" — back to listen\n", transcript.c_str());
    return true;
  }
  Serial.printf("pipe: STT ok in %ums -> \"%s\"\n", (unsigned)stt_ms, transcript.c_str());

  // Opener (if any) plays inside groq_chat only when TTFT is slow.
  String reply;
  uint32_t t_llm = millis();
  Serial.println("pipe: LLM+TTS …");
  if (!groq_chat(transcript, reply)) {
    Serial.println("pipe: LLM failed");
    speak_session_end();
    return false;
  }
  uint32_t llm_tts_ms = millis() - t_llm;
  Serial.printf("pipe: turn done stt=%ums llm+tts=%ums total=%ums reply=\"%s\"\n",
                (unsigned)stt_ms, (unsigned)llm_tts_ms,
                (unsigned)(millis() - t_pipe), reply.c_str());

  end_turn_cleanup();
  return true;
}

bool run_pipeline() {
  // Serialize against the core-0 preconnect task; blocks until any
  // in-flight handshake finishes, then owns the connection for the turn.
  xSemaphoreTake(net_mutex, portMAX_DELAY);
  bool ok = run_pipeline_inner();
  xSemaphoreGive(net_mutex);
  return ok;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\ntalkbot groq voice chat");
  Serial.printf("heap free: internal=%u psram=%u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  init_codec();

  size_t record_bytes = MAX_SAMPLES * sizeof(int16_t);
  record_buf = (int16_t*)heap_caps_malloc(record_bytes, MALLOC_CAP_SPIRAM);
  if (!record_buf) {
    Serial.println("ERR: record buffer PSRAM alloc failed");
    while (true) delay(1000);
  }
  Serial.printf("record buffer: %u bytes in PSRAM\n", (unsigned)record_bytes);

  secure_client = new WiFiClientSecure();
  secure_client->setInsecure();
  secure_client->setTimeout(60000);

  tts_client = new WiFiClientSecure();
  tts_client->setInsecure();
  tts_client->setTimeout(60000);

  net_mutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(net_task, "net", 16384, nullptr, 1, nullptr, 0);

  init_wifi();
  // wifi_portal_begin() already tries saved/auto-connect; ensure once more if needed
  if (!wifi_portal_connected()) connect_wifi(true);

  device_config_begin(GOOGLE_TTS_VOICE, "안녕, 난 디노야! 같이 놀자.", GROQ_API_KEY, GOOGLE_API_KEY);

  if (!wifi_portal_connected()) {
    Serial.println("WARN: WiFi not connected — join AP Talkbot-Setup -> http://192.168.4.1");
    device_config_print_pair_info();
  } else {
    device_config_sync();
    device_config_print_pair_info();
    Serial.printf("tts primary: Google Cloud (%s / %s, LINEAR16 %uHz)\n", GOOGLE_TTS_LANG,
                  effective_tts_voice(), (unsigned)TTS_SAMPLE_RATE);
    Serial.println("tts fallback: Groq Orpheus (en) or StreamElements");
    Serial.printf("stt language: %s\n", STT_LANGUAGE);
    Serial.printf("llm model: %s host=%s\n", LLM_MODEL, GEMINI_HOST);
    if (TTS_BOOT_TEST) {
      const char* boot = device_config_get().boot_phrase;
      if (!boot || !boot[0]) boot = "안녕, 난 디노야! 같이 놀자.";
      if (!speak_text(boot)) {
        Serial.println("WARN: boot TTS test failed");
      }
      // Cache a same-voice local opener for instant feedback during LLM TTFT.
      if (!cache_opener_clip("음!")) {
        Serial.println("WARN: opener cache failed — will use chirp fallback");
      }
      end_turn_cleanup();
    }
  }

  Serial.println("ready: VAD listening loop");
  Serial.println("dbg: keys — speak near mic; PAIR/SYNC to refresh cloud config");
}

void loop() {
  maintain_wifi();
  device_config_maintain(120000);
  poll_serial_commands();

  if (state != STATE_LISTEN) {
    delay(10);
    return;
  }

  if (stt_cooldown_active()) {
    static uint32_t last_cooldown_log_ms = 0;
    uint32_t now = millis();
    if (now - last_cooldown_log_ms > 5000) {
      Serial.printf("state: LISTEN blocked — groq cooldown %us (calls=%u)\n",
                    stt_cooldown_remaining_ms() / 1000, stt_call_count);
      last_cooldown_log_ms = now;
    }
    delay(500);
    return;
  }

  if (millis() < listen_paused_until_ms) {
    delay(10);
    return;
  }

  static uint32_t last_idle_log_ms = 0;
  uint32_t now = millis();
  if (now - last_idle_log_ms > 8000) {
    Serial.printf("state: LISTEN idle (wifi=%s rssi=%d)\n",
                  WiFi.status() == WL_CONNECTED ? "ok" : "down", WiFi.RSSI());
    last_idle_log_ms = now;
  }

  if (!record_utterance()) {
    delay(100);
    return;
  }

  set_state(STATE_PROCESS, "speech captured");
  if (!run_pipeline()) {
    Serial.println("pipeline failed — retrying");
    stop_groq_tls();
    stop_tts_tls();
  }
  set_state(STATE_LISTEN, "loop");
  delay(50);
}
