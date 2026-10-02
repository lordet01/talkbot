/**
 * Local control-panel API + static PWA (no Cloudflare account required).
 * Run: cd control-panel && npm start
 * Default: http://0.0.0.0:8787
 */
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import {
  catalogPayload,
  compilePersonaExtra,
  defaultBootPhrase,
  normalizeProfile,
  parseProfileJson,
} from "./src/chat-profile.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PORT = Number(process.env.PORT || 8787);
const HOST = process.env.HOST || "0.0.0.0";
const DATA_DIR = path.join(__dirname, "data");
const DB_PATH = path.join(DATA_DIR, "db.json");
const PUBLIC_DIR = path.join(__dirname, "public");

const VOICES = [
  { id: "ko-KR-Chirp3-HD-Kore", label: "Kore (따뜻함)" },
  { id: "ko-KR-Chirp3-HD-Leda", label: "Leda (밝음)" },
  { id: "ko-KR-Chirp3-HD-Puck", label: "Puck (활발)" },
  { id: "ko-KR-Chirp3-HD-Zephyr", label: "Zephyr (부드러움)" },
];

function loadDb() {
  if (!fs.existsSync(DATA_DIR)) fs.mkdirSync(DATA_DIR, { recursive: true });
  if (!fs.existsSync(DB_PATH)) {
    const empty = { accounts: {}, sessions: {}, devices: {} };
    fs.writeFileSync(DB_PATH, JSON.stringify(empty, null, 2));
    return empty;
  }
  return JSON.parse(fs.readFileSync(DB_PATH, "utf8"));
}

function saveDb(db) {
  fs.writeFileSync(DB_PATH, JSON.stringify(db, null, 2));
}

function hashPassword(password, salt = crypto.randomBytes(16).toString("hex")) {
  const hash = crypto.scryptSync(password, salt, 32).toString("hex");
  return `${salt}:${hash}`;
}

function verifyPassword(password, stored) {
  const [salt, hash] = stored.split(":");
  const check = crypto.scryptSync(password, salt, 32).toString("hex");
  return crypto.timingSafeEqual(Buffer.from(hash, "hex"), Buffer.from(check, "hex"));
}

function token() {
  return crypto.randomBytes(24).toString("hex");
}

function json(res, status, body) {
  const raw = JSON.stringify(body);
  res.writeHead(status, {
    "Content-Type": "application/json; charset=utf-8",
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Headers": "Content-Type, Authorization",
    "Access-Control-Allow-Methods": "GET,POST,PUT,OPTIONS",
  });
  res.end(raw);
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => {
      const raw = Buffer.concat(chunks).toString("utf8");
      if (!raw) return resolve({});
      try {
        resolve(JSON.parse(raw));
      } catch (e) {
        reject(e);
      }
    });
    req.on("error", reject);
  });
}

function bearer(req) {
  const h = req.headers.authorization || "";
  const m = /^Bearer\s+(.+)$/i.exec(h);
  return m ? m[1].trim() : "";
}

function accountFromSession(db, tok) {
  const s = db.sessions[tok];
  if (!s) return null;
  return db.accounts[s.account_id] || null;
}

function deviceConfigPayload(dev, account) {
  return {
    device_id: dev.device_id,
    device_token: dev.device_token || "",
    voice: dev.voice,
    persona_extra: dev.persona_extra || "",
    boot_phrase: dev.boot_phrase,
    config_rev: dev.config_rev,
    paired: Boolean(dev.account_id && dev.device_token),
    groq_api_key: account?.groq_api_key || "",
    google_api_key: account?.google_api_key || "",
  };
}

function mime(filePath) {
  if (filePath.endsWith(".html")) return "text/html; charset=utf-8";
  if (filePath.endsWith(".js")) return "text/javascript; charset=utf-8";
  if (filePath.endsWith(".css")) return "text/css; charset=utf-8";
  if (filePath.endsWith(".webmanifest")) return "application/manifest+json";
  if (filePath.endsWith(".svg")) return "image/svg+xml";
  if (filePath.endsWith(".png")) return "image/png";
  return "application/octet-stream";
}

function serveStatic(req, res, urlPath) {
  let rel = decodeURIComponent(urlPath.split("?")[0]);
  if (rel === "/") rel = "/index.html";
  const filePath = path.normalize(path.join(PUBLIC_DIR, rel));
  if (!filePath.startsWith(PUBLIC_DIR)) {
    res.writeHead(403);
    return res.end("forbidden");
  }
  if (!fs.existsSync(filePath) || fs.statSync(filePath).isDirectory()) {
    res.writeHead(404);
    return res.end("not found");
  }
  res.writeHead(200, { "Content-Type": mime(filePath) });
  fs.createReadStream(filePath).pipe(res);
}

async function handleApi(req, res, url) {
  const db = loadDb();
  const method = req.method || "GET";

  if (method === "OPTIONS") {
    return json(res, 204, {});
  }

  if (method === "GET" && url.pathname === "/api/health") {
    return json(res, 200, { ok: true, voices: VOICES });
  }

  if (method === "GET" && url.pathname === "/api/voices") {
    return json(res, 200, { voices: VOICES });
  }

  if (method === "GET" && url.pathname === "/api/chat-profile-schema") {
    return json(res, 200, catalogPayload());
  }

  if (method === "POST" && url.pathname === "/api/auth/register") {
    const body = await readBody(req);
    const email = String(body.email || "")
      .trim()
      .toLowerCase();
    const password = String(body.password || "");
    if (!email || password.length < 6) {
      return json(res, 400, { error: "email and password(>=6) required" });
    }
    if (Object.values(db.accounts).some((a) => a.email === email)) {
      return json(res, 409, { error: "email already registered" });
    }
    const id = token();
    db.accounts[id] = {
      id,
      email,
      password_hash: hashPassword(password),
      groq_api_key: "",
      google_api_key: "",
      created_at: Date.now(),
    };
    const session = token();
    db.sessions[session] = { token: session, account_id: id, created_at: Date.now() };
    saveDb(db);
    return json(res, 200, { token: session, email });
  }

  if (method === "POST" && url.pathname === "/api/auth/login") {
    const body = await readBody(req);
    const email = String(body.email || "")
      .trim()
      .toLowerCase();
    const password = String(body.password || "");
    const account = Object.values(db.accounts).find((a) => a.email === email);
    if (!account || !verifyPassword(password, account.password_hash)) {
      return json(res, 401, { error: "invalid credentials" });
    }
    const session = token();
    db.sessions[session] = {
      token: session,
      account_id: account.id,
      created_at: Date.now(),
    };
    saveDb(db);
    return json(res, 200, { token: session, email: account.email });
  }

  if (method === "GET" && url.pathname === "/api/me") {
    const account = accountFromSession(db, bearer(req));
    if (!account) return json(res, 401, { error: "unauthorized" });
    return json(res, 200, {
      email: account.email,
      has_groq_key: Boolean(account.groq_api_key),
      has_google_key: Boolean(account.google_api_key),
    });
  }

  if (method === "PUT" && url.pathname === "/api/account/keys") {
    const account = accountFromSession(db, bearer(req));
    if (!account) return json(res, 401, { error: "unauthorized" });
    const body = await readBody(req);
    if (typeof body.groq_api_key === "string") account.groq_api_key = body.groq_api_key.trim();
    if (typeof body.google_api_key === "string")
      account.google_api_key = body.google_api_key.trim();
    db.accounts[account.id] = account;
    saveDb(db);
    return json(res, 200, {
      ok: true,
      has_groq_key: Boolean(account.groq_api_key),
      has_google_key: Boolean(account.google_api_key),
    });
  }

  if (method === "POST" && url.pathname === "/api/devices/pair") {
    const account = accountFromSession(db, bearer(req));
    if (!account) return json(res, 401, { error: "unauthorized" });
    const body = await readBody(req);
    const device_id = String(body.device_id || "").trim();
    const pair_secret = String(body.pair_secret || "").trim();
    if (!device_id || !pair_secret) {
      return json(res, 400, { error: "device_id and pair_secret required" });
    }
    let dev = db.devices[device_id];
    if (!dev) {
      dev = {
        device_id,
        pair_secret,
        device_token: token(),
        account_id: account.id,
        voice: "ko-KR-Chirp3-HD-Kore",
        persona_extra: "",
        boot_phrase: "안녕, 난 디노야! 같이 놀자.",
        chat_profile: "",
        config_rev: 1,
        paired_at: Date.now(),
        created_at: Date.now(),
      };
    } else {
      if (dev.pair_secret !== pair_secret) {
        return json(res, 403, { error: "invalid pair secret" });
      }
      if (dev.account_id && dev.account_id !== account.id) {
        return json(res, 409, { error: "device already paired to another account" });
      }
      dev.account_id = account.id;
      if (!dev.device_token) dev.device_token = token();
      dev.paired_at = Date.now();
    }
    db.devices[device_id] = dev;
    saveDb(db);
    return json(res, 200, { ok: true, device: publicDevice(dev) });
  }

  if (method === "GET" && url.pathname === "/api/devices") {
    const account = accountFromSession(db, bearer(req));
    if (!account) return json(res, 401, { error: "unauthorized" });
    const list = Object.values(db.devices)
      .filter((d) => d.account_id === account.id)
      .map(publicDevice);
    return json(res, 200, { devices: list });
  }

  const configMatch = url.pathname.match(/^\/api\/devices\/([^/]+)\/config$/);
  if (configMatch && method === "GET") {
    const account = accountFromSession(db, bearer(req));
    if (!account) return json(res, 401, { error: "unauthorized" });
    const device_id = decodeURIComponent(configMatch[1]);
    const dev = db.devices[device_id];
    if (!dev || dev.account_id !== account.id) return json(res, 404, { error: "not found" });
    return json(res, 200, publicDevice(dev));
  }

  if (configMatch && method === "PUT") {
    const account = accountFromSession(db, bearer(req));
    if (!account) return json(res, 401, { error: "unauthorized" });
    const device_id = decodeURIComponent(configMatch[1]);
    const dev = db.devices[device_id];
    if (!dev || dev.account_id !== account.id) return json(res, 404, { error: "not found" });
    const body = await readBody(req);
    if (typeof body.voice === "string" && body.voice) dev.voice = body.voice.trim();

    let profile = parseProfileJson(dev.chat_profile || "");
    if (body.chat_profile && typeof body.chat_profile === "object") {
      profile = normalizeProfile(body.chat_profile);
    }
    let persona_extra = compilePersonaExtra(profile, 360);
    if (typeof body.persona_extra === "string" && body.persona_extra.trim() && !body.chat_profile) {
      persona_extra = body.persona_extra.trim().slice(0, 360);
    }
    dev.persona_extra = persona_extra;
    dev.chat_profile = JSON.stringify(profile);

    if (typeof body.boot_phrase === "string" && body.boot_phrase.trim()) {
      dev.boot_phrase = body.boot_phrase.trim().slice(0, 90);
    } else if (body.chat_profile && (!dev.boot_phrase || !String(dev.boot_phrase).trim())) {
      dev.boot_phrase = defaultBootPhrase(profile);
    }

    dev.config_rev = (dev.config_rev || 0) + 1;
    db.devices[device_id] = dev;
    saveDb(db);
    return json(res, 200, publicDevice(dev));
  }

  if (method === "POST" && url.pathname === "/api/device/claim") {
    const body = await readBody(req);
    const device_id = String(body.device_id || "").trim();
    const pair_secret = String(body.pair_secret || "").trim();
    const dev = db.devices[device_id];
    if (!dev || dev.pair_secret !== pair_secret || !dev.account_id || !dev.device_token) {
      return json(res, 404, { error: "unpaired", status: "unpaired" });
    }
    const account = db.accounts[dev.account_id];
    return json(res, 200, deviceConfigPayload(dev, account));
  }

  if (method === "GET" && url.pathname === "/api/device/config") {
    const tok = bearer(req);
    const dev = Object.values(db.devices).find((d) => d.device_token && d.device_token === tok);
    if (!dev) return json(res, 401, { error: "unauthorized" });
    const account = db.accounts[dev.account_id];
    return json(res, 200, deviceConfigPayload(dev, account));
  }

  return json(res, 404, { error: "not found" });
}

function publicDevice(dev) {
  return {
    device_id: dev.device_id,
    voice: dev.voice,
    persona_extra: dev.persona_extra || "",
    boot_phrase: dev.boot_phrase,
    chat_profile: parseProfileJson(dev.chat_profile || ""),
    config_rev: dev.config_rev,
    paired_at: dev.paired_at || null,
  };
}

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url || "/", `http://${req.headers.host || "localhost"}`);
    if (url.pathname.startsWith("/api/")) {
      await handleApi(req, res, url);
      return;
    }
    serveStatic(req, res, url.pathname);
  } catch (err) {
    console.error(err);
    json(res, 500, { error: "internal error" });
  }
});

server.listen(PORT, HOST, () => {
  console.log(`talkbot control panel http://${HOST}:${PORT}`);
  console.log(`pair page: http://127.0.0.1:${PORT}/pair.html`);
});
