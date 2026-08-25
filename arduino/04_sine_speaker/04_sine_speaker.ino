/**
 * Seeed 공식: Volume Adjustment + Sine Wave Generator
 * https://wiki.seeedstudio.com/respeaker_volume/
 * https://wiki.seeedstudio.com/respeaker_streams_generator/
 *
 * 깨끗한 사인파가 스피커에서 나와야 합니다.
 */

#include "AudioTools.h"
#include "Wire.h"

#define AIC3204_ADDR 0x18
#define XMOS_ADDR 0x42

AudioInfo info_16k(16000, 2, 32);
SineWaveGenerator<int16_t> sineWave(32000);
GeneratedSoundStream<int16_t> sound(sineWave);
I2SStream out;
StreamCopy copier(out, sound);

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

void setup() {
  Serial.begin(115200);
  Wire.begin(5, 6);

  xmos_write_1byte(0xF1, 0x10, 1);
  aic3204_write_reg(0x00, 0x01);
  aic3204_write_reg(0x10, 0x3A);
  aic3204_write_reg(0x11, 0x3A);
  aic3204_write_reg(0x12, 0x3A);
  aic3204_write_reg(0x13, 0x3A);

  auto cfg = out.defaultConfig(TX_MODE);
  cfg.copyFrom(info_16k);
  cfg.pin_bck = 8;
  cfg.pin_ws = 7;
  cfg.pin_data = 43;
  cfg.pin_data_rx = 44;
  cfg.is_master = false;
  out.begin(cfg);

  sineWave.begin(info_16k, N_B4);
  if (Serial) Serial.println("sine");
}

void loop() {
  copier.copy();
}
