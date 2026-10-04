#include "device_config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef CONTROL_PANEL_URL
#define CONTROL_PANEL_URL "https://talkbot-control-panel.nine-raptorex.workers.dev"
#endif

#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_mac.h>
#include "mbedtls/sha256.h"

static Preferences prefs;
static DeviceRuntimeConfig cfg;
static char pair_url_buf[220];
static uint32_t last_sync_ms = 0;
static bool identity_ready = false;

static void hex_of(const uint8_t* bytes, size_t n, char* out, size_t out_len) {
  static const char* hex = "0123456789abcdef";
  size_t need = n * 2 + 1;
  if (out_len < need) {
    out[0] = 0;
    return;
  }
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = hex[(bytes[i] >> 4) & 0xF];
    out[i * 2 + 1] = hex[bytes[i] & 0xF];
  }
  out[n * 2] = 0;
}

// Deterministic pair secret from MAC so floor QR sticker matches firmware forever.
static void derive_identity(char* device_id, size_t id_len, char* pair_secret, size_t secret_len) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char mac_hex[13];
  hex_of(mac, 6, mac_hex, sizeof(mac_hex));
  snprintf(device_id, id_len, "tb-%s", mac_hex);

  const char salt[] = "talkbot-pair-v1";
  uint8_t msg[sizeof(salt) - 1 + 6];
  memcpy(msg, salt, sizeof(salt) - 1);
  memcpy(msg + sizeof(salt) - 1, mac, 6);
  uint8_t hash[32];
  mbedtls_sha256(msg, sizeof(msg), hash, 0);
  hex_of(hash, 8, pair_secret, secret_len);
}

static void ensure_identity() {
  if (identity_ready) return;

  prefs.begin("devcfg", false);
  uint8_t psver = prefs.getUChar("psver", 0);
  String id = prefs.getString("id", "");
  String secret = prefs.getString("secret", "");

  // psver=1: MAC-derived id/secret (matches printed bottom QR).
  if (psver != 1 || id.length() < 8 || secret.length() < 8) {
    derive_identity(cfg.device_id, sizeof(cfg.device_id), cfg.pair_secret, sizeof(cfg.pair_secret));
    prefs.putUChar("psver", 1);
    prefs.putString("id", cfg.device_id);
    prefs.putString("secret", cfg.pair_secret);
    // New identity → clear old cloud token
    prefs.putString("token", "");
    Serial.printf("device: identity id=%s (QR-ready)\n", cfg.device_id);
  } else {
    strncpy(cfg.device_id, id.c_str(), sizeof(cfg.device_id) - 1);
    cfg.device_id[sizeof(cfg.device_id) - 1] = 0;
    strncpy(cfg.pair_secret, secret.c_str(), sizeof(cfg.pair_secret) - 1);
    cfg.pair_secret[sizeof(cfg.pair_secret) - 1] = 0;
  }

  String token = prefs.getString("token", "");
  strncpy(cfg.device_token, token.c_str(), sizeof(cfg.device_token) - 1);
  cfg.device_token[sizeof(cfg.device_token) - 1] = 0;
  cfg.paired = cfg.device_token[0] != 0;
  cfg.config_rev = prefs.getUInt("rev", 0);

  String voice = prefs.getString("voice", "");
  if (voice.length()) {
    strncpy(cfg.voice, voice.c_str(), sizeof(cfg.voice) - 1);
  }
  String persona = prefs.getString("persona", "");
  if (persona.length()) {
    strncpy(cfg.persona_extra, persona.c_str(), sizeof(cfg.persona_extra) - 1);
  }
  String boot = prefs.getString("boot", "");
  if (boot.length()) {
    strncpy(cfg.boot_phrase, boot.c_str(), sizeof(cfg.boot_phrase) - 1);
  }
  String groq = prefs.getString("groq", "");
  if (groq.length()) {
    strncpy(cfg.groq_api_key, groq.c_str(), sizeof(cfg.groq_api_key) - 1);
  }
  String google = prefs.getString("google", "");
  if (google.length()) {
    strncpy(cfg.google_api_key, google.c_str(), sizeof(cfg.google_api_key) - 1);
  }

  prefs.end();
  identity_ready = true;
}

static void persist_runtime() {
  prefs.begin("devcfg", false);
  prefs.putString("token", cfg.device_token);
  prefs.putUInt("rev", cfg.config_rev);
  prefs.putString("voice", cfg.voice);
  prefs.putString("persona", cfg.persona_extra);
  prefs.putString("boot", cfg.boot_phrase);
  if (cfg.groq_api_key[0]) prefs.putString("groq", cfg.groq_api_key);
  if (cfg.google_api_key[0]) prefs.putString("google", cfg.google_api_key);
  prefs.end();
}

static void rebuild_pair_url() {
  // Floor QR target: https://<panel>/?d=<id>&t=<secret> → opens control panel immediately.
  String base = CONTROL_PANEL_URL;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  snprintf(pair_url_buf, sizeof(pair_url_buf), "%s/?d=%s&t=%s", base.c_str(), cfg.device_id,
           cfg.pair_secret);
}

void device_config_begin(const char* default_voice, const char* default_boot_phrase,
                         const char* default_groq_key, const char* default_google_key) {
  memset(&cfg, 0, sizeof(cfg));
  if (default_voice && default_voice[0]) {
    strncpy(cfg.voice, default_voice, sizeof(cfg.voice) - 1);
  } else {
    strncpy(cfg.voice, "ko-KR-Chirp3-HD-Kore", sizeof(cfg.voice) - 1);
  }
  if (default_boot_phrase && default_boot_phrase[0]) {
    strncpy(cfg.boot_phrase, default_boot_phrase, sizeof(cfg.boot_phrase) - 1);
  } else {
    strncpy(cfg.boot_phrase, "Hi, I'm Dino! Let's play English!", sizeof(cfg.boot_phrase) - 1);
  }
  if (default_groq_key) {
    strncpy(cfg.groq_api_key, default_groq_key, sizeof(cfg.groq_api_key) - 1);
  }
  if (default_google_key) {
    strncpy(cfg.google_api_key, default_google_key, sizeof(cfg.google_api_key) - 1);
  }
  ensure_identity();
  rebuild_pair_url();
}

const DeviceRuntimeConfig& device_config_get() { return cfg; }

const char* device_config_pair_url() {
  rebuild_pair_url();
  return pair_url_buf;
}

void device_config_print_pair_info() {
  rebuild_pair_url();
  Serial.println("---- talkbot pair ----");
  Serial.printf("device_id: %s\n", cfg.device_id);
  Serial.printf("paired: %s\n", cfg.paired ? "yes" : "no");
  Serial.printf("voice: %s\n", cfg.voice);
  Serial.printf("config_rev: %u\n", (unsigned)cfg.config_rev);
  Serial.printf("QR/open: %s\n", pair_url_buf);
  Serial.println("----------------------");
}

static bool parse_url(const String& url, bool* https, String* host, uint16_t* port, String* path) {
  *https = url.startsWith("https://");
  bool http = url.startsWith("http://");
  if (!*https && !http) return false;
  int start = *https ? 8 : 7;
  int slash = url.indexOf('/', start);
  String hostport = slash < 0 ? url.substring(start) : url.substring(start, slash);
  *path = slash < 0 ? "/" : url.substring(slash);
  int colon = hostport.indexOf(':');
  if (colon >= 0) {
    *host = hostport.substring(0, colon);
    *port = (uint16_t)hostport.substring(colon + 1).toInt();
  } else {
    *host = hostport;
    *port = *https ? 443 : 80;
  }
  return host->length() > 0;
}

static bool http_json(const char* method, const String& full_url, const String& body,
                      const char* auth_bearer, int* status_out, String* response_out) {
  bool https = false;
  String host, path;
  uint16_t port = 80;
  if (!parse_url(full_url, &https, &host, &port, &path)) {
    Serial.println("device_config: bad CONTROL_PANEL_URL");
    return false;
  }

  WiFiClient plain;
  WiFiClientSecure secure;
  Client* client = nullptr;
  if (https) {
    secure.setInsecure();
    secure.setTimeout(15000);
    if (!secure.connect(host.c_str(), port)) {
      Serial.printf("device_config: TLS connect fail %s:%u\n", host.c_str(), port);
      return false;
    }
    client = &secure;
  } else {
    plain.setTimeout(15000);
    if (!plain.connect(host.c_str(), port)) {
      Serial.printf("device_config: TCP connect fail %s:%u\n", host.c_str(), port);
      return false;
    }
    client = &plain;
  }

  client->printf("%s %s HTTP/1.1\r\n", method, path.c_str());
  client->printf("Host: %s\r\n", host.c_str());
  client->print("Connection: close\r\n");
  client->print("Accept: application/json\r\n");
  if (auth_bearer && auth_bearer[0]) {
    client->printf("Authorization: Bearer %s\r\n", auth_bearer);
  }
  if (body.length()) {
    client->print("Content-Type: application/json\r\n");
    client->printf("Content-Length: %u\r\n", (unsigned)body.length());
  }
  client->print("\r\n");
  if (body.length()) client->print(body);

  String status_line = client->readStringUntil('\n');
  status_line.trim();
  int status = 0;
  if (status_line.startsWith("HTTP/")) status = status_line.substring(9, 12).toInt();
  *status_out = status;

  // Skip headers
  while (client->connected() || client->available()) {
    String line = client->readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
  }

  *response_out = "";
  uint32_t t0 = millis();
  while (millis() - t0 < 12000) {
    while (client->available()) {
      char c = (char)client->read();
      response_out->concat(c);
      if (response_out->length() > 4096) break;
    }
    if (!client->connected() && !client->available()) break;
    delay(5);
  }
  client->stop();
  return status > 0;
}

static bool apply_config_json(JsonObject obj) {
  bool changed = false;
  if (obj["device_token"].is<const char*>()) {
    const char* tok = obj["device_token"];
    if (tok && tok[0] && strcmp(cfg.device_token, tok) != 0) {
      strncpy(cfg.device_token, tok, sizeof(cfg.device_token) - 1);
      cfg.device_token[sizeof(cfg.device_token) - 1] = 0;
      cfg.paired = true;
      changed = true;
    }
  }
  uint32_t rev = obj["config_rev"] | cfg.config_rev;
  if (obj["voice"].is<const char*>()) {
    const char* v = obj["voice"];
    if (v && strcmp(cfg.voice, v) != 0) {
      strncpy(cfg.voice, v, sizeof(cfg.voice) - 1);
      cfg.voice[sizeof(cfg.voice) - 1] = 0;
      changed = true;
    }
  }
  if (obj["persona_extra"].is<const char*>()) {
    const char* p = obj["persona_extra"];
    if (p && strcmp(cfg.persona_extra, p) != 0) {
      strncpy(cfg.persona_extra, p, sizeof(cfg.persona_extra) - 1);
      cfg.persona_extra[sizeof(cfg.persona_extra) - 1] = 0;
      changed = true;
    }
  }
  if (obj["boot_phrase"].is<const char*>()) {
    const char* b = obj["boot_phrase"];
    if (b && strcmp(cfg.boot_phrase, b) != 0) {
      strncpy(cfg.boot_phrase, b, sizeof(cfg.boot_phrase) - 1);
      cfg.boot_phrase[sizeof(cfg.boot_phrase) - 1] = 0;
      changed = true;
    }
  }
  if (obj["groq_api_key"].is<const char*>()) {
    const char* k = obj["groq_api_key"];
    if (k && k[0] && strcmp(cfg.groq_api_key, k) != 0) {
      strncpy(cfg.groq_api_key, k, sizeof(cfg.groq_api_key) - 1);
      cfg.groq_api_key[sizeof(cfg.groq_api_key) - 1] = 0;
      changed = true;
    }
  }
  if (obj["google_api_key"].is<const char*>()) {
    const char* k = obj["google_api_key"];
    if (k && k[0] && strcmp(cfg.google_api_key, k) != 0) {
      strncpy(cfg.google_api_key, k, sizeof(cfg.google_api_key) - 1);
      cfg.google_api_key[sizeof(cfg.google_api_key) - 1] = 0;
      changed = true;
    }
  }
  if (rev != cfg.config_rev) {
    cfg.config_rev = rev;
    changed = true;
  }
  return changed;
}

bool device_config_sync() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String base = CONTROL_PANEL_URL;
  while (base.endsWith("/")) base.remove(base.length() - 1);

  int status = 0;
  String resp;
  bool changed = false;

  // Always try claim — works for first pair and refreshes token if needed.
  {
    JsonDocument req;
    req["device_id"] = cfg.device_id;
    req["pair_secret"] = cfg.pair_secret;
    String body;
    serializeJson(req, body);
    String url = base + "/api/device/claim";
    if (http_json("POST", url, body, nullptr, &status, &resp)) {
      if (status == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, resp)) {
          if (apply_config_json(doc.as<JsonObject>())) changed = true;
          Serial.printf("device_config: claim ok paired=%d rev=%u\n", (int)cfg.paired,
                        (unsigned)cfg.config_rev);
        }
      } else if (status == 404) {
        Serial.println("device_config: not paired yet — scan QR / open pair URL");
      } else {
        Serial.printf("device_config: claim HTTP %d %s\n", status, resp.c_str());
      }
    }
  }

  if (cfg.device_token[0]) {
    String url = base + "/api/device/config";
    status = 0;
    resp = "";
    if (http_json("GET", url, "", cfg.device_token, &status, &resp)) {
      if (status == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, resp)) {
          if (apply_config_json(doc.as<JsonObject>())) changed = true;
        }
      } else {
        Serial.printf("device_config: config HTTP %d\n", status);
      }
    }
  }

  if (changed) {
    persist_runtime();
    Serial.printf("device_config: applied voice=%s rev=%u\n", cfg.voice, (unsigned)cfg.config_rev);
  }
  last_sync_ms = millis();
  return changed;
}

void device_config_maintain(uint32_t interval_ms) {
  if (WiFi.status() != WL_CONNECTED) return;
  uint32_t now = millis();
  if (last_sync_ms != 0 && now - last_sync_ms < interval_ms) return;
  device_config_sync();
}
