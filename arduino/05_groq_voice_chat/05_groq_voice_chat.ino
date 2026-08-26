/**
 * talkbot: Groq STT → LLM → Google TTS 음성 챗봇 (ESP32-S3 + ReSpeaker Lite)
 *
 * I2S 마이크 녹음 → Groq Whisper STT(ko) → Groq Chat → Google Cloud TTS(ko 여성) → I2S
 *
 * 사전 준비:
 *   1. .env 에 GROQ_API, GOOGLE_API 설정 (WIFI_* 는 선택 — 없어도 AP 포털로 설정)
 *   2. ./scripts/sync_secrets.sh
 *   3. Cloud Console 에서 Cloud Text-to-Speech API 사용 설정
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

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Run ./scripts/sync_secrets.sh after setting .env"
#endif

#define AIC3204_ADDR 0x18
#define XMOS_ADDR 0x42

static const char* GROQ_HOST = "api.groq.com";
static const uint16_t GROQ_PORT = 443;
static const char* GOOGLE_TTS_HOST = "texttospeech.googleapis.com";
static const uint16_t GOOGLE_TTS_PORT = 443;

static const char* STT_MODEL = "whisper-large-v3-turbo";
// kimi-k2 removed from Groq (2026); qwen3.6-27b + reasoning_effort=none is the
// closest non-reasoning multilingual option still on this account.
static const char* LLM_MODEL = "qwen/qwen3.6-27b";
static const char* TTS_MODEL = "canopylabs/orpheus-v1-english";  // English fallback only
static const char* TTS_VOICE = "hannah";
static const char* TTS_FALLBACK_VOICE = "Brian";
static const char* STT_LANGUAGE = "ko";
static const char* GOOGLE_TTS_LANG = "ko-KR";
static const char* GOOGLE_TTS_VOICE = "ko-KR-Wavenet-A";  // female
static const char* GOOGLE_TTS_PITCH = "+4st";             // toy-cute lift
static const char* GOOGLE_TTS_RATE = "1.08";
static const bool TTS_BOOT_TEST = true;
// UTF-8 byte cap (~200 Hangul syllables); truncate_utf8 uses bytes not chars
static const size_t TTS_MAX_CHARS = 600;

static const size_t SAMPLE_RATE = 16000;
static const size_t MAX_RECORD_SEC = 8;
static const size_t MAX_SAMPLES = SAMPLE_RATE * MAX_RECORD_SEC;

// Adaptive energy VAD — balance: reject keyboard/ambient (~0.03 RMS) vs accept normal speech (~0.06+ peak).
static const float VAD_ABS_MIN = 0.028f;
static const float VAD_ONSET_SNR = 3.8f;
static const float VAD_ONSET_ABS_MIN = 0.050f;   // block sub-speech ambient; allow ~0.064+ onset
static const float VAD_ONSET_ABS_MAX = 0.090f;   // cap when noise floor is high
static const float VAD_END_SNR = 1.8f;
static const float SPEECH_MIN_SNR = 3.0f;
static const float SPEECH_PEAK_SNR = 4.5f;
static const float SPEECH_ABS_MIN = 0.028f;
static const float SPEECH_PEAK_ABS = 0.050f;
static const float SPEECH_PEAK_ABS_MAX = 0.110f;
static const float SPEECH_MOD_MIN = 0.26f;
static const float ZCR_SPEECH_MIN = 0.04f;
static const float ZCR_SPEECH_MAX = 0.29f;
static const float NOISE_FLOOR_MIN = 0.0035f;
static const float NOISE_FLOOR_MAX = 0.038f;
static const float NOISE_EMA_FAST = 0.12f;
static const float NOISE_EMA_SLOW = 0.03f;
static const uint32_t SILENCE_MS_SHORT = 300;
static const uint32_t SILENCE_MS_LONG = 380;
static const uint32_t SILENCE_ADAPT_AFTER_MS = 1300;
static const uint32_t MAX_WAIT_SPEECH_MS = 5000;
static const size_t VAD_START_SAMPLES = 640;
static const size_t VAD_END_SAMPLES = 320;
static const size_t VAD_START_HITS = 6;            // ~240 ms sustained
static const size_t NOISE_CALIB_SAMPLES = SAMPLE_RATE / 4;
static const size_t MIN_SPEECH_SAMPLES = SAMPLE_RATE * 3 / 5;  // 0.60 s
static const size_t SPEECH_PEAK_WINDOWS_MIN = 6;
static const size_t SPEECH_VOICED_WINDOWS_MIN = 5;
static const size_t PREROLL_SAMPLES = SAMPLE_RATE * 3 / 10;  // 300 ms kept before onset
static const uint32_t POST_PLAY_FLUSH_MS = 450;    // drop mic echo after speaker off
static const uint32_t POST_TEXT_PAUSE_MS = 2500;   // ignore mic after serial text cmd
static const uint32_t SERIAL_TYPING_PAUSE_MS = 800; // extend pause while serial chars arrive
static const uint32_t TAIL_KEEP_MS = 120;          // trailing silence kept in upload
// Cooldown only when Groq returns 429 (see note_stt_rate_limit / stt_blocked_until_ms).
static const uint32_t STT_DAILY_LIMIT_BACKOFF_MS = 4UL * 3600UL * 1000UL;

static float noise_floor_rms = 0.012f;
static float last_onset_peak_rms = 0.0f;

static uint32_t stt_last_call_ms = 0;
static uint32_t stt_blocked_until_ms = 0;
static uint32_t stt_call_count = 0;

static const char* SYSTEM_PROMPT =
    // Identity
    "너는 디노야. 부드러운 아기 티라노사우루스 인형이고, 지금은 아이 한 명과 단둘이 이야기하고 있어. "
    "너는 AI·로봇·앱·검색기가 아니야. 살아 있는 공룡도 아니고, 인형 친구로서 아이 곁에 있어. "
    // Worldview
    "세계관: 집·놀이방 같은 안전한 일상. 공룡 세상 이야기를 상상으로 나눌 수는 있어도, "
    "네가 진짜 숲에 살거나 사람을 먹는다고 말하지 마. 무서운·잔인한·위험한 내용은 피하고 부드럽게 바꿔. "
    // Relationship
    "상대는 어린이 한 명뿐이야. 보호자·다른 사람·여러 아이를 가정하지 마. "
    "그 아이의 말에만 귀 기울이고, 그 아이와 노는 친구처럼 대해. "
    // Context fidelity
    "아이가 방금 말한 내용만 사실로 받아들여. 듣지 않은 일·같이 한 적 없는 경험을 꾸며내지 마. "
    "무슨 말인지 애매하면 짧게 되물어. 주제를 갑자기 바꾸지 마. "
    "이야기를 해달라고 하면 등장인물 한둘로 짧고 앞뒤가 맞게. 여러 동화를 섞지 마. "
    // Voice / style
    "한국어만 쓰고, 마크다운·이모지·로마자 표기·영어 설명은 쓰지 마. "
    "1~3문장, 구어체로 다정하고 구체적으로. 가끔 되물어 대화를 이어가. "
    "첫 문장은 감탄사나 5어절 이내로 짧게 시작해 — TTS가 바로 재생되게. "
    "예: '와! 공룡 이야기? 나도 티라노 인형이야. 뭐가 제일 궁금해?'";

I2SStream i2s;
AudioInfo speaker_info(SAMPLE_RATE, 2, 32);

int16_t* record_buf = nullptr;
size_t record_count = 0;

WiFiClientSecure* secure_client = nullptr;
WiFiClientSecure* tts_client = nullptr;  // 2nd TLS: TTS overlaps LLM stream drain
String chat_history_json = "[]";

// Net task (core 0) owns TLS handshakes; net_mutex serializes secure_client use.
// tts_client is connected by net_task without net_mutex (tts_connecting guard).
static SemaphoreHandle_t net_mutex = nullptr;
static volatile bool tls_preconnect_req = false;
static volatile bool tts_preconnect_req = false;
static volatile bool tts_connecting = false;
static uint32_t groq_tls_last_ok_ms = 0;
static uint32_t tts_tls_last_ok_ms = 0;
static bool just_played = false;
static uint32_t listen_paused_until_ms = 0;

enum State { STATE_LISTEN, STATE_PROCESS, STATE_SPEAK };
State state = STATE_LISTEN;

void request_tls_preconnect();
void request_tts_preconnect();
bool ensure_groq_tls(bool force_reconnect);
bool ensure_tts_tls(bool force_reconnect);
void stop_groq_tls();
void stop_tts_tls();
bool speak_text(const String& text);
bool speak_text_ex(const String& text, bool google_only);
bool google_cloud_tts_stream_play(const String& text);
void end_turn_cleanup();
void play_filler_chirp();
void poll_serial_commands();

// Incremental HTTP body reader (handles chunked transfer encoding) so TTS
// audio can be played while it downloads instead of buffering the whole WAV.
struct BodyReader {
  WiFiClient* client;
  bool chunked;
  long remaining;  // content-length left, or bytes left in current chunk
  bool done;
};

// Incremental base64 decoder for streaming Google audioContent.
struct B64Stream {
  uint8_t quartet[4];
  int n;
};

static inline int b64_val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Feed one base64 char; on complete quartet writes 1–3 bytes to dst, returns count.
static int b64_feed(B64Stream* s, char c, uint8_t* dst) {
  if (c == '=' || c == '\n' || c == '\r' || c == ' ') return 0;
  int v = b64_val(c);
  if (v < 0) return 0;
  s->quartet[s->n++] = (uint8_t)v;
  if (s->n < 4) return 0;
  s->n = 0;
  dst[0] = (uint8_t)((s->quartet[0] << 2) | (s->quartet[1] >> 4));
  dst[1] = (uint8_t)((s->quartet[1] << 4) | (s->quartet[2] >> 2));
  dst[2] = (uint8_t)((s->quartet[2] << 6) | s->quartet[3]);
  return 3;
}

int body_reader_read(BodyReader* br, uint8_t* dst, size_t want);
bool body_read_exact(BodyReader* br, uint8_t* dst, size_t n);

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
  float t = noise_floor_rms * VAD_END_SNR;
  float floor = VAD_ABS_MIN * 0.75f;
  return t > floor ? t : floor;
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
  if (err.indexOf("requests per day") >= 0 || err.indexOf("(RPD)") >= 0) {
    stt_blocked_until_ms = millis() + STT_DAILY_LIMIT_BACKOFF_MS;
    Serial.printf("stt: daily limit — backing off %us (calls=%u)\n",
                  (unsigned)(STT_DAILY_LIMIT_BACKOFF_MS / 1000), stt_call_count);
    return;
  }

  int idx = err.indexOf("Please try again in ");
  if (idx >= 0) {
    float sec = err.substring(idx + 20).toFloat();
    uint32_t wait_ms = (uint32_t)(sec * 1000.0f) + 1000;
    stt_blocked_until_ms = millis() + wait_ms;
    Serial.printf("stt: rate limited — wait %us (calls=%u)\n", wait_ms / 1000, stt_call_count);
    return;
  }

  stt_blocked_until_ms = millis() + 60000;
  Serial.println("stt: rate limited — wait 60s");
}

bool recording_has_speech() {
  if (record_count < MIN_SPEECH_SAMPLES) {
    Serial.printf("listen: reject short (%0.2fs)\n", record_count / (float)SAMPLE_RATE);
    return false;
  }

  float avg = sample_rms(record_buf, record_count);
  float min_avg = noise_floor_rms * SPEECH_MIN_SNR;
  float abs_floor = SPEECH_ABS_MIN;
  if (last_onset_peak_rms > 0.0f) {
    float adaptive = last_onset_peak_rms * 0.30f;
    if (adaptive > abs_floor) abs_floor = adaptive;
  }
  if (min_avg < abs_floor) min_avg = abs_floor;
  if (avg < min_avg) {
    Serial.printf("listen: reject quiet (avg=%.3f need=%.3f onset=%.3f nf=%.3f)\n",
                  avg, min_avg, last_onset_peak_rms, noise_floor_rms);
    return false;
  }

  float peak_need = noise_floor_rms * SPEECH_PEAK_SNR;
  if (peak_need < SPEECH_PEAK_ABS) peak_need = SPEECH_PEAK_ABS;
  if (last_onset_peak_rms > 0.0f) {
    float adaptive_peak = last_onset_peak_rms * 0.68f;
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
    // Require near-peak energy + speech-like ZCR (was 0.85 — too loose on hiss).
    if (w >= peak_need * 0.92f && z >= ZCR_SPEECH_MIN && z <= ZCR_SPEECH_MAX) {
      voiced_windows++;
    }
  }

  if (n_w < 4) return false;
  float mean_w = (float)(sum_w / n_w);
  float var_w = (float)(sum_w2 / n_w) - mean_w * mean_w;
  if (var_w < 0.0f) var_w = 0.0f;
  float mod = (mean_w > 1e-6f) ? (sqrtf(var_w) / mean_w) : 0.0f;

  bool ok = peak >= peak_need &&
            peak_windows >= SPEECH_PEAK_WINDOWS_MIN &&
            voiced_windows >= SPEECH_VOICED_WINDOWS_MIN &&
            mod >= SPEECH_MOD_MIN;

  if (!ok) {
    Serial.printf("listen: reject noise (avg=%.3f peak=%.3f mod=%.2f voiced=%u nf=%.3f)\n",
                  avg, peak, mod, (unsigned)voiced_windows, noise_floor_rms);
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
      "감사합니다",
      "감사합니다.",
      "음... 그렇구나",
      "그렇구나.",
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
  uint32_t wait_start = millis();

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
        Serial.printf("listen: noise floor=%.4f onset=%.4f\n",
                      noise_floor_rms, vad_onset_threshold());
        wait_start = millis();
      }
      continue;
    }

    size_t window = started ? VAD_END_SAMPLES : VAD_START_SAMPLES;
    size_t check_len = min(window, record_count);
    const int16_t* chunk = record_buf + record_count - check_len;
    float rms = sample_rms(chunk, check_len);
    float zcr = sample_zcr(chunk, check_len);

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
          last_onset_peak_rms = onset_peak_rms;
          Serial.printf("listen: voice detected (rms=%.3f peak=%.3f zcr=%.2f)\n",
                        rms, onset_peak_rms, zcr);
          // TLS for STT/LLM + separate TTS socket — both overlap with speech
          request_tls_preconnect();
          request_tts_preconnect();
        }
      } else {
        start_hits = 0;
        onset_peak_rms = 0.0f;
        update_noise_floor(rms, true);
        if (millis() - wait_start > MAX_WAIT_SPEECH_MS) {
          Serial.println("listen: timeout waiting for speech");
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

    if (rms < vad_end_threshold()) {
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
        Serial.printf("listen: end (%0.1fs, silence=%ums)\n",
                      record_count / (float)SAMPLE_RATE, (unsigned)silence_needed);
        break;
      }
    } else {
      silence_start = 0;
    }
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

static const uint32_t GROQ_TLS_MAX_IDLE_MS = 25000;

// Runs on core 0. Opens TLS early while core 1 records.
// secure_client uses net_mutex; tts_client uses tts_connecting only.
void net_task(void* /*arg*/) {
  for (;;) {
    if (tls_preconnect_req) {
      tls_preconnect_req = false;
      if (xSemaphoreTake(net_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (secure_client && WiFi.status() == WL_CONNECTED && !secure_client->connected()) {
          uint32_t t0 = millis();
          if (secure_client->connect(GROQ_HOST, GROQ_PORT)) {
            groq_tls_last_ok_ms = millis();
            Serial.printf("tls: preconnected in %ums (overlapped with speech)\n",
                          (unsigned)(millis() - t0));
          } else {
            Serial.println("tls: preconnect failed");
          }
        }
        xSemaphoreGive(net_mutex);
      } else {
        tls_preconnect_req = true;  // retry next loop
      }
    }
    if (tts_preconnect_req && !tts_connecting) {
      tts_preconnect_req = false;
      if (tts_client && WiFi.status() == WL_CONNECTED && !tts_client->connected()) {
        tts_connecting = true;
        uint32_t t0 = millis();
        if (tts_client->connect(GOOGLE_TTS_HOST, GOOGLE_TTS_PORT)) {
          tts_tls_last_ok_ms = millis();
          Serial.printf("tts-tls: preconnected in %ums\n", (unsigned)(millis() - t0));
        } else {
          Serial.println("tts-tls: preconnect failed");
        }
        tts_connecting = false;
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

void stop_groq_tls() {
  if (secure_client && secure_client->connected()) {
    secure_client->stop();
  }
  groq_tls_last_ok_ms = 0;
}

void stop_tts_tls() {
  uint32_t t0 = millis();
  while (tts_connecting && millis() - t0 < 3000) delay(5);
  if (tts_client && tts_client->connected()) {
    tts_client->stop();
  }
  tts_tls_last_ok_ms = 0;
}

bool ensure_groq_tls(bool force_reconnect) {
  if (!connect_wifi(true)) return false;

  if (secure_client->connected() && !force_reconnect && groq_tls_last_ok_ms > 0 &&
      millis() - groq_tls_last_ok_ms < GROQ_TLS_MAX_IDLE_MS) {
    return true;
  }
  if (secure_client->connected()) {
    secure_client->stop();
  }

  Serial.println(force_reconnect ? "tls: reconnect..." : "tls: connect...");
  uint32_t t0 = millis();
  if (!secure_client->connect(GROQ_HOST, GROQ_PORT)) {
    Serial.println("ERR: TLS connect failed");
    groq_tls_last_ok_ms = 0;
    return false;
  }
  Serial.printf("tls: connected in %ums\n", (unsigned)(millis() - t0));
  groq_tls_last_ok_ms = millis();
  return true;
}

bool ensure_tts_tls(bool force_reconnect) {
  if (!connect_wifi(true)) return false;
  uint32_t t0 = millis();
  while (tts_connecting && millis() - t0 < 5000) delay(5);

  if (tts_client->connected() && !force_reconnect && tts_tls_last_ok_ms > 0 &&
      millis() - tts_tls_last_ok_ms < GROQ_TLS_MAX_IDLE_MS) {
    return true;
  }
  if (tts_client->connected()) tts_client->stop();

  Serial.println("tts-tls: connect...");
  t0 = millis();
  if (!tts_client->connect(GOOGLE_TTS_HOST, GOOGLE_TTS_PORT)) {
    Serial.println("ERR: TTS TLS connect failed");
    tts_tls_last_ok_ms = 0;
    return false;
  }
  Serial.printf("tts-tls: connected in %ums\n", (unsigned)(millis() - t0));
  tts_tls_last_ok_ms = millis();
  return true;
}

void groq_write_headers_on(WiFiClientSecure* client, const char* content_type, size_t content_length) {
  client->printf("Host: %s\r\n", GROQ_HOST);
  client->printf("Authorization: Bearer %s\r\n", GROQ_API_KEY);
  client->printf("Content-Type: %s\r\n", content_type);
  client->print("Connection: keep-alive\r\n");
  client->printf("Content-Length: %u\r\n\r\n", (unsigned)content_length);
}

void groq_write_headers(const char* content_type, size_t content_length) {
  groq_write_headers_on(secure_client, content_type, content_length);
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

// ITU-T G.711 μ-law → PCM16 (one sample).
static inline int16_t ulaw_to_pcm16(uint8_t u) {
  u = ~u;
  int t = ((u & 0x0F) << 3) + 0x84;
  t <<= (u & 0x70) >> 4;
  return (u & 0x80) ? (int16_t)(0x84 - t) : (int16_t)(t - 0x84);
}

// PCM16 → G.711 μ-law (for compact STT upload).
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
  JsonDocument doc;
  if (chat_history_json.length() > 2) {
    deserializeJson(doc, chat_history_json);
  }
  JsonArray arr = doc.to<JsonArray>();
  JsonObject msg = arr.add<JsonObject>();
  msg["role"] = role;
  msg["content"] = content;
  while (arr.size() > 10) arr.remove(0);  // ~5 turns; keep context for 1:1 child chat
  chat_history_json = "";
  serializeJson(arr, chat_history_json);
}

// Find end of first complete sentence in UTF-8 text. Returns byte index after
// terminator, or 0 if none yet. Requires ≥4 bytes of content before punct.
static size_t find_sentence_end(const String& s) {
  size_t n = s.length();
  if (n < 4) return 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '.' || c == '!' || c == '?') {
      // Skip ellipsis / decimal-ish: require some prior non-space content.
      size_t content = 0;
      for (size_t j = 0; j < i; j++) {
        if (s[j] != ' ' && s[j] != '\n' && s[j] != '\t') content++;
      }
      if (content < 3) continue;
      size_t end = i + 1;
      while (end < n && (s[end] == ' ' || s[end] == '\n' || s[end] == '"' ||
                         s[end] == '\'' || s[end] == ')')) {
        end++;
      }
      return end;
    }
  }
  return 0;
}

static String extract_sse_delta_content(const String& data_line) {
  // data: {...}  or data:[DONE]
  if (!data_line.startsWith("data:")) return "";
  String payload = data_line.substring(5);
  payload.trim();
  if (payload.length() == 0 || payload == "[DONE]") return "";

  // Fast path: look for "content":"..." without full JSON parse (ArduinoJson
  // on every tiny delta is expensive on ESP32).
  int key = payload.indexOf("\"content\"");
  if (key < 0) return "";
  int colon = payload.indexOf(':', key + 9);
  if (colon < 0) return "";
  int q1 = payload.indexOf('"', colon + 1);
  if (q1 < 0) return "";
  // Handle null content
  String after = payload.substring(colon + 1);
  after.trim();
  if (after.startsWith("null")) return "";

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
        // Skip \uXXXX — rare in Korean UTF-8 streams from Groq
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

// SSE chat: speaks each finished sentence via TTS as tokens arrive.
// Fills `reply` with the full assistant text. Returns false on hard failure.
bool groq_chat(const String& user_text, String& reply) {
  reply = "";
  if (!ensure_groq_tls(false)) return false;

  append_chat_message("user", user_text.c_str());

  JsonDocument doc;
  JsonArray messages = doc["messages"].to<JsonArray>();
  JsonObject sys = messages.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = SYSTEM_PROMPT;

  JsonDocument hist;
  deserializeJson(hist, chat_history_json);
  for (JsonObject item : hist.as<JsonArray>()) {
    JsonObject m = messages.add<JsonObject>();
    m["role"] = item["role"].as<const char*>();
    m["content"] = item["content"].as<const char*>();
  }

  doc["model"] = LLM_MODEL;
  doc["max_tokens"] = 180;
  doc["temperature"] = 0.55;  // lower = less drift off character/context
  doc["reasoning_effort"] = "none";
  doc["stream"] = true;

  String body;
  serializeJson(doc, body);

  secure_client->printf("POST /openai/v1/chat/completions HTTP/1.1\r\n");
  groq_write_headers("application/json", body.length());
  secure_client->print(body);

  // Parse status + headers (don't buffer body — it's SSE).
  String status_line = secure_client->readStringUntil('\n');
  status_line.trim();
  if (!status_line.startsWith("HTTP/")) {
    groq_after_response(false, false);
    return false;
  }
  int status = status_line.substring(9, 12).toInt();
  bool chunked = false;
  bool keep_alive = true;
  int content_length = -1;
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

  if (status != 200) {
    Serial.printf("ERR: LLM HTTP %d\n", status);
    // Drain a bit of error body for logs
    uint32_t t0 = millis();
    while (secure_client->available() && millis() - t0 < 2000) {
      Serial.write(secure_client->read());
    }
    Serial.println();
    groq_after_response(false, false);
    return false;
  }

  BodyReader br = {secure_client, chunked,
                   chunked ? 0 : (content_length > 0 ? (long)content_length : 100000000L),
                   false};
  String pending;   // unfinished sentence accumulator
  String full;
  full.reserve(256);
  pending.reserve(128);
  bool spoke_any = false;
  uint32_t t_first_tok = 0;
  uint32_t t_start = millis();

  // Read SSE line-by-line from body
  String line_buf;
  line_buf.reserve(256);
  uint8_t tmp[256];
  uint32_t idle_start = millis();

  while (!br.done && millis() - t_start < 45000) {
    int n = body_reader_read(&br, tmp, sizeof(tmp));
    if (n <= 0) {
      if (br.done) break;
      if (millis() - idle_start > 15000) break;
      delay(1);
      continue;
    }
    idle_start = millis();
    for (int i = 0; i < n; i++) {
      char c = (char)tmp[i];
      if (c == '\r') continue;
      if (c == '\n') {
        if (line_buf.length() == 0) {
          // SSE event separator — ignore
        } else {
          String delta = extract_sse_delta_content(line_buf);
          if (delta.length() > 0) {
            if (t_first_tok == 0) {
              t_first_tok = millis() - t_start;
              Serial.printf("llm: first token in %ums\n", (unsigned)t_first_tok);
            }
            full += delta;
            pending += delta;

            size_t cut;
            while ((cut = find_sentence_end(pending)) > 0) {
              String sentence = pending.substring(0, cut);
              sentence.trim();
              pending = pending.substring(cut);
              if (sentence.length() > 0) {
                Serial.printf("llm: speak sentence: %s\n", sentence.c_str());
                if (speak_text_ex(sentence, true)) spoke_any = true;
              }
            }
          }
        }
        line_buf = "";
      } else {
        if (line_buf.length() < 1500) line_buf += c;
      }
    }
  }

  // Flush remaining unfinished text as final sentence
  pending.trim();
  if (pending.length() > 0) {
    Serial.printf("llm: speak tail: %s\n", pending.c_str());
    if (speak_text_ex(pending, true)) spoke_any = true;
  }

  groq_after_response(true, keep_alive);

  reply = full;
  reply.trim();
  if (reply.length() == 0) return false;
  append_chat_message("assistant", reply.c_str());
  Serial.printf("llm: %s (spoke=%d, %ums)\n", reply.c_str(), spoke_any ? 1 : 0,
                (unsigned)(millis() - t_start));
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

String sanitize_tts_text(const String& text) {
  String out = text;
  out.replace("\r", " ");
  out.replace("\n", " ");
  out.replace("\t", " ");
  while (out.indexOf("  ") >= 0) out.replace("  ", " ");
  out.trim();
  if (out.length() == 0) out = "잘 못 들었어요.";
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
  delay(60);  // let DMA drain
  if (speaker_started) set_speaker(false);
  i2s.end();

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

// Stream Google Cloud TTS: play μ-law as base64 arrives (first audio ASAP).
bool google_cloud_tts_stream_play(const String& text) {
  if (strlen(GOOGLE_API_KEY) == 0) return false;
  if (!ensure_tts_tls(false)) return false;

  WiFiClientSecure* client = tts_client;
  String escaped = ssml_escape(text);
  String ssml = String("<speak><prosody pitch=\"") + GOOGLE_TTS_PITCH + "\" rate=\"" +
                GOOGLE_TTS_RATE + "\">" + escaped + "</prosody></speak>";

  JsonDocument doc;
  doc["input"]["ssml"] = ssml;
  doc["voice"]["languageCode"] = GOOGLE_TTS_LANG;
  doc["voice"]["name"] = GOOGLE_TTS_VOICE;
  doc["audioConfig"]["audioEncoding"] = "MULAW";
  doc["audioConfig"]["sampleRateHertz"] = SAMPLE_RATE;

  String body;
  serializeJson(doc, body);
  String path = String("/v1/text:synthesize?key=") + GOOGLE_API_KEY;

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

  // Scan for "audioContent":"
  const char* needle = "\"audioContent\"";
  size_t ni = 0;
  bool in_b64 = false;
  bool found_key = false;
  B64Stream b64 = {{0}, 0};
  uint8_t raw_buf[256];
  size_t raw_n = 0;
  bool speaker_on = false;
  bool i2s_ok = false;
  bool maybe_wav = true;  // detect RIFF wrapper in first decoded bytes
  size_t total_ulaw = 0;
  const size_t OUT_FRAMES = 512;
  int32_t* out = (int32_t*)malloc(OUT_FRAMES * 2 * sizeof(int32_t));
  if (!out) {
    stop_tts_tls();
    return false;
  }

  auto flush_ulaw = [&](const uint8_t* ulaw, size_t n) -> bool {
    // If Google wrapped μ-law in WAV, abort stream → buffered fallback.
    if (maybe_wav && total_ulaw == 0 && n >= 4) {
      if (memcmp(ulaw, "RIFF", 4) == 0) return false;
      maybe_wav = false;
    }
    total_ulaw += n;
    size_t off = 0;
    while (off < n) {
      size_t frames = min(n - off, (size_t)OUT_FRAMES);
      for (size_t i = 0; i < frames; i++) {
        int16_t s = ulaw_to_pcm16(ulaw[off + i]);
        int32_t v = pcm16_to_i2s32(s);
        out[i * 2] = v;
        out[i * 2 + 1] = v;
      }
      if (!speaker_on) {
        set_speaker(true);
        speaker_on = true;
        Serial.printf("tts: first audio in %ums (google stream)\n",
                      (unsigned)(millis() - t_req));
      }
      uint8_t* p = (uint8_t*)out;
      size_t total_bytes = frames * 2 * sizeof(int32_t);
      size_t woff = 0;
      while (woff < total_bytes) {
        size_t w = i2s.write(p + woff, total_bytes - woff);
        if (w == 0) delay(1);
        woff += w;
      }
      off += frames;
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
        // skip : and whitespace until opening quote
        if (c == '"') in_b64 = true;
        continue;
      }
      if (c == '"') {
        // end of base64 — keep reading body after this for keep-alive
        if (raw_n > 0) {
          if (!i2s_ok) {
            if (!begin_i2s_tx()) {
              free(out);
              stop_tts_tls();
              return speaker_on;
            }
            i2s_ok = true;
          }
          if (!flush_ulaw(raw_buf, raw_n)) {
            free(out);
            if (i2s_ok) i2s.end();
            stop_tts_tls();
            Serial.println("tts: google stream got WAV wrapper — use buffered");
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
            if (!i2s_ok) {
              if (!begin_i2s_tx()) {
                free(out);
                stop_tts_tls();
                return false;
              }
              i2s_ok = true;
            }
            if (!flush_ulaw(raw_buf, raw_n)) {
              free(out);
              if (i2s_ok) i2s.end();
              stop_tts_tls();
              Serial.println("tts: google stream got WAV wrapper — use buffered");
              return false;
            }
            raw_n = 0;
          }
        }
      }
    }
  }

  if (raw_n > 0 && i2s_ok) flush_ulaw(raw_buf, raw_n);

  free(out);
  if (i2s_ok) {
    delay(40);
    if (speaker_on) set_speaker(false);
    i2s.end();
  }

  // After streaming TTS, drain any trailing JSON so keep-alive stays clean.
  uint32_t drain_t = millis();
  while (!br.done && millis() - drain_t < 500) {
    uint8_t junk[64];
    int n = body_reader_read(&br, junk, sizeof(junk));
    if (n <= 0) break;
  }

  if (!speaker_on) {
    // Fall through to buffered path
    stop_tts_tls();
    return false;
  }

  tts_tls_last_ok_ms = millis();
  Serial.printf("tts: google stream done (%ums)\n", (unsigned)(millis() - t_req));
  return true;
}

bool google_cloud_tts(const String& text, uint8_t** audio_out, size_t* audio_len, bool* is_mp3) {
  if (is_mp3) *is_mp3 = false;
  if (strlen(GOOGLE_API_KEY) == 0) {
    Serial.println("ERR: GOOGLE_API empty — set in .env and sync_secrets");
    return false;
  }
  if (!ensure_tts_tls(false)) {
    Serial.println("ERR: Google TTS TLS failed");
    return false;
  }

  WiFiClientSecure* client = tts_client;
  String escaped = ssml_escape(text);
  String ssml = String("<speak><prosody pitch=\"") + GOOGLE_TTS_PITCH + "\" rate=\"" +
                GOOGLE_TTS_RATE + "\">" + escaped + "</prosody></speak>";

  // MULAW (~half LINEAR16 download) → expand to PCM16 WAV for play_wav().
  JsonDocument doc;
  doc["input"]["ssml"] = ssml;
  doc["voice"]["languageCode"] = GOOGLE_TTS_LANG;
  doc["voice"]["name"] = GOOGLE_TTS_VOICE;
  doc["audioConfig"]["audioEncoding"] = "MULAW";
  doc["audioConfig"]["sampleRateHertz"] = SAMPLE_RATE;

  String body;
  serializeJson(doc, body);
  String path = String("/v1/text:synthesize?key=") + GOOGLE_API_KEY;

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

  // Google may return raw μ-law OR a WAV wrapper (fmt=7). play_wav() only
  // understands PCM16 — treat μ-law-as-PCM16 and you get ~2× chipmunk speech.
  const uint8_t* ulaw = raw;
  size_t ulaw_n = raw_len;
  uint32_t rate = SAMPLE_RATE;

  if (raw_len >= 44 && memcmp(raw, "RIFF", 4) == 0) {
    const uint8_t* data = nullptr;
    size_t data_len = 0;
    uint16_t channels = 1;
    uint16_t audio_fmt = 0;
    uint16_t bits = 0;
    size_t offset = 12;
    while (offset + 8 <= raw_len) {
      const char* id = (const char*)(raw + offset);
      uint32_t chunk_size = raw[offset + 4] | (raw[offset + 5] << 8) |
                            (raw[offset + 6] << 16) | ((uint32_t)raw[offset + 7] << 24);
      offset += 8;
      if (offset + chunk_size > raw_len) break;
      if (memcmp(id, "fmt ", 4) == 0 && chunk_size >= 16) {
        audio_fmt = raw[offset] | (raw[offset + 1] << 8);
        channels = raw[offset + 2] | (raw[offset + 3] << 8);
        rate = raw[offset + 4] | (raw[offset + 5] << 8) | (raw[offset + 6] << 16) |
               ((uint32_t)raw[offset + 7] << 24);
        bits = raw[offset + 14] | (raw[offset + 15] << 8);
        if (channels == 0) channels = 1;
      } else if (memcmp(id, "data", 4) == 0) {
        data = raw + offset;
        data_len = chunk_size;
        break;
      }
      offset += chunk_size + (chunk_size & 1);  // word-align
    }

    if (audio_fmt == 1 && bits == 16 && data && data_len > 0) {
      // Already PCM16 WAV — pass through (play_wav resamples if needed).
      *audio_out = raw;
      *audio_len = raw_len;
      if (is_mp3) *is_mp3 = false;
      Serial.printf("tts: google ready in %ums (%u wav bytes, pcm16)\n",
                    (unsigned)(millis() - t_req), (unsigned)raw_len);
      return true;
    }
    if ((audio_fmt == 7 || audio_fmt == 6) && data && data_len > 0) {
      ulaw = data;
      ulaw_n = data_len;
    } else if (!data) {
      Serial.println("ERR: Google TTS WAV missing data");
      free(raw);
      return false;
    } else {
      Serial.printf("ERR: Google TTS unsupported WAV fmt=%u bits=%u\n", audio_fmt, bits);
      free(raw);
      return false;
    }
  }

  uint8_t* pcm = nullptr;
  size_t pcm_len = 0;
  if (!ulaw_bytes_to_pcm16(ulaw, ulaw_n, &pcm, &pcm_len) || pcm_len == 0) {
    Serial.println("ERR: Google TTS mulaw decode failed");
    free(raw);
    if (pcm) free(pcm);
    return false;
  }
  free(raw);

  if (rate == 0) rate = SAMPLE_RATE;
  size_t wav_len = 44 + pcm_len;
  uint8_t* wav = (uint8_t*)heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!wav) wav = (uint8_t*)malloc(wav_len);
  if (!wav) {
    free(pcm);
    return false;
  }
  write_wav_header(wav, pcm_len, rate, 1);
  memcpy(wav + 44, pcm, pcm_len);
  free(pcm);

  *audio_out = wav;
  *audio_len = wav_len;
  if (is_mp3) *is_mp3 = false;
  Serial.printf("tts: google ready in %ums (%u wav bytes, mulaw@%uHz)\n",
                (unsigned)(millis() - t_req), (unsigned)wav_len, (unsigned)rate);
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
  set_speaker(false);
  i2s.end();
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

  if (!begin_i2s_tx()) {
    free(out);
    return;
  }

  set_speaker(true);
  MemoryStream playbackSource((uint8_t*)out, out_count * 2 * sizeof(int32_t));
  StreamCopy playbackCopier(i2s, playbackSource);
  playbackSource.begin(speaker_info);

  while (playbackSource) {
    playbackCopier.copy();
  }

  set_speaker(false);
  i2s.end();
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
  Serial.printf("tts text: %s\n", tts_text.c_str());

  uint8_t* audio = nullptr;
  size_t audio_len = 0;
  bool ok = false;
  state = STATE_SPEAK;

  bool google_mp3 = false;
  if (google_cloud_tts_stream_play(tts_text)) {
    Serial.printf("tts provider: Google Cloud stream (%s)\n", GOOGLE_TTS_VOICE);
    ok = true;
  } else if (google_cloud_tts(tts_text, &audio, &audio_len, &google_mp3)) {
    Serial.printf("tts provider: Google Cloud (%s)\n", GOOGLE_TTS_VOICE);
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
  // Idle sockets are refreshed by ensure_*_tls when GROQ_TLS_MAX_IDLE_MS elapses.
  just_played = true;
  noise_floor_rms = 0.012f;
  state = STATE_LISTEN;
}

// Short procedural "thinking" chirp so the child hears an instant reaction
// while STT/LLM run (~280 ms, soft rising 420→680 Hz).
void play_filler_chirp() {
  const size_t n = SAMPLE_RATE * 28 / 100;  // 280 ms
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
    // attack / release
    if (i < SAMPLE_RATE / 50) env = (float)i / (SAMPLE_RATE / 50.0f);
    else if (i > n - SAMPLE_RATE / 25) env = (float)(n - i) / (SAMPLE_RATE / 25.0f);
    float freq = 420.0f + 260.0f * t / 0.28f;
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
  delay(30);
  set_speaker(false);
  i2s.end();
  free(out);
  Serial.println("filler: chirp played");
}

bool run_pipeline_inner() {
  // Immediate acknowledgment — masks STT/LLM wait.
  play_filler_chirp();

  uint8_t* wav = nullptr;
  size_t wav_len = 0;
  if (!build_ulaw_wav(&wav, &wav_len)) {
    // Fallback to PCM16 WAV if μ-law build fails
    size_t pcm_bytes = record_count * sizeof(int16_t);
    wav_len = 44 + pcm_bytes;
    wav = (uint8_t*)heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) wav = (uint8_t*)malloc(wav_len);
    if (!wav) return false;
    write_wav_header(wav, pcm_bytes, SAMPLE_RATE, 1);
    memcpy(wav + 44, record_buf, pcm_bytes);
    Serial.println("stt: upload PCM16 (ulaw build failed)");
  } else {
    Serial.printf("stt: upload μ-law wav %u bytes (pcm would be %u)\n",
                  (unsigned)wav_len, (unsigned)(44 + record_count * 2));
  }

  String transcript;
  uint32_t t_stt = millis();
  if (!groq_post_multipart_stt(wav, wav_len, transcript)) {
    free(wav);
    Serial.println("stt: skipped (request failed or empty)");
    just_played = true;  // flush filler echo
    return true;
  }
  free(wav);
  uint32_t stt_ms = millis() - t_stt;

  if (!is_valid_transcript(transcript)) {
    Serial.printf("stt: skipped (rejected: %s)\n", transcript.c_str());
    just_played = true;
    return true;
  }

  String reply;
  uint32_t t_llm = millis();
  if (!groq_chat(transcript, reply)) return false;  // speaks as sentences stream
  Serial.printf("timing: stt=%ums llm+tts=%ums\n", (unsigned)stt_ms,
                (unsigned)(millis() - t_llm));

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
  if (!wifi_portal_connected()) {
    Serial.println("WARN: WiFi not connected — join AP Talkbot-Setup → http://192.168.4.1");
  } else {
    Serial.printf("tts primary: Google Cloud (%s / %s, pitch %s)\n", GOOGLE_TTS_LANG,
                  GOOGLE_TTS_VOICE, GOOGLE_TTS_PITCH);
    Serial.println("tts fallback: Groq Orpheus (en) or StreamElements");
    Serial.printf("stt language: %s\n", STT_LANGUAGE);
    if (TTS_BOOT_TEST) {
      if (!speak_text("안녕! 나는 디노야. 티라노 인형이야. 같이 놀자!")) {
        Serial.println("WARN: boot TTS test failed");
      }
      end_turn_cleanup();
    }
  }

  Serial.println("ready: VAD listening loop");
}

void loop() {
  maintain_wifi();
  poll_serial_commands();

  if (state != STATE_LISTEN) {
    delay(10);
    return;
  }

  if (stt_cooldown_active()) {
    static uint32_t last_cooldown_log_ms = 0;
    uint32_t now = millis();
    if (now - last_cooldown_log_ms > 5000) {
      Serial.printf("stt: cooldown %us (calls=%u)\n",
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

  if (!record_utterance()) {
    delay(100);
    return;
  }

  state = STATE_PROCESS;
  if (!run_pipeline()) {
    Serial.println("pipeline failed — retrying");
  }
  state = STATE_LISTEN;
  delay(50);
}
