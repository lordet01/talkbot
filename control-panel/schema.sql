CREATE TABLE IF NOT EXISTS accounts (
  id TEXT PRIMARY KEY,
  email TEXT NOT NULL UNIQUE,
  password_hash TEXT NOT NULL,
  groq_api_key TEXT NOT NULL DEFAULT '',
  google_api_key TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS sessions (
  token TEXT PRIMARY KEY,
  account_id TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  FOREIGN KEY (account_id) REFERENCES accounts(id)
);

CREATE TABLE IF NOT EXISTS devices (
  device_id TEXT PRIMARY KEY,
  pair_secret TEXT NOT NULL,
  device_token TEXT,
  account_id TEXT,
  voice TEXT NOT NULL DEFAULT 'ko-KR-Chirp3-HD-Kore',
  persona_extra TEXT NOT NULL DEFAULT '',
  boot_phrase TEXT NOT NULL DEFAULT '안녕, 난 디노야! 같이 놀자.',
  chat_profile TEXT NOT NULL DEFAULT '',
  config_rev INTEGER NOT NULL DEFAULT 1,
  paired_at INTEGER,
  created_at INTEGER NOT NULL,
  FOREIGN KEY (account_id) REFERENCES accounts(id)
);

CREATE INDEX IF NOT EXISTS idx_devices_account ON devices(account_id);
CREATE INDEX IF NOT EXISTS idx_devices_token ON devices(device_token);
