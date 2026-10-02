const TOKEN_KEY = "talkbot_session";
const params = new URLSearchParams(location.search);
const qrDeviceId = (params.get("d") || "").trim();
const qrPairSecret = (params.get("t") || "").trim();
const fromQr = Boolean(qrDeviceId && qrPairSecret);

const CORE_CATEGORIES = ["play", "day", "meal", "emo", "story", "sleep"];

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

function show(el, text, ok) {
  if (!el) return;
  el.hidden = false;
  el.textContent = text;
  el.className = "msg " + (ok ? "ok" : "err");
}

const authView = document.getElementById("authView");
const appView = document.getElementById("appView");
const qrBanner = document.getElementById("qrBanner");
let voices = [];
let schema = null;
let selectedDevice = null;
let profileState = null;

function applyQrChrome() {
  if (!fromQr) {
    if (qrBanner) qrBanner.hidden = true;
    return;
  }
  document.title = "리듬이 제어판";
  if (qrBanner) {
    qrBanner.hidden = false;
    qrBanner.querySelector("[data-device]").textContent = qrDeviceId;
  }
  const lede = document.getElementById("lede");
  if (lede) {
    lede.textContent = "인형 QR로 열림 · 로그인하면 바로 이 인형 설정으로 연결됩니다.";
  }
}

async function pairFromQr() {
  if (!fromQr || !token()) return null;
  const data = await api("/api/devices/pair", {
    method: "POST",
    body: JSON.stringify({ device_id: qrDeviceId, pair_secret: qrPairSecret }),
  });
  return data.device;
}

function renderChipGroup(containerId, options, selectedId, multi = false) {
  const el = document.getElementById(containerId);
  if (!el || !options) return;
  el.innerHTML = "";
  for (const opt of options) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "chip";
    btn.textContent = opt.label;
    btn.dataset.id = opt.id;
    const pressed = multi
      ? (selectedId || []).includes(opt.id)
      : selectedId === opt.id;
    btn.setAttribute("aria-pressed", pressed ? "true" : "false");
    btn.onclick = () => {
      if (multi) {
        const set = new Set(profileState.categories || []);
        if (set.has(opt.id)) {
          if (opt.id !== "play") set.delete(opt.id);
        } else {
          set.add(opt.id);
        }
        profileState.categories = [...set];
        renderChipGroup(containerId, options, profileState.categories, true);
      } else {
        const map = {
          kindChips: "character_kind",
          ageChips: "age_band",
          purposeChips: "purpose",
          toneChips: "tone",
          turnChips: "turn_style",
          questionChips: "question_rate",
        };
        const key = map[containerId];
        if (key) profileState[key] = opt.id;
        renderChipGroup(containerId, options, opt.id, false);
        if (key === "character_kind" || key === "character_name") syncBootHint();
      }
      updatePersonaPreview();
    };
    el.appendChild(btn);
  }
}

function syncBootHint() {
  const boot = document.getElementById("bootPhrase");
  if (!boot || !profileState) return;
  const name = (document.getElementById("characterName").value || profileState.character_name || "디노").trim();
  // Only auto-fill if empty or still a default-style greeting.
  const cur = boot.value.trim();
  if (!cur || /^안녕,?\s*난\s+.+\s*야!?/.test(cur) || cur.includes("디노")) {
    boot.value = `안녕, 난 ${name || "디노"}야! 같이 놀자.`.slice(0, 90);
  }
}

function readProfileFromForm() {
  const name = (document.getElementById("characterName").value || "디노").trim().slice(0, 20);
  return {
    v: schema?.version || 1,
    character_name: name || "디노",
    character_kind: profileState.character_kind,
    age_band: profileState.age_band,
    purpose: profileState.purpose,
    tone: profileState.tone,
    turn_style: profileState.turn_style,
    question_rate: profileState.question_rate,
    categories: [...(profileState.categories || [])],
    custom_notes: (document.getElementById("customNotes").value || "").trim().slice(0, 120),
  };
}

function compilePreview(p) {
  // Lightweight mirror of server compile for live preview (Korean).
  const kind = (schema.character_kinds || []).find((x) => x.id === p.character_kind);
  const age = (schema.age_bands || []).find((x) => x.id === p.age_band);
  const purpose = (schema.purposes || []).find((x) => x.id === p.purpose);
  const tone = (schema.tones || []).find((x) => x.id === p.tone);
  const turn = (schema.turn_styles || []).find((x) => x.id === p.turn_style);
  const q = (schema.question_rates || []).find((x) => x.id === p.question_rate);
  const cats = schema.categories || [];
  const on = cats.filter((c) => (p.categories || []).includes(c.id)).map((c) => c.label);
  const off = cats.filter((c) => !(p.categories || []).includes(c.id)).map((c) => c.label);
  const parts = [
    `이름은 ${p.character_name}. ${kind?.label || ""} 인형.`,
    `목적: ${purpose?.label || ""}.`,
    `말투: ${tone?.label || ""} · ${age?.label || ""}.`,
    `${turn?.label || ""}. ${q?.label || ""}.`,
  ];
  if (on.length) parts.push(`환영: ${on.join("·")}.`);
  if (off.length) parts.push(`끄기: ${off.join("·")}.`);
  if (p.custom_notes) parts.push(p.custom_notes);
  return parts.join(" ");
}

function updatePersonaPreview() {
  if (!schema || !profileState) return;
  const p = readProfileFromForm();
  const el = document.getElementById("personaPreview");
  if (el) el.textContent = compilePreview(p);
}

function applyProfileToForm(profile) {
  profileState = {
    character_kind: profile.character_kind,
    age_band: profile.age_band,
    purpose: profile.purpose,
    tone: profile.tone,
    turn_style: profile.turn_style,
    question_rate: profile.question_rate,
    categories: [...(profile.categories || schema.defaults.categories)],
  };
  document.getElementById("characterName").value = profile.character_name || "디노";
  document.getElementById("customNotes").value = profile.custom_notes || "";
  renderChipGroup("kindChips", schema.character_kinds, profileState.character_kind);
  renderChipGroup("ageChips", schema.age_bands, profileState.age_band);
  renderChipGroup("purposeChips", schema.purposes, profileState.purpose);
  renderChipGroup("toneChips", schema.tones, profileState.tone);
  renderChipGroup("turnChips", schema.turn_styles, profileState.turn_style);
  renderChipGroup("questionChips", schema.question_rates, profileState.question_rate);
  renderChipGroup("categoryChips", schema.categories, profileState.categories, true);
  updatePersonaPreview();
}

async function loadSchema() {
  schema = await api("/api/chat-profile-schema");
}

async function refreshMe() {
  applyQrChrome();
  if (!token()) {
    authView.hidden = false;
    appView.hidden = true;
    return;
  }
  try {
    const me = await api("/api/me");
    authView.hidden = true;
    appView.hidden = false;
    document.getElementById("meEmail").textContent = me.email;
    document.getElementById("keyStatus").textContent =
      `Groq ${me.has_groq_key ? "저장됨" : "없음"} · Google ${me.has_google_key ? "저장됨" : "없음"}`;

    let paired = null;
    if (fromQr) {
      try {
        paired = await pairFromQr();
        const status = document.getElementById("qrStatus");
        if (status) {
          status.hidden = false;
          status.textContent = "이 인형이 계정에 연결되었습니다.";
          status.className = "msg ok";
        }
      } catch (e) {
        const status = document.getElementById("qrStatus");
        if (status) {
          status.hidden = false;
          status.textContent = e.message;
          status.className = "msg err";
        }
      }
    }

    await loadSchema();
    await loadVoices();
    await loadDevices();
    if (paired) openEditor(paired);
    else if (fromQr) {
      const list = await api("/api/devices");
      const hit = (list.devices || []).find((d) => d.device_id === qrDeviceId);
      if (hit) openEditor(hit);
    }

    if (fromQr && token() && history.replaceState) {
      history.replaceState({}, "", `/?d=${encodeURIComponent(qrDeviceId)}`);
    }
  } catch {
    setToken("");
    authView.hidden = false;
    appView.hidden = true;
  }
}

async function loadVoices() {
  const data = await api("/api/voices");
  voices = data.voices || [];
  const sel = document.getElementById("voiceSelect");
  sel.innerHTML = "";
  for (const v of voices) {
    const opt = document.createElement("option");
    opt.value = v.id;
    opt.textContent = v.label;
    sel.appendChild(opt);
  }
}

async function loadDevices() {
  const data = await api("/api/devices");
  const list = document.getElementById("deviceList");
  list.innerHTML = "";
  if (!data.devices.length) {
    list.innerHTML = `<p class="hint">연결된 인형이 없습니다. 인형 바닥 QR을 다시 찍어 주세요.</p>`;
    return;
  }
  for (const d of data.devices) {
    const row = document.createElement("div");
    row.className = "card";
    const mark = fromQr && d.device_id === qrDeviceId ? " · 방금 QR" : "";
    const name = d.chat_profile?.character_name || "";
    const label = name ? `${name} · ${d.voice}` : d.voice;
    row.innerHTML = `<div><strong>${d.device_id}</strong><span>${label} · rev ${d.config_rev}${mark}</span></div>`;
    const btn = document.createElement("button");
    btn.type = "button";
    btn.textContent = "설정";
    btn.onclick = () => openEditor(d);
    row.appendChild(btn);
    list.appendChild(row);
  }
}

function openEditor(d) {
  selectedDevice = d.device_id;
  document.getElementById("deviceEditor").hidden = false;
  document.getElementById("editTitle").textContent = d.device_id;
  document.getElementById("voiceSelect").value = d.voice;
  document.getElementById("bootPhrase").value = d.boot_phrase || "";
  document.getElementById("editMeta").textContent = `config_rev ${d.config_rev}`;
  document.getElementById("editMsg").hidden = true;
  const previewMsg = document.getElementById("previewMsg");
  if (previewMsg) previewMsg.hidden = true;

  const profile = d.chat_profile || schema?.defaults || {};
  applyProfileToForm(profile);

  document.getElementById("deviceEditor").scrollIntoView({ behavior: "smooth", block: "start" });
}

const VOICE_SAMPLE = {
  "ko-KR-Chirp3-HD-Kore": "/samples/kore.wav",
  "ko-KR-Chirp3-HD-Leda": "/samples/leda.wav",
  "ko-KR-Chirp3-HD-Puck": "/samples/puck.wav",
  "ko-KR-Chirp3-HD-Zephyr": "/samples/zephyr.wav",
};

let previewAudio = null;

async function previewSelectedVoice() {
  const voice = document.getElementById("voiceSelect").value;
  const src = VOICE_SAMPLE[voice];
  const btn = document.getElementById("previewVoiceBtn");
  const msg = document.getElementById("previewMsg");
  if (!src) {
    if (msg) {
      msg.hidden = false;
      msg.textContent = "이 목소리 샘플이 없습니다.";
    }
    return;
  }
  try {
    if (previewAudio) {
      previewAudio.pause();
      previewAudio = null;
    }
    btn.setAttribute("aria-busy", "true");
    btn.textContent = "재생 중…";
    previewAudio = new Audio(src);
    previewAudio.onended = () => {
      btn.removeAttribute("aria-busy");
      btn.textContent = "미리 듣기";
    };
    previewAudio.onerror = () => {
      btn.removeAttribute("aria-busy");
      btn.textContent = "미리 듣기";
      if (msg) {
        msg.hidden = false;
        msg.textContent = "샘플 재생 실패";
      }
    };
    await previewAudio.play();
    if (msg) msg.hidden = true;
  } catch (e) {
    btn.removeAttribute("aria-busy");
    btn.textContent = "미리 듣기";
    if (msg) {
      msg.hidden = false;
      msg.textContent = e.message || "재생할 수 없습니다";
    }
  }
}

document.getElementById("previewVoiceBtn").onclick = () => {
  previewSelectedVoice();
};

document.getElementById("characterName").addEventListener("input", () => {
  syncBootHint();
  updatePersonaPreview();
});
document.getElementById("customNotes").addEventListener("input", updatePersonaPreview);

document.getElementById("catsAllBtn").onclick = () => {
  if (!schema) return;
  profileState.categories = schema.categories.map((c) => c.id);
  renderChipGroup("categoryChips", schema.categories, profileState.categories, true);
  updatePersonaPreview();
};

document.getElementById("catsCoreBtn").onclick = () => {
  if (!schema) return;
  profileState.categories = CORE_CATEGORIES.filter((id) =>
    schema.categories.some((c) => c.id === id)
  );
  renderChipGroup("categoryChips", schema.categories, profileState.categories, true);
  updatePersonaPreview();
};

async function afterAuth(msgText) {
  const msg = document.getElementById("authMsg");
  show(msg, msgText, true);
  await refreshMe();
}

document.getElementById("loginBtn").onclick = async () => {
  const msg = document.getElementById("authMsg");
  try {
    const data = await api("/api/auth/login", {
      method: "POST",
      body: JSON.stringify({
        email: document.getElementById("email").value,
        password: document.getElementById("password").value,
      }),
    });
    setToken(data.token);
    await afterAuth(fromQr ? "로그인 · 인형 연결 중…" : "로그인 완료");
  } catch (e) {
    show(msg, e.message, false);
  }
};

document.getElementById("registerBtn").onclick = async () => {
  const msg = document.getElementById("authMsg");
  try {
    const data = await api("/api/auth/register", {
      method: "POST",
      body: JSON.stringify({
        email: document.getElementById("email").value,
        password: document.getElementById("password").value,
      }),
    });
    setToken(data.token);
    await afterAuth(fromQr ? "가입 · 인형 연결 중…" : "가입 완료");
  } catch (e) {
    show(msg, e.message, false);
  }
};

document.getElementById("logoutBtn").onclick = () => {
  setToken("");
  refreshMe();
};

document.getElementById("saveKeysBtn").onclick = async () => {
  const body = {};
  const g = document.getElementById("groqKey").value.trim();
  const o = document.getElementById("googleKey").value.trim();
  if (g) body.groq_api_key = g;
  if (o) body.google_api_key = o;
  try {
    const data = await api("/api/account/keys", { method: "PUT", body: JSON.stringify(body) });
    document.getElementById("groqKey").value = "";
    document.getElementById("googleKey").value = "";
    document.getElementById("keyStatus").textContent =
      `Groq ${data.has_groq_key ? "저장됨" : "없음"} · Google ${data.has_google_key ? "저장됨" : "없음"}`;
  } catch (e) {
    alert(e.message);
  }
};

document.getElementById("saveDeviceBtn").onclick = async () => {
  if (!selectedDevice) return;
  const msg = document.getElementById("editMsg");
  try {
    const chat_profile = readProfileFromForm();
    const data = await api(`/api/devices/${encodeURIComponent(selectedDevice)}/config`, {
      method: "PUT",
      body: JSON.stringify({
        voice: document.getElementById("voiceSelect").value,
        boot_phrase: document.getElementById("bootPhrase").value,
        chat_profile,
      }),
    });
    document.getElementById("editMeta").textContent = `config_rev ${data.config_rev} — 인형이 다음 sync 때 반영`;
    if (data.chat_profile) applyProfileToForm(data.chat_profile);
    const preview = document.getElementById("personaPreview");
    if (preview && data.persona_extra) preview.textContent = data.persona_extra;
    show(msg, "저장됨 · 인형에 적용 대기", true);
    await loadDevices();
  } catch (e) {
    show(msg, e.message, false);
  }
};

refreshMe();
