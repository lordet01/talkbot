-- Add structured chatbot profile JSON (compiled into persona_extra on save).
ALTER TABLE devices ADD COLUMN chat_profile TEXT NOT NULL DEFAULT '';
