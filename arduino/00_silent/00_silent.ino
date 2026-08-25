/**
 * 오디오 출력 중지. 사인파 테스트 후 스피커를 끕니다.
 */
#include "Wire.h"

#define XMOS_ADDR 0x42

void setup() {
  Wire.begin(5, 6);
  Wire.beginTransmission(XMOS_ADDR);
  Wire.write(0xF1);
  Wire.write(0x10);
  Wire.write(1);
  Wire.write(0);  // mute speaker
  Wire.endTransmission();
}

void loop() {
  delay(1000);
}
