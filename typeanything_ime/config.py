"""Load config.json from the IME directory."""

import json
import os.path
from dataclasses import dataclass, field


@dataclass
class Config:
    api_key: str
    model: str
    endpoint: str
    target_lang: str = "english"
    lang_options: list[str] = field(default_factory=lambda: [
        "english", "japanese", "spanish", "french", "german", "korean",
    ])
    max_candidates: int = 5
    max_buffer: int = 40
    timeout_s: float = 5.0
    temperature: float = 0.3
    provider: str = "deepseek"


def load_config(ime_dir: str) -> Config:
    path = os.path.join(ime_dir, "config.json")
    if not os.path.isfile(path):
        path = os.path.join(ime_dir, "config.json.example")
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)

    target_lang = data.get("target_lang", "english")
    lang_options = data.get("lang_options") or [
        "english", "japanese", "spanish", "french", "german", "korean",
    ]
    if target_lang not in lang_options:
        lang_options = [target_lang] + [l for l in lang_options if l != target_lang]

    return Config(
        api_key=data.get("api_key", ""),
        model=data.get("model", "deepseek-chat"),
        endpoint=data.get("endpoint", "https://api.deepseek.com/v1/chat/completions"),
        target_lang=target_lang,
        lang_options=lang_options,
        max_candidates=int(data.get("max_candidates", 5)),
        max_buffer=int(data.get("max_buffer", 40)),
        timeout_s=float(data.get("timeout_s", 5.0)),
        temperature=float(data.get("temperature", 0.3)),
        provider=data.get("provider", "deepseek"),
    )
