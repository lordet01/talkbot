#pragma once
#include <Arduino.h>
#include <Client.h>

// Client* accepts WiFiClient / WiFiClientSecure.
struct BodyReader {
  Client* client;
  bool chunked;
  long remaining;
  bool done;
};

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

static inline int b64_feed(B64Stream* s, char c, uint8_t* dst) {
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
