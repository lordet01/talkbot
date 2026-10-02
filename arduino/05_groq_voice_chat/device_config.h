#pragma once

#include <Arduino.h>

// Device identity + cloud config sync.
// SoftAP remains Wi-Fi only; this module talks to the control-panel API.
// CONTROL_PANEL_URL comes from secrets.h (sync_secrets.sh).

#ifndef CONTROL_PANEL_URL
#define CONTROL_PANEL_URL "https://talkbot-control-panel.nine-raptorex.workers.dev"
#endif

static const size_t DC_DEVICE_ID_LEN = 17;      // "tb-" + 12 hex MAC
static const size_t DC_PAIR_SECRET_LEN = 17;    // 16 hex + NUL
static const size_t DC_DEVICE_TOKEN_LEN = 49;
static const size_t DC_VOICE_LEN = 48;
static const size_t DC_PERSONA_EXTRA_LEN = 384;
static const size_t DC_BOOT_PHRASE_LEN = 96;
static const size_t DC_API_KEY_LEN = 128;

struct DeviceRuntimeConfig {
  char device_id[DC_DEVICE_ID_LEN];
  char pair_secret[DC_PAIR_SECRET_LEN];
  char device_token[DC_DEVICE_TOKEN_LEN];
  char voice[DC_VOICE_LEN];
  char persona_extra[DC_PERSONA_EXTRA_LEN];
  char boot_phrase[DC_BOOT_PHRASE_LEN];
  char groq_api_key[DC_API_KEY_LEN];
  char google_api_key[DC_API_KEY_LEN];
  uint32_t config_rev;
  bool paired;
};

void device_config_begin(const char* default_voice, const char* default_boot_phrase,
                         const char* default_groq_key, const char* default_google_key);
const DeviceRuntimeConfig& device_config_get();
const char* device_config_pair_url();
void device_config_print_pair_info();

// Call after Wi-Fi is up. Claims token if unpaired parent completed pairing,
// then pulls latest config. Returns true if config changed.
bool device_config_sync();

// Idle helper — sync at most every interval_ms when Wi-Fi is up.
void device_config_maintain(uint32_t interval_ms = 120000);
