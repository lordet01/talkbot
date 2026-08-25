/**
 * Test 3: I2S → Serial CSV
 * Pins from Seeed ReSpeaker Lite + XIAO ESP32S3 reference
 */
#include "AudioTools.h"

AudioInfo info(16000, 1, 32);
I2SStream i2sStream;
CsvOutput<int32_t> csvStream(Serial);
StreamCopy copier(csvStream, i2sStream);

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("talkbot: I2S CSV setup");

  auto cfg = i2sStream.defaultConfig(RX_MODE);
  cfg.copyFrom(info);
  cfg.pin_bck = 8;
  cfg.pin_ws = 7;
  cfg.pin_data_rx = 44;
  cfg.is_master = false;
  cfg.i2s_format = I2S_STD_FORMAT;
  cfg.use_apll = false;
  i2sStream.begin(cfg);
  csvStream.begin(info);

  Serial.println("# talkbot: I2S CSV stream started");
}

void loop() {
  copier.copy();
}
