#pragma once

#include <Arduino.h>

// SoftAP + STA 포털: 부팅 시 AP로 설정 페이지를 열고,
// NVS에 저장된 최근 성공 SSID로 자동 접속을 시도한다.

static const char* WIFI_AP_SSID = "Talkbot-Setup";
static const char* WIFI_AP_PASS = "";  // open AP
static const uint8_t WIFI_SAVED_MAX = 5;

void wifi_portal_begin();
void wifi_portal_loop();
bool wifi_portal_connected();
bool wifi_portal_ensure(bool blocking);
void wifi_portal_maintain();
