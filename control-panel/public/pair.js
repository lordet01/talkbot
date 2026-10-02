const TOKEN_KEY = "talkbot_session";
const params = new URLSearchParams(location.search);
const deviceId = params.get("d") || "";
const pairSecret = params.get("t") || "";

document.getElementById("deviceId").value = deviceId;
document.getElementById("pairSecret").value = pairSecret;

function token() {
  return localStorage.getItem(TOKEN_KEY) || "";
}
function setToken(t) {
  if (t) localStorage.setItem(TOKEN_KEY, t);
  else localStorage.removeItem(TOKEN_KEY);
}

async function api(path, opts = {}) {
  const headers = { "Content-Type": "application/json", ...(opts.headers || {}) };
  const t = token();
  if (t) headers.Authorization = `Bearer ${t}`;
  const res = await fetch(path, { ...opts, headers });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
  return data;
}

function show(text, ok) {
  const el = document.getElementById("msg");
  el.hidden = false;
  el.textContent = text;
  el.className = "msg " + (ok ? "ok" : "err");
}

function syncAuthUi() {
  document.getElementById("needAuth").hidden = Boolean(token());
}

async function doAuth(register) {
  const path = register ? "/api/auth/register" : "/api/auth/login";
  const data = await api(path, {
    method: "POST",
    body: JSON.stringify({
      email: document.getElementById("email").value,
      password: document.getElementById("password").value,
    }),
  });
  setToken(data.token);
  syncAuthUi();
}

document.getElementById("loginBtn").onclick = async () => {
  try {
    await doAuth(false);
    show("로그인됨", true);
  } catch (e) {
    show(e.message, false);
  }
};
document.getElementById("registerBtn").onclick = async () => {
  try {
    await doAuth(true);
    show("가입됨", true);
  } catch (e) {
    show(e.message, false);
  }
};

document.getElementById("pairBtn").onclick = async () => {
  if (!deviceId || !pairSecret) {
    show("QR 파라미터가 없습니다 (d, t)", false);
    return;
  }
  if (!token()) {
    show("먼저 로그인하세요", false);
    syncAuthUi();
    return;
  }
  try {
    await api("/api/devices/pair", {
      method: "POST",
      body: JSON.stringify({ device_id: deviceId, pair_secret: pairSecret }),
    });
    show("연결 완료. 인형이 곧 설정을 받아갑니다.", true);
  } catch (e) {
    show(e.message, false);
  }
};

syncAuthUi();
