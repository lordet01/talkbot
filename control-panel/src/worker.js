/**
 * Cloudflare Worker + D1 production API.
 * Local/dev without CF: use ../local-server.mjs instead.
 */
import {
  catalogPayload,
  compilePersonaExtra,
  defaultBootPhrase,
  normalizeProfile,
  parseProfileJson,
} from "./chat-profile.js";

const VOICES = [
  { id: "ko-KR-Chirp3-HD-Kore", label: "Kore (따뜻함)" },
  { id: "ko-KR-Chirp3-HD-Leda", label: "Leda (밝음)" },
  { id: "ko-KR-Chirp3-HD-Puck", label: "Puck (활발)" },
  { id: "ko-KR-Chirp3-HD-Zephyr", label: "Zephyr (부드러움)" },
];

function json(data, status = 200) {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Headers": "Content-Type, Authorization",
      "Access-Control-Allow-Methods": "GET,POST,PUT,OPTIONS",
    },
  });
}

function bearer(req) {
  const h = req.headers.get("Authorization") || "";
  const m = /^Bearer\s+(.+)$/i.exec(h);
  return m ? m[1].trim() : "";
}

async function sha256(text) {
  const data = new TextEncoder().encode(text);
  const digest = await crypto.subtle.digest("SHA-256", data);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function token() {
  const bytes = new Uint8Array(24);
  crypto.getRandomValues(bytes);
  return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function accountFromSession(db, tok) {
  if (!tok) return null;
  const row = await db
    .prepare(
      `SELECT a.* FROM sessions s JOIN accounts a ON a.id = s.account_id WHERE s.token = ?`
    )
    .bind(tok)
    .first();
  return row || null;
}

function publicDevice(row) {
  const chat_profile = parseProfileJson(row.chat_profile || "");
  return {
    device_id: row.device_id,
    voice: row.voice,
    persona_extra: row.persona_extra || "",
    boot_phrase: row.boot_phrase,
    chat_profile,
    config_rev: row.config_rev,
    paired_at: row.paired_at || null,
  };
}

function devicePayload(row, account) {
  return {
    device_id: row.device_id,
    device_token: row.device_token || "",
    voice: row.voice,
    persona_extra: row.persona_extra || "",
    boot_phrase: row.boot_phrase,
    config_rev: row.config_rev,
    paired: Boolean(row.account_id && row.device_token),
    groq_api_key: account?.groq_api_key || "",
    google_api_key: account?.google_api_key || "",
  };
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === "OPTIONS") return json({}, 204);

    if (!url.pathname.startsWith("/api/")) {
      if (env.ASSETS) return env.ASSETS.fetch(request);
      return new Response("not found", { status: 404 });
    }

    try {
      return await handleApi(request, env, url);
    } catch (err) {
      return json({ error: String(err.message || err) }, 500);
    }
  },
};

async function handleApi(request, env, url) {
  const db = env.DB;
  const method = request.method;

  if (method === "GET" && url.pathname === "/api/health") {
    return json({ ok: true, voices: VOICES });
  }
  if (method === "GET" && url.pathname === "/api/voices") {
    return json({ voices: VOICES });
  }
  if (method === "GET" && url.pathname === "/api/chat-profile-schema") {
    return json(catalogPayload());
  }

  if (method === "POST" && url.pathname === "/api/auth/register") {
    const body = await request.json();
    const email = String(body.email || "").trim().toLowerCase();
    const password = String(body.password || "");
    if (!email || password.length < 6) return json({ error: "email and password(>=6) required" }, 400);
    const existing = await db.prepare(`SELECT id FROM accounts WHERE email = ?`).bind(email).first();
    if (existing) return json({ error: "email already registered" }, 409);
    const id = token();
    const password_hash = await sha256(`${email}:${password}`);
    await db
      .prepare(
        `INSERT INTO accounts (id, email, password_hash, groq_api_key, google_api_key, created_at)
         VALUES (?, ?, ?, '', '', ?)`
      )
      .bind(id, email, password_hash, Date.now())
      .run();
    const session = token();
    await db
      .prepare(`INSERT INTO sessions (token, account_id, created_at) VALUES (?, ?, ?)`)
      .bind(session, id, Date.now())
      .run();
    return json({ token: session, email });
  }

  if (method === "POST" && url.pathname === "/api/auth/login") {
    const body = await request.json();
    const email = String(body.email || "").trim().toLowerCase();
    const password = String(body.password || "");
    const password_hash = await sha256(`${email}:${password}`);
    const account = await db
      .prepare(`SELECT * FROM accounts WHERE email = ? AND password_hash = ?`)
      .bind(email, password_hash)
      .first();
    if (!account) return json({ error: "invalid credentials" }, 401);
    const session = token();
    await db
      .prepare(`INSERT INTO sessions (token, account_id, created_at) VALUES (?, ?, ?)`)
      .bind(session, account.id, Date.now())
      .run();
    return json({ token: session, email: account.email });
  }

  if (method === "GET" && url.pathname === "/api/me") {
    const account = await accountFromSession(db, bearer(request));
    if (!account) return json({ error: "unauthorized" }, 401);
    return json({
      email: account.email,
      has_groq_key: Boolean(account.groq_api_key),
      has_google_key: Boolean(account.google_api_key),
    });
  }

  if (method === "PUT" && url.pathname === "/api/account/keys") {
    const account = await accountFromSession(db, bearer(request));
    if (!account) return json({ error: "unauthorized" }, 401);
    const body = await request.json();
    const groq = typeof body.groq_api_key === "string" ? body.groq_api_key.trim() : account.groq_api_key;
    const google =
      typeof body.google_api_key === "string" ? body.google_api_key.trim() : account.google_api_key;
    await db
      .prepare(`UPDATE accounts SET groq_api_key = ?, google_api_key = ? WHERE id = ?`)
      .bind(groq, google, account.id)
      .run();
    return json({ ok: true, has_groq_key: Boolean(groq), has_google_key: Boolean(google) });
  }

  if (method === "POST" && url.pathname === "/api/devices/pair") {
    const account = await accountFromSession(db, bearer(request));
    if (!account) return json({ error: "unauthorized" }, 401);
    const body = await request.json();
    const device_id = String(body.device_id || "").trim();
    const pair_secret = String(body.pair_secret || "").trim();
    if (!device_id || !pair_secret) return json({ error: "device_id and pair_secret required" }, 400);
    let row = await db.prepare(`SELECT * FROM devices WHERE device_id = ?`).bind(device_id).first();
    if (!row) {
      const device_token = token();
      await db
        .prepare(
          `INSERT INTO devices
           (device_id, pair_secret, device_token, account_id, voice, persona_extra, boot_phrase, chat_profile, config_rev, paired_at, created_at)
           VALUES (?, ?, ?, ?, 'ko-KR-Chirp3-HD-Kore', '', '안녕, 난 디노야! 같이 놀자.', '', 1, ?, ?)`
        )
        .bind(device_id, pair_secret, device_token, account.id, Date.now(), Date.now())
        .run();
    } else {
      if (row.pair_secret !== pair_secret) return json({ error: "invalid pair secret" }, 403);
      if (row.account_id && row.account_id !== account.id) {
        return json({ error: "device already paired to another account" }, 409);
      }
      const device_token = row.device_token || token();
      await db
        .prepare(
          `UPDATE devices SET account_id = ?, device_token = ?, paired_at = ? WHERE device_id = ?`
        )
        .bind(account.id, device_token, Date.now(), device_id)
        .run();
    }
    row = await db.prepare(`SELECT * FROM devices WHERE device_id = ?`).bind(device_id).first();
    return json({ ok: true, device: publicDevice(row) });
  }

  if (method === "GET" && url.pathname === "/api/devices") {
    const account = await accountFromSession(db, bearer(request));
    if (!account) return json({ error: "unauthorized" }, 401);
    const { results } = await db
      .prepare(`SELECT * FROM devices WHERE account_id = ? ORDER BY paired_at DESC`)
      .bind(account.id)
      .all();
    return json({ devices: (results || []).map(publicDevice) });
  }

  const configMatch = url.pathname.match(/^\/api\/devices\/([^/]+)\/config$/);
  if (configMatch && method === "GET") {
    const account = await accountFromSession(db, bearer(request));
    if (!account) return json({ error: "unauthorized" }, 401);
    const device_id = decodeURIComponent(configMatch[1]);
    const row = await db.prepare(`SELECT * FROM devices WHERE device_id = ?`).bind(device_id).first();
    if (!row || row.account_id !== account.id) return json({ error: "not found" }, 404);
    return json(publicDevice(row));
  }

  if (configMatch && method === "PUT") {
    const account = await accountFromSession(db, bearer(request));
    if (!account) return json({ error: "unauthorized" }, 401);
    const device_id = decodeURIComponent(configMatch[1]);
    const row = await db.prepare(`SELECT * FROM devices WHERE device_id = ?`).bind(device_id).first();
    if (!row || row.account_id !== account.id) return json({ error: "not found" }, 404);
    const body = await request.json();
    const voice = typeof body.voice === "string" && body.voice ? body.voice.trim() : row.voice;

    let profile = parseProfileJson(row.chat_profile || "");
    if (body.chat_profile && typeof body.chat_profile === "object") {
      profile = normalizeProfile(body.chat_profile);
    }

    // Structured profile always wins over raw persona_extra when chat_profile is sent.
    let persona_extra = compilePersonaExtra(profile, 360);
    if (typeof body.persona_extra === "string" && body.persona_extra.trim() && !body.chat_profile) {
      persona_extra = body.persona_extra.trim().slice(0, 360);
    }

    let boot_phrase = row.boot_phrase;
    if (typeof body.boot_phrase === "string" && body.boot_phrase.trim()) {
      boot_phrase = body.boot_phrase.trim().slice(0, 90);
    } else if (body.chat_profile && typeof body.chat_profile === "object") {
      // Keep custom boot unless empty — then derive from character name.
      if (!boot_phrase || !String(boot_phrase).trim()) {
        boot_phrase = defaultBootPhrase(profile);
      }
    }

    const config_rev = (row.config_rev || 0) + 1;
    const chat_profile_json = JSON.stringify(profile);
    try {
      await db
        .prepare(
          `UPDATE devices SET voice = ?, persona_extra = ?, boot_phrase = ?, chat_profile = ?, config_rev = ? WHERE device_id = ?`
        )
        .bind(voice, persona_extra, boot_phrase, chat_profile_json, config_rev, device_id)
        .run();
    } catch (e) {
      // Pre-migration DBs without chat_profile column.
      if (String(e.message || e).includes("chat_profile")) {
        await db
          .prepare(
            `UPDATE devices SET voice = ?, persona_extra = ?, boot_phrase = ?, config_rev = ? WHERE device_id = ?`
          )
          .bind(voice, persona_extra, boot_phrase, config_rev, device_id)
          .run();
      } else {
        throw e;
      }
    }
    const updated = await db.prepare(`SELECT * FROM devices WHERE device_id = ?`).bind(device_id).first();
    return json(publicDevice(updated));
  }

  if (method === "POST" && url.pathname === "/api/device/claim") {
    const body = await request.json();
    const device_id = String(body.device_id || "").trim();
    const pair_secret = String(body.pair_secret || "").trim();
    const row = await db.prepare(`SELECT * FROM devices WHERE device_id = ?`).bind(device_id).first();
    if (!row || row.pair_secret !== pair_secret || !row.account_id || !row.device_token) {
      return json({ error: "unpaired", status: "unpaired" }, 404);
    }
    const account = await db.prepare(`SELECT * FROM accounts WHERE id = ?`).bind(row.account_id).first();
    return json(devicePayload(row, account));
  }

  if (method === "GET" && url.pathname === "/api/device/config") {
    const tok = bearer(request);
    const row = await db.prepare(`SELECT * FROM devices WHERE device_token = ?`).bind(tok).first();
    if (!row) return json({ error: "unauthorized" }, 401);
    const account = await db.prepare(`SELECT * FROM accounts WHERE id = ?`).bind(row.account_id).first();
    return json(devicePayload(row, account));
  }

  return json({ error: "not found" }, 404);
}
