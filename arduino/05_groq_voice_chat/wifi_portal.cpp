#include "wifi_portal.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

struct SavedNet {
  char ssid[33];
  char pass[65];
  uint32_t last_ok;
};

static Preferences prefs;
static WebServer server(80);
static DNSServer dns;
static SavedNet saved[WIFI_SAVED_MAX];
static uint8_t saved_count = 0;
static bool portal_ready = false;
static uint32_t last_wifi_attempt_ms = 0;
static const uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
static char connecting_ssid[33] = "";
static volatile bool connect_busy = false;

static void load_saved() {
  prefs.begin("wificfg", true);
  saved_count = prefs.getUChar("count", 0);
  if (saved_count > WIFI_SAVED_MAX) saved_count = WIFI_SAVED_MAX;
  for (uint8_t i = 0; i < saved_count; i++) {
    char ks[8], kp[8], kt[8];
    snprintf(ks, sizeof(ks), "s%u", i);
    snprintf(kp, sizeof(kp), "p%u", i);
    snprintf(kt, sizeof(kt), "t%u", i);
    String ss = prefs.getString(ks, "");
    String pw = prefs.getString(kp, "");
    strncpy(saved[i].ssid, ss.c_str(), sizeof(saved[i].ssid) - 1);
    saved[i].ssid[sizeof(saved[i].ssid) - 1] = 0;
    strncpy(saved[i].pass, pw.c_str(), sizeof(saved[i].pass) - 1);
    saved[i].pass[sizeof(saved[i].pass) - 1] = 0;
    saved[i].last_ok = prefs.getUInt(kt, 0);
  }
  prefs.end();
}

static void persist_saved() {
  prefs.begin("wificfg", false);
  prefs.clear();
  prefs.putUChar("count", saved_count);
  for (uint8_t i = 0; i < saved_count; i++) {
    char ks[8], kp[8], kt[8];
    snprintf(ks, sizeof(ks), "s%u", i);
    snprintf(kp, sizeof(kp), "p%u", i);
    snprintf(kt, sizeof(kt), "t%u", i);
    prefs.putString(ks, saved[i].ssid);
    prefs.putString(kp, saved[i].pass);
    prefs.putUInt(kt, saved[i].last_ok);
  }
  prefs.end();
}

static void remember_success(const char* ssid, const char* pass) {
  if (!ssid || !ssid[0]) return;
  int found = -1;
  for (uint8_t i = 0; i < saved_count; i++) {
    if (strcmp(saved[i].ssid, ssid) == 0) {
      found = (int)i;
      break;
    }
  }
  SavedNet entry;
  memset(&entry, 0, sizeof(entry));
  strncpy(entry.ssid, ssid, sizeof(entry.ssid) - 1);
  if (pass && pass[0]) {
    strncpy(entry.pass, pass, sizeof(entry.pass) - 1);
  } else if (found >= 0) {
    strncpy(entry.pass, saved[found].pass, sizeof(entry.pass) - 1);
  }
  entry.last_ok = (uint32_t)(millis() / 1000) + 1;

  if (found >= 0) {
    for (int i = found; i > 0; i--) saved[i] = saved[i - 1];
    saved[0] = entry;
  } else {
    if (saved_count < WIFI_SAVED_MAX) saved_count++;
    for (int i = (int)saved_count - 1; i > 0; i--) saved[i] = saved[i - 1];
    saved[0] = entry;
  }
  persist_saved();
}

static bool forget_ssid(const char* ssid) {
  int found = -1;
  for (uint8_t i = 0; i < saved_count; i++) {
    if (strcmp(saved[i].ssid, ssid) == 0) {
      found = (int)i;
      break;
    }
  }
  if (found < 0) return false;
  for (uint8_t i = (uint8_t)found; i + 1 < saved_count; i++) saved[i] = saved[i + 1];
  saved_count--;
  persist_saved();
  return true;
}

static void seed_from_secrets() {
  if (!WIFI_SSID[0]) return;
  for (uint8_t i = 0; i < saved_count; i++) {
    if (strcmp(saved[i].ssid, WIFI_SSID) == 0) return;
  }
  if (saved_count >= WIFI_SAVED_MAX) return;
  SavedNet& e = saved[saved_count++];
  memset(&e, 0, sizeof(e));
  strncpy(e.ssid, WIFI_SSID, sizeof(e.ssid) - 1);
  strncpy(e.pass, WIFI_PASSWORD, sizeof(e.pass) - 1);
  e.last_ok = 0;
  persist_saved();
}

static bool try_sta_connect(const char* ssid, const char* pass, uint32_t timeout_ms) {
  if (!ssid || !ssid[0]) return false;
  connect_busy = true;
  strncpy(connecting_ssid, ssid, sizeof(connecting_ssid) - 1);
  connecting_ssid[sizeof(connecting_ssid) - 1] = 0;

  Serial.printf("wifi: connecting to %s\n", ssid);
  WiFi.disconnect(false, false);
  delay(50);
  if (pass && pass[0]) {
    WiFi.begin(ssid, pass);
  } else {
    WiFi.begin(ssid);
  }

  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    if (WiFi.status() == WL_CONNECTED) {
      remember_success(ssid, pass ? pass : "");
      Serial.printf("wifi: connected %s ip=%s\n", ssid, WiFi.localIP().toString().c_str());
      connecting_ssid[0] = 0;
      connect_busy = false;
      return true;
    }
    dns.processNextRequest();
    server.handleClient();
    delay(50);
  }

  Serial.printf("wifi: failed %s\n", ssid);
  connecting_ssid[0] = 0;
  connect_busy = false;
  return false;
}

static bool auto_connect_saved(uint32_t per_net_ms) {
  if (WiFi.status() == WL_CONNECTED) return true;
  for (uint8_t i = 0; i < saved_count; i++) {
    if (try_sta_connect(saved[i].ssid, saved[i].pass, per_net_ms)) return true;
  }
  return false;
}

static String json_escape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if (c == '\n') {
      o += "\\n";
    } else if ((uint8_t)c < 0x20) {
      continue;
    } else {
      o += c;
    }
  }
  return o;
}

static void send_json(int code, const String& body) {
  server.send(code, "application/json; charset=utf-8", body);
}

static void handle_status() {
  bool connected = WiFi.status() == WL_CONNECTED;
  String body = "{";
  body += "\"staConnected\":";
  body += connected ? "true" : "false";
  body += ",\"ssid\":\"";
  body += connected ? json_escape(WiFi.SSID()) : "";
  body += "\",\"ip\":\"";
  body += connected ? WiFi.localIP().toString() : "";
  body += "\",\"gateway\":\"";
  body += connected ? WiFi.gatewayIP().toString() : "";
  body += "\",\"subnet\":\"";
  body += connected ? WiFi.subnetMask().toString() : "";
  body += "\",\"rssi\":";
  body += connected ? String(WiFi.RSSI()) : "null";
  body += ",\"mac\":\"";
  body += WiFi.macAddress();
  body += "\",\"apSsid\":\"";
  body += WIFI_AP_SSID;
  body += "\",\"apIp\":\"";
  body += WiFi.softAPIP().toString();
  body += "\",\"connecting\":\"";
  body += json_escape(String(connecting_ssid));
  body += "\",\"savedCount\":";
  body += String(saved_count);
  body += "}";
  send_json(200, body);
}

static void handle_scan() {
  if (connect_busy) {
    send_json(409, "{\"ok\":false,\"error\":\"connecting\"}");
    return;
  }
  Serial.println("wifi: scanning...");
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
  String body = "{\"networks\":[";
  bool first = true;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    if (!first) body += ",";
    first = false;
    body += "{\"ssid\":\"";
    body += json_escape(ssid);
    body += "\",\"rssi\":";
    body += String(WiFi.RSSI(i));
    body += ",\"secure\":";
    body += (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? "true" : "false";
    body += "}";
  }
  body += "]}";
  WiFi.scanDelete();
  send_json(200, body);
}

static void handle_saved() {
  String body = "{\"networks\":[";
  for (uint8_t i = 0; i < saved_count; i++) {
    if (i) body += ",";
    body += "{\"ssid\":\"";
    body += json_escape(String(saved[i].ssid));
    body += "\",\"hasPassword\":";
    body += saved[i].pass[0] ? "true" : "false";
    body += ",\"lastOk\":";
    body += String(saved[i].last_ok);
    body += "}";
  }
  body += "]}";
  send_json(200, body);
}

static String form_arg(const char* key) {
  if (server.hasArg(key)) return server.arg(key);
  return "";
}

static void handle_connect() {
  if (connect_busy) {
    send_json(409, "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }
  String ssid = form_arg("ssid");
  String pass = form_arg("password");
  ssid.trim();
  if (ssid.length() == 0 && server.hasArg("plain")) {
    // minimal JSON: {"ssid":"...","password":"..."}
    String plain = server.arg("plain");
    int si = plain.indexOf("\"ssid\"");
    int pi = plain.indexOf("\"password\"");
    auto extract = [&](int keyPos) -> String {
      if (keyPos < 0) return "";
      int colon = plain.indexOf(':', keyPos);
      int q1 = plain.indexOf('"', colon + 1);
      int q2 = plain.indexOf('"', q1 + 1);
      if (q1 < 0 || q2 < 0) return "";
      return plain.substring(q1 + 1, q2);
    };
    if (ssid.length() == 0) ssid = extract(si);
    if (pass.length() == 0) pass = extract(pi);
  }
  if (ssid.length() == 0) {
    send_json(400, "{\"ok\":false,\"error\":\"ssid required\"}");
    return;
  }

  // Use saved password if omitted
  if (pass.length() == 0) {
    for (uint8_t i = 0; i < saved_count; i++) {
      if (ssid == saved[i].ssid && saved[i].pass[0]) {
        pass = saved[i].pass;
        break;
      }
    }
  }

  bool ok = try_sta_connect(ssid.c_str(), pass.c_str(), 20000);
  if (ok) {
    String body = "{\"ok\":true,\"ssid\":\"";
    body += json_escape(WiFi.SSID());
    body += "\",\"ip\":\"";
    body += WiFi.localIP().toString();
    body += "\"}";
    send_json(200, body);
  } else {
    send_json(200, "{\"ok\":false,\"error\":\"connect failed\"}");
  }
}

static void handle_forget() {
  String ssid = form_arg("ssid");
  if (ssid.length() == 0 && server.hasArg("plain")) {
    String plain = server.arg("plain");
    int si = plain.indexOf("\"ssid\"");
    if (si >= 0) {
      int colon = plain.indexOf(':', si);
      int q1 = plain.indexOf('"', colon + 1);
      int q2 = plain.indexOf('"', q1 + 1);
      if (q1 >= 0 && q2 > q1) ssid = plain.substring(q1 + 1, q2);
    }
  }
  if (ssid.length() == 0) {
    send_json(400, "{\"ok\":false,\"error\":\"ssid required\"}");
    return;
  }
  bool ok = forget_ssid(ssid.c_str());
  send_json(200, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"not found\"}");
}

static void handle_autoconnect() {
  if (connect_busy) {
    send_json(409, "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }
  bool ok = auto_connect_saved(12000);
  if (ok) {
    String body = "{\"ok\":true,\"ssid\":\"";
    body += json_escape(WiFi.SSID());
    body += "\",\"ip\":\"";
    body += WiFi.localIP().toString();
    body += "\"}";
    send_json(200, body);
  } else {
    send_json(200, "{\"ok\":false,\"error\":\"no saved network connected\"}");
  }
}

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"/>
<title>Talkbot Wi-Fi</title>
<style>
:root{--bg:#0f1419;--card:#1a222c;--line:#2c3845;--text:#e8eef4;--muted:#8b9aab;--acc:#3d9cf0;--ok:#3ecf8e;--bad:#f07178}
*{box-sizing:border-box}
body{margin:0;font:15px/1.45 system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--text)}
main{max-width:440px;margin:0 auto;padding:20px 16px 40px}
h1{font-size:1.25rem;margin:0 0 4px;font-weight:650}
.sub{color:var(--muted);font-size:.9rem;margin:0 0 18px}
section{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 14px 12px;margin:0 0 14px}
section h2{font-size:.95rem;margin:0 0 10px;font-weight:600}
.row{display:flex;gap:8px;align-items:center;margin:0 0 8px}
.row:last-child{margin:0}
label{display:block;color:var(--muted);font-size:.8rem;margin:0 0 4px}
input[type=text],input[type=password]{width:100%;padding:10px 12px;border-radius:8px;border:1px solid var(--line);background:#11171e;color:var(--text);font-size:1rem}
button,.btn{appearance:none;border:0;border-radius:8px;padding:10px 14px;font-weight:600;font-size:.9rem;cursor:pointer;background:var(--acc);color:#fff}
button.secondary{background:#2a3542;color:var(--text)}
button.danger{background:#5a3038;color:#ffc9cd}
button:disabled{opacity:.5;cursor:wait}
.kv{display:grid;grid-template-columns:88px 1fr;gap:4px 10px;font-size:.9rem}
.kv b{color:var(--muted);font-weight:500}
.pill{display:inline-block;padding:2px 8px;border-radius:999px;font-size:.75rem;font-weight:600}
.pill.on{background:#1e3d32;color:var(--ok)}
.pill.off{background:#3a2a2c;color:var(--bad)}
.list{list-style:none;margin:0;padding:0}
.list li{display:flex;align-items:center;gap:8px;padding:10px 0;border-top:1px solid var(--line)}
.list li:first-child{border-top:0;padding-top:0}
.list .meta{flex:1;min-width:0}
.list .ssid{font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.list .hint{color:var(--muted);font-size:.8rem}
.msg{min-height:1.2em;font-size:.85rem;color:var(--muted);margin-top:8px}
.msg.err{color:var(--bad)}
.msg.ok{color:var(--ok)}
.tiny{padding:6px 10px;font-size:.8rem}
</style>
</head>
<body>
<main>
  <h1>Talkbot Wi-Fi</h1>
  <p class="sub">AP <b id="apName">Talkbot-Setup</b> · 설정 주소 <span id="apIp">192.168.4.1</span></p>

  <section>
    <h2>연결 상태</h2>
    <div class="row"><span class="pill off" id="staPill">확인 중</span></div>
    <div class="kv" style="margin-top:10px">
      <b>SSID</b><span id="stSsid">—</span>
      <b>로컬 IP</b><span id="stIp">—</span>
      <b>게이트웨이</b><span id="stGw">—</span>
      <b>RSSI</b><span id="stRssi">—</span>
      <b>MAC</b><span id="stMac">—</span>
    </div>
  </section>

  <section>
    <h2>네트워크 검색</h2>
    <div class="row">
      <button type="button" id="btnScan" class="secondary">SSID 검색</button>
    </div>
    <ul class="list" id="scanList"></ul>
    <div class="msg" id="scanMsg"></div>
  </section>

  <section>
    <h2>연결</h2>
    <label for="ssid">SSID</label>
    <input id="ssid" type="text" autocomplete="off" placeholder="네트워크 이름"/>
    <label for="pass" style="margin-top:10px">비밀번호</label>
    <input id="pass" type="password" autocomplete="off" placeholder="개방형이면 비워 두기"/>
    <div class="row" style="margin-top:12px">
      <button type="button" id="btnConnect">연결</button>
    </div>
    <div class="msg" id="connMsg"></div>
  </section>

  <section>
    <h2>최근 성공 · 자동접속</h2>
    <div class="row">
      <button type="button" id="btnAuto" class="secondary">저장된 네트워크로 자동접속</button>
    </div>
    <ul class="list" id="savedList"></ul>
    <div class="msg" id="savedMsg"></div>
  </section>
</main>
<script>
const $=id=>document.getElementById(id);
function setMsg(el,text,cls){el.textContent=text||'';el.className='msg'+(cls?' '+cls:'');}
async function api(path,opts){
  const r=await fetch(path,opts);
  const t=await r.text();
  try{return JSON.parse(t);}catch(e){throw new Error(t||r.statusText);}
}
function fillStatus(s){
  $('apName').textContent=s.apSsid||'Talkbot-Setup';
  $('apIp').textContent=s.apIp||'192.168.4.1';
  const on=!!s.staConnected;
  $('staPill').textContent=on?'STA 연결됨':(s.connecting?'연결 중…':'미연결');
  $('staPill').className='pill '+(on?'on':'off');
  $('stSsid').textContent=s.ssid||'—';
  $('stIp').textContent=s.ip||'—';
  $('stGw').textContent=s.gateway||'—';
  $('stRssi').textContent=s.rssi==null?'—':(s.rssi+' dBm');
  $('stMac').textContent=s.mac||'—';
}
async function refreshStatus(){
  try{fillStatus(await api('/api/status'));}catch(e){}
}
async function refreshSaved(){
  const box=$('savedList');
  box.innerHTML='';
  try{
    const d=await api('/api/saved');
    const nets=d.networks||[];
    if(!nets.length){setMsg($('savedMsg'),'저장된 네트워크 없음');return;}
    setMsg($('savedMsg'),'');
    nets.forEach((n,i)=>{
      const li=document.createElement('li');
      li.innerHTML=`<div class="meta"><div class="ssid">${n.ssid}</div><div class="hint">${i===0?'최근 성공 · ':''}${n.hasPassword?'비밀번호 저장됨':'개방형'}</div></div>`;
      const b1=document.createElement('button');
      b1.className='tiny';b1.textContent='접속';
      b1.onclick=()=>connectTo(n.ssid,'');
      const b2=document.createElement('button');
      b2.className='tiny danger';b2.textContent='삭제';
      b2.onclick=async()=>{
        await api('/api/forget',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(n.ssid)});
        refreshSaved();refreshStatus();
      };
      li.appendChild(b1);li.appendChild(b2);box.appendChild(li);
    });
  }catch(e){setMsg($('savedMsg'),String(e),'err');}
}
async function doScan(){
  const btn=$('btnScan');btn.disabled=true;
  setMsg($('scanMsg'),'검색 중…');
  $('scanList').innerHTML='';
  try{
    const d=await api('/api/scan');
    const nets=d.networks||[];
    nets.sort((a,b)=>b.rssi-a.rssi);
    if(!nets.length){setMsg($('scanMsg'),'검색된 SSID 없음');return;}
    setMsg($('scanMsg'),nets.length+'개 발견');
    nets.forEach(n=>{
      const li=document.createElement('li');
      li.innerHTML=`<div class="meta"><div class="ssid">${n.ssid}</div><div class="hint">${n.rssi} dBm · ${n.secure?'보안':'개방'}</div></div>`;
      const b=document.createElement('button');
      b.className='tiny secondary';b.textContent='선택';
      b.onclick=()=>{$('ssid').value=n.ssid;$('pass').focus();};
      li.appendChild(b);$('scanList').appendChild(li);
    });
  }catch(e){setMsg($('scanMsg'),String(e),'err');}
  finally{btn.disabled=false;}
}
async function connectTo(ssid,password){
  $('ssid').value=ssid;
  if(password!==undefined&&password!==null&&password!=='')$('pass').value=password;
  const btn=$('btnConnect');btn.disabled=true;$('btnAuto').disabled=true;
  setMsg($('connMsg'),ssid+' 연결 중…');
  try{
    const body='ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent($('pass').value||password||'');
    const d=await api('/api/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
    if(d.ok){setMsg($('connMsg'),'연결됨 · '+d.ip,'ok');}
    else{setMsg($('connMsg'),d.error||'실패','err');}
  }catch(e){setMsg($('connMsg'),String(e),'err');}
  finally{btn.disabled=false;$('btnAuto').disabled=false;refreshStatus();refreshSaved();}
}
$('btnScan').onclick=doScan;
$('btnConnect').onclick=()=>connectTo($('ssid').value.trim(),$('pass').value);
$('btnAuto').onclick=async()=>{
  $('btnAuto').disabled=true;setMsg($('savedMsg'),'자동접속 시도 중…');
  try{
    const d=await api('/api/autoconnect',{method:'POST'});
    if(d.ok){setMsg($('savedMsg'),'연결됨 · '+d.ssid+' / '+d.ip,'ok');}
    else{setMsg($('savedMsg'),d.error||'실패','err');}
  }catch(e){setMsg($('savedMsg'),String(e),'err');}
  finally{$('btnAuto').disabled=false;refreshStatus();refreshSaved();}
};
refreshStatus();refreshSaved();
setInterval(refreshStatus,4000);
</script>
</body>
</html>
)HTML";

static void handle_root() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

static void handle_captive() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

static void handle_not_found() {
  if (WiFi.softAPgetStationNum() > 0) {
    handle_captive();
    return;
  }
  server.send(404, "text/plain", "Not found");
}

void wifi_portal_begin() {
  load_saved();
  seed_from_secrets();

  WiFi.persistent(false);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);

  bool ap_ok = WIFI_AP_PASS[0]
                   ? WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS)
                   : WiFi.softAP(WIFI_AP_SSID);
  Serial.printf("wifi: AP %s %s ip=%s\n", WIFI_AP_SSID, ap_ok ? "up" : "FAIL",
                WiFi.softAPIP().toString().c_str());

  dns.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, handle_root);
  server.on("/api/status", HTTP_GET, handle_status);
  server.on("/api/scan", HTTP_GET, handle_scan);
  server.on("/api/saved", HTTP_GET, handle_saved);
  server.on("/api/connect", HTTP_POST, handle_connect);
  server.on("/api/forget", HTTP_POST, handle_forget);
  server.on("/api/autoconnect", HTTP_POST, handle_autoconnect);
  server.on("/generate_204", HTTP_GET, handle_captive);
  server.on("/hotspot-detect.html", HTTP_GET, handle_captive);
  server.on("/connecttest.txt", HTTP_GET, handle_captive);
  server.on("/ncsi.txt", HTTP_GET, handle_captive);
  server.onNotFound(handle_not_found);
  server.begin();
  portal_ready = true;

  Serial.printf("wifi: portal http://%s/  saved=%u\n", WiFi.softAPIP().toString().c_str(),
                (unsigned)saved_count);

  if (saved_count > 0) {
    Serial.println("wifi: auto-connect from saved list...");
    auto_connect_saved(15000);
  }
}

void wifi_portal_loop() {
  if (!portal_ready) return;
  dns.processNextRequest();
  server.handleClient();
}

bool wifi_portal_connected() { return WiFi.status() == WL_CONNECTED; }

bool wifi_portal_ensure(bool blocking) {
  if (WiFi.status() == WL_CONNECTED) return true;

  uint32_t now = millis();
  if (!blocking && now - last_wifi_attempt_ms < WIFI_RETRY_INTERVAL_MS) return false;
  last_wifi_attempt_ms = now;

  if (!blocking) {
    if (saved_count == 0 || connect_busy) return false;
    // Non-blocking: kick off reconnect to most recent
    connect_busy = true;
    strncpy(connecting_ssid, saved[0].ssid, sizeof(connecting_ssid) - 1);
    Serial.printf("wifi: reconnect %s\n", saved[0].ssid);
    if (saved[0].pass[0]) {
      WiFi.begin(saved[0].ssid, saved[0].pass);
    } else {
      WiFi.begin(saved[0].ssid);
    }
    connect_busy = false;
    connecting_ssid[0] = 0;
    return false;
  }

  if (auto_connect_saved(12000)) return true;
  Serial.println("ERR: WiFi failed — open Talkbot-Setup portal");
  return false;
}

void wifi_portal_maintain() {
  wifi_portal_loop();
  if (WiFi.status() == WL_CONNECTED) return;
  wifi_portal_ensure(false);
}
