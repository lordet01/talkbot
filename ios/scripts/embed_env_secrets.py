#!/usr/bin/env python3
"""Write BundledSecrets.swift from repo .env. Generated file is gitignored."""
from pathlib import Path
import json

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / 'ios' / 'Talkbot' / 'Generated' / 'BundledSecrets.swift'


def parse_env(path: Path) -> dict[str, str]:
    values = {}
    if not path.exists():
        return values
    for raw in path.read_text(encoding='utf-8').splitlines():
        line = raw.strip()
        if not line or line.startswith('#') or '=' not in line:
            continue
        key, value = line.split('=', 1)
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def main() -> None:
    env = parse_env(REPO / '.env')
    key = env.get('OPENAI_API') or env.get('OPENAI_API_KEY') or ''
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(
        '// Generated from .env. Do not commit.\n'
        'enum BundledSecrets {\n'
        f'    static let openAIAPIKey = {json.dumps(key, ensure_ascii=False)}\n'
        '}\n',
        encoding='utf-8',
    )


if __name__ == '__main__':
    main()
