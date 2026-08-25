/**
 * talkbot: Groq STT → LLM → TTS 음성 챗봇 (ESP32-S3 + ReSpeaker Lite)
 *
 * I2S 마이크 녹음 → Groq Whisper STT → Groq Chat → Groq Orpheus TTS → I2S 스피커
 *
 * 사전 준비:
 *   1. .env 에 WIFI_SSID, WIFI_PASSWORD, GROQ_API 설정
 *   2. ./scripts/sync_secrets.sh
 *   3. Groq 콘솔에서 Orpheus TTS 약관 수락
 *   4. ./scripts/upload_sketch.sh 05_groq_voice_chat
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

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Run ./scripts/sync_secrets.sh after setting .env"
#endif

#define AIC3204_ADDR 0x18
#define XMOS_ADDR 0x42

static const char* GROQ_HOST = "api.groq.com";
static const uint16_t GROQ_PORT = 443;

static const char* STT_MODEL = "whisper-large-v3-turbo";
static const char* LLM_MODEL = "groq/compound-mini";
static const char* TTS_MODEL = "canopylabs/orpheus-v1-english";
static const char* TTS_VOICE = "hannah";
static const char* TTS_FALLBACK_VOICE = "Brian";
static const char* STT_LANGUAGE = "ko";
static const bool TTS_BOOT_TEST = true;
static const size_t TTS_MAX_CHARS = 200;

static const size_t SAMPLE_RATE = 16000;
static const size_t MAX_RECORD_SEC = 8;
static const size_t MAX_SAMPLES = SAMPLE_RATE * MAX_RECORD_SEC;

// Adaptive energy VAD (absolute floors + SNR vs estimated noise)
static const float VAD_ABS_MIN = 0.012f;           // never trigger below this
static const float VAD_ONSET_SNR = 2.4f;           // window RMS / noise floor to start
static const float VAD_END_SNR = 1.5f;             // below this → silence for end
static const float SPEECH_MIN_SNR = 2.0f;          // whole-buffer avg vs noise
static const float SPEECH_PEAK_SNR = 3.2f;         // peak windows vs noise
static const float SPEECH_ABS_MIN = 0.018f;        // absolute avg floor
static const float SPEECH_PEAK_ABS = 0.028f;       // absolute peak floor
static const float SPEECH_MOD_MIN = 0.18f;         // CV of frame RMS (speech modulates)
static const float ZCR_SPEECH_MIN = 0.015f;        // too low → hum/tone
static const float ZCR_SPEECH_MAX = 0.38f;         // too high → hiss/white noise
static const float NOISE_FLOOR_MIN = 0.0015f;
static const float NOISE_FLOOR_MAX = 0.035f;
static const float NOISE_EMA_FAST = 0.12f;
static const float NOISE_EMA_SLOW = 0.03f;
// Endpointing: balance latency vs cut-off (too short → weak clips / STT misses)
static const uint32_t SILENCE_MS_SHORT = 550;
static const uint32_t SILENCE_MS_LONG = 650;
static const uint32_t SILENCE_ADAPT_AFTER_MS = 1500;
static const uint32_t MAX_WAIT_SPEECH_MS = 5000;
static const size_t VAD_START_SAMPLES = 640;       // 40 ms — speech onset
static const size_t VAD_END_SAMPLES = 320;         // 20 ms — end detection
static const size_t VAD_START_HITS = 3;            // consecutive speech-like windows
static const size_t NOISE_CALIB_SAMPLES = SAMPLE_RATE / 4;  // 250 ms ambient calib
static const size_t MIN_SPEECH_SAMPLES = SAMPLE_RATE * 4 / 10;  // 0.4 s
static const size_t SPEECH_PEAK_WINDOWS_MIN = 4;   // ~80 ms above peak
static const size_t SPEECH_VOICED_WINDOWS_MIN = 3; // frames with speech-like ZCR+energy
static const size_t PREROLL_SAMPLES = SAMPLE_RATE * 3 / 10;  // 300 ms kept before onset
static const uint32_t POST_PLAY_FLUSH_MS = 350;    // drop mic input after speaker off
static const uint32_t TAIL_KEEP_MS = 200;          // trailing silence kept in upload
static const uint32_t STT_MIN_INTERVAL_MS = 3000;  // min gap between STT calls
static const uint32_t STT_DAILY_LIMIT_BACKOFF_MS = 4UL * 3600UL * 1000UL;

static float noise_floor_rms = 0.008f;

static uint32_t stt_last_call_ms = 0;
static uint32_t stt_blocked_until_ms = 0;
static uint32_t stt_call_count = 0;

static const char* SYSTEM_PROMPT =
    "You are Dino, a friendly baby Tyrannosaurus voice companion on a small speaker device. "
    "The user speaks through speech-to-text; it may be Korean or mixed. First understand what they mean, "
    "then reply in English only, as if translating and answering in one natural spoken reply. "
    "Stay in character as a curious, warm baby T-Rex named Dino. "
    "Use 1-2 short sentences, plain ASCII, no markdown, no Korean characters.";

I2SStream i2s;
AudioInfo speaker_info(SAMPLE_RATE, 2, 32);

int16_t* record_buf = nullptr;
size_t record_count = 0;

WiFiClientSecure* secure_client = nullptr;
String chat_history_json = "[]";

// Net task (core 0) owns TLS handshakes; net_mutex serializes all secure_client use.
// Preconnect fires at speech onset so the handshake overlaps with the user talking.
static SemaphoreHandle_t net_mutex = nullptr;
static volatile bool tls_preconnect_req = false;
static uint32_t groq_tls_last_ok_ms = 0;
static bool just_played = false;

enum State { STATE_LISTEN, STATE_PROCESS, STATE_SPEAK };
State state = STATE_LISTEN;

void request_tls_preconnect();
bool ensure_groq_tls(bool force_reconnect);
void stop_groq_tls();

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

void init_codec() {
  Wire.begin(5, 6);
  set_speaker(false);
  aic3204_write_reg(0x00, 0x01);
  aic3204_write_reg(0x10, 0x3A);
  aic3204_write_reg(0x11, 0x3A);
  aic3204_write_reg(0x12, 0x3A);
  aic3204_write_reg(0x13, 0x3A);
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
  return t > VAD_ABS_MIN ? t : VAD_ABS_MIN;
}

float vad_end_threshold() {
  float t = noise_floor_rms * VAD_END_SNR;
  float floor = VAD_ABS_MIN * 0.75f;
  return t > floor ? t : floor;
}

bool window_looks_like_speech(float rms, float zcr) {
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
  float gate = noise_floor_rms * 1.8f;
  if (gate < 0.004f) gate = 0.004f;

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
  uint32_t now = millis();
  if (now < stt_blocked_until_ms) return true;
  if (stt_last_call_ms > 0 && now - stt_last_call_ms < STT_MIN_INTERVAL_MS) return true;
  return false;
}

uint32_t stt_cooldown_remaining_ms() {
  uint32_t now = millis();
  uint32_t block = (now < stt_blocked_until_ms) ? (stt_blocked_until_ms - now) : 0;
  uint32_t interval = 0;
  if (stt_last_call_ms > 0 && now - stt_last_call_ms < STT_MIN_INTERVAL_MS) {
    interval = STT_MIN_INTERVAL_MS - (now - stt_last_call_ms);
  }
  return block > interval ? block : interval;
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
  if (min_avg < SPEECH_ABS_MIN) min_avg = SPEECH_ABS_MIN;
  if (avg < min_avg) {
    Serial.printf("listen: reject quiet (avg=%.3f need=%.3f nf=%.3f)\n",
                  avg, min_avg, noise_floor_rms);
    return false;
  }

  float peak_need = noise_floor_rms * SPEECH_PEAK_SNR;
  if (peak_need < SPEECH_PEAK_ABS) peak_need = SPEECH_PEAK_ABS;

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
    if (w >= peak_need * 0.85f && z >= ZCR_SPEECH_MIN && z <= ZCR_SPEECH_MAX) {
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
  float calib_min = 1.0f;
  size_t calib_n = 0;
  uint32_t silence_start = 0;
  uint32_t wait_start = millis();

  while (record_count < MAX_SAMPLES) {
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
          // Quietest window is robust against residual echo/decay in the calib slice.
          noise_floor_rms = calib_min * 1.35f;
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
        start_hits++;
        if (start_hits >= VAD_START_HITS) {
          started = true;
          silence_start = 0;
          Serial.printf("listen: voice detected (rms=%.3f zcr=%.2f)\n", rms, zcr);
          // TLS handshake on core 0 overlaps with the rest of the utterance.
          request_tls_preconnect();
        }
      } else {
        start_hits = 0;
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
    Serial.println("listen: no speech captured");
    return false;
  }

  denoise_recording();
  return true;
}

// --- HTTP / Groq ---

static uint32_t last_wifi_attempt_ms = 0;
static const uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
static const uint32_t GROQ_TLS_MAX_IDLE_MS = 25000;

// Runs on core 0. Sole job: open the TLS session early, under net_mutex,
// while core 1 keeps recording. All other secure_client use happens on the
// main task, also under net_mutex, so access is fully serialized.
void net_task(void* /*arg*/) {
  for (;;) {
    if (tls_preconnect_req) {
      tls_preconnect_req = false;
      if (xSemaphoreTake(net_mutex, portMAX_DELAY) == pdTRUE) {
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
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void request_tls_preconnect() {
  tls_preconnect_req = true;
}

void stop_groq_tls() {
  if (secure_client && secure_client->connected()) {
    secure_client->stop();
  }
  groq_tls_last_ok_ms = 0;
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

void groq_write_headers(const char* content_type, size_t content_length) {
  secure_client->printf("Host: %s\r\n", GROQ_HOST);
  secure_client->printf("Authorization: Bearer %s\r\n", GROQ_API_KEY);
  secure_client->printf("Content-Type: %s\r\n", content_type);
  secure_client->print("Connection: keep-alive\r\n");
  secure_client->printf("Content-Length: %u\r\n\r\n", (unsigned)content_length);
}

void groq_after_response(bool ok, bool keep_alive) {
  if (!ok || !keep_alive) {
    stop_groq_tls();
  } else {
    groq_tls_last_ok_ms = millis();
  }
}

void init_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setSleep(WIFI_PS_NONE);
}

bool connect_wifi(bool blocking) {
  if (WiFi.status() == WL_CONNECTED) return true;

  uint32_t now = millis();
  if (now - last_wifi_attempt_ms < WIFI_RETRY_INTERVAL_MS) return false;
  last_wifi_attempt_ms = now;

  wl_status_t status = WiFi.status();
  if (status != WL_CONNECTED) {
    Serial.printf("wifi: connecting to %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  if (!blocking) return false;

  for (int i = 0; i < 60; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("wifi: connected %s\n", WiFi.localIP().toString().c_str());
      return true;
    }
    delay(250);
  }
  Serial.println("ERR: WiFi failed — will retry");
  return false;
}

void maintain_wifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  connect_wifi(false);
}

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
  while (arr.size() > 6) arr.remove(0);
  chat_history_json = "";
  serializeJson(arr, chat_history_json);
}

bool groq_chat(const String& user_text, String& reply) {
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
  doc["temperature"] = 0.6;

  String body;
  serializeJson(doc, body);

  secure_client->printf("POST /openai/v1/chat/completions HTTP/1.1\r\n");
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

  if (status != 200) {
    Serial.printf("ERR: LLM HTTP %d\n", status);
    if (resp) {
      Serial.write(resp, min(resp_len, (size_t)512));
      Serial.println();
      free(resp);
    }
    groq_after_response(false, false);
    return false;
  }

  groq_after_response(true, keep_alive);

  JsonDocument out;
  if (deserializeJson(out, resp, resp_len)) {
    free(resp);
    return false;
  }
  free(resp);

  reply = out["choices"][0]["message"]["content"].as<String>();
  reply.trim();
  append_chat_message("assistant", reply.c_str());
  Serial.printf("llm: %s\n", reply.c_str());
  return reply.length() > 0;
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

bool google_cloud_tts(const String& text, uint8_t** wav_out, size_t* wav_len) {
  if (strlen(GOOGLE_API_KEY) == 0) return false;
  if (!connect_wifi(true)) return false;
  stop_groq_tls();  // shared client — other host
  if (!secure_client->connect("texttospeech.googleapis.com", 443)) return false;

  JsonDocument doc;
  doc["input"]["text"] = text;
  doc["voice"]["languageCode"] = "en-US";
  doc["voice"]["name"] = "en-US-Standard-C";
  doc["audioConfig"]["audioEncoding"] = "LINEAR16";
  doc["audioConfig"]["sampleRateHertz"] = SAMPLE_RATE;

  String body;
  serializeJson(doc, body);
  String path = String("/v1/text:synthesize?key=") + GOOGLE_API_KEY;

  secure_client->printf("POST %s HTTP/1.1\r\n", path.c_str());
  secure_client->print("Host: texttospeech.googleapis.com\r\n");
  secure_client->print("Content-Type: application/json\r\n");
  secure_client->printf("Content-Length: %u\r\n\r\n", (unsigned)body.length());
  secure_client->print(body);

  int status = 0;
  uint8_t* resp = nullptr;
  size_t resp_len = 0;
  if (!read_http_response(secure_client, &status, &resp, &resp_len, nullptr)) {
    secure_client->stop();
    return false;
  }
  secure_client->stop();
  if (status != 200) {
    Serial.printf("ERR: Google TTS HTTP %d\n", status);
    if (resp) free(resp);
    return false;
  }

  JsonDocument out;
  if (deserializeJson(out, resp, resp_len)) {
    free(resp);
    return false;
  }
  free(resp);

  const char* b64 = out["audioContent"];
  if (!b64) return false;

  uint8_t* pcm = nullptr;
  size_t pcm_len = 0;
  if (!base64_decode(b64, strlen(b64), &pcm, &pcm_len) || pcm_len == 0) {
    if (pcm) free(pcm);
    return false;
  }

  size_t total = 44 + pcm_len;
  uint8_t* wav = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!wav) wav = (uint8_t*)malloc(total);
  if (!wav) {
    free(pcm);
    return false;
  }
  write_wav_header(wav, pcm_len, SAMPLE_RATE, 1);
  memcpy(wav + 44, pcm, pcm_len);
  free(pcm);

  *wav_out = wav;
  *wav_len = total;
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
    int32_t v = ((int32_t)s) << 16;
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
  String out = english_for_tts(text);
  if (out.length() > TTS_MAX_CHARS) out = out.substring(0, TTS_MAX_CHARS);
  return out;
}

bool speak_text(const String& text) {
  String tts_text = prepare_tts_text(text);
  Serial.printf("tts text: %s\n", tts_text.c_str());

  uint8_t* wav = nullptr;
  size_t wav_len = 0;
  bool ok = false;
  if (groq_tts(tts_text, &wav, &wav_len)) {
    Serial.println("tts provider: Groq Orpheus");
    state = STATE_SPEAK;
    play_wav(wav, wav_len);
    free(wav);
    ok = true;
  } else if (google_cloud_tts(tts_text, &wav, &wav_len)) {
    Serial.println("tts provider: Google Cloud");
    state = STATE_SPEAK;
    play_wav(wav, wav_len);
    free(wav);
    ok = true;
  } else {
    uint8_t* mp3 = nullptr;
    size_t mp3_len = 0;
    if (streamelements_tts(tts_text, &mp3, &mp3_len)) {
      Serial.println("tts provider: StreamElements fallback");
      state = STATE_SPEAK;
      play_mp3(mp3, mp3_len);
      free(mp3);
      ok = true;
    }
  }

  // End of turn: drop keep-alive so the next listen doesn't reuse a stale socket
  stop_groq_tls();
  just_played = true;  // next listen flushes speaker echo tail before calib
  state = STATE_LISTEN;

  if (!ok) {
    Serial.println("ERR: TTS failed (Groq Orpheus terms / GOOGLE_API / fallback)");
  }
  return ok;
}

bool run_pipeline_inner() {
  size_t pcm_bytes = record_count * sizeof(int16_t);
  size_t wav_len = 44 + pcm_bytes;
  uint8_t* wav = (uint8_t*)heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!wav) wav = (uint8_t*)malloc(wav_len);
  if (!wav) return false;

  write_wav_header(wav, pcm_bytes, SAMPLE_RATE, 1);
  memcpy(wav + 44, record_buf, pcm_bytes);

  String transcript;
  uint32_t t_stt = millis();
  if (!groq_post_multipart_stt(wav, wav_len, transcript)) {
    free(wav);
    Serial.println("stt: skipped (request failed or empty)");
    return true;
  }
  free(wav);
  uint32_t stt_ms = millis() - t_stt;

  if (!is_valid_transcript(transcript)) {
    Serial.printf("stt: skipped (rejected: %s)\n", transcript.c_str());
    return true;
  }

  String reply;
  uint32_t t_llm = millis();
  if (!groq_chat(transcript, reply)) return false;
  Serial.printf("timing: stt=%ums llm=%ums\n", (unsigned)stt_ms,
                (unsigned)(millis() - t_llm));

  if (!speak_text(reply)) return false;
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

  net_mutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(net_task, "net", 16384, nullptr, 1, nullptr, 0);

  init_wifi();
  connect_wifi(true);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WARN: WiFi not connected — will retry in loop");
  } else {
    Serial.printf("tts primary: Groq Orpheus (%s / %s)\n", TTS_MODEL, TTS_VOICE);
    Serial.println("tts fallback: Google Cloud TTS or StreamElements");
    if (TTS_BOOT_TEST) {
      if (!speak_text("Hi, I am Dino! Ready to chat.")) {
        Serial.println("WARN: boot TTS test failed");
      }
    }
  }

  Serial.println("ready: VAD listening loop");
}

void loop() {
  maintain_wifi();

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

  if (!record_utterance()) {
    delay(100);
    return;
  }

  state = STATE_PROCESS;
  if (!run_pipeline()) {
    Serial.println("pipeline failed — retrying");
  }
  state = STATE_LISTEN;
  delay(150);
}
