/**
 * Seeed 공식 KWS + 음성 피드백 (검증된 I2S 분리 방식)
 * KWS: RX_MODE mono — https://wiki.seeedstudio.com/respeaker_streams_i2s_tflite/
 * 재생: TX_MODE stereo 32bit — https://wiki.seeedstudio.com/respeaker_volume/
 */

#include "AudioTools.h"
#include "AudioTools/AudioLibs/TfLiteAudioStream.h"
#include "Wire.h"
#include "model.h"
#include "yes_no_samples.h"

#define AIC3204_ADDR 0x18
#define XMOS_ADDR 0x42

I2SStream i2s;
TfLiteAudioStream tfl;
StreamCopy kwsCopier(tfl, i2s);

AudioInfo speaker_info(16000, 2, 32);
MemoryStream playbackSource(yes_pcm, yes_pcm_len);
StreamCopy playbackCopier(i2s, playbackSource);

const char* kCategoryLabels[4] = {"silence", "unknown", "yes", "no"};
int channels = 1;
int samples_per_second = 16000;

enum PendingClip { CLIP_NONE = 0, CLIP_YES, CLIP_NO };
volatile PendingClip pending_clip = CLIP_NONE;

// 재생 직후 에코/잔향으로 인한 자기-트리거 방지
#define AFTER_PLAY_IGNORE_MS 1500
unsigned long ignore_until_ms = 0;

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

void set_speaker(bool on) {
  xmos_write_1byte(0xF1, 0x10, on ? 1 : 0);
}

void init_codec() {
  Wire.begin(5, 6);
  set_speaker(false);  // 평소에는 스피커를 꺼서 잡음/하울링 차단
  aic3204_write_reg(0x00, 0x01);
  aic3204_write_reg(0x10, 0x3A);
  aic3204_write_reg(0x11, 0x3A);
  aic3204_write_reg(0x12, 0x3A);
  aic3204_write_reg(0x13, 0x3A);
}

void begin_kws_i2s() {
  auto cfg = i2s.defaultConfig(RX_MODE);
  cfg.channels = channels;
  cfg.sample_rate = samples_per_second;
  cfg.use_apll = false;
  cfg.buffer_size = 512;
  cfg.buffer_count = 16;
  cfg.pin_bck = 8;
  cfg.pin_ws = 7;
  cfg.pin_data_rx = 44;
  cfg.is_master = false;
  i2s.begin(cfg);
}

void begin_speaker_i2s() {
  auto cfg = i2s.defaultConfig(TX_MODE);
  cfg.copyFrom(speaker_info);
  cfg.pin_bck = 8;
  cfg.pin_ws = 7;
  cfg.pin_data = 43;
  cfg.pin_data_rx = 44;
  cfg.is_master = false;
  i2s.begin(cfg);
}

void reset_kws() {
  auto tcfg = tfl.defaultConfig();
  tcfg.setCategories(kCategoryLabels);
  tcfg.channels = channels;
  tcfg.sample_rate = samples_per_second;
  tcfg.kTensorArenaSize = 10 * 1024;
  tcfg.respondToCommand = respondToCommand;
  tcfg.model = g_model;
  tfl.begin(tcfg);
}

void play_pcm(const uint8_t* data, size_t len) {
  i2s.end();
  begin_speaker_i2s();
  set_speaker(true);
  playbackSource.setValue(data, static_cast<int>(len), FLASH_RAM);
  playbackSource.begin(speaker_info);
  while (playbackSource) {
    playbackCopier.copy();
  }
  set_speaker(false);
  i2s.end();
  begin_kws_i2s();

  // 인식기 내부 버퍼를 비워 재생음 잔상으로 인한 재트리거 방지
  reset_kws();
  pending_clip = CLIP_NONE;
  ignore_until_ms = millis() + AFTER_PLAY_IGNORE_MS;
}

void respondToCommand(const char* found_command, uint8_t score, bool is_new_command) {
  (void)score;
  if (millis() < ignore_until_ms) {
    return;
  }
  if (!is_new_command) {
    return;
  }
  if (strcmp(found_command, "yes") == 0) {
    if (Serial) Serial.println("yes");
    pending_clip = CLIP_YES;
  } else if (strcmp(found_command, "no") == 0) {
    if (Serial) Serial.println("no");
    pending_clip = CLIP_NO;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  init_codec();
  begin_kws_i2s();
  reset_kws();
  ignore_until_ms = millis() + 2000;  // 부팅 직후 오검출 무시
}

void loop() {
  kwsCopier.copy();

  PendingClip clip = pending_clip;
  if (clip == CLIP_NONE) {
    return;
  }
  pending_clip = CLIP_NONE;

  if (clip == CLIP_YES) {
    play_pcm(yes_pcm, yes_pcm_len);
  } else if (clip == CLIP_NO) {
    play_pcm(no_pcm, no_pcm_len);
  }
}
