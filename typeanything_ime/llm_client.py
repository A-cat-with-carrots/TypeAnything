"""LLM client — Plan F5.

Stage 1 (per-keystroke pinyin -> Chinese candidates) is now handled by the
local pinyin_engine, NOT the LLM. The LLM is only used for stage 2:
accumulated Chinese -> target language translation, triggered on Enter.

Persistent httpx.Client kept open across calls (LLMSession) so TLS pooling
keeps subsequent calls fast (~500-700ms typical for short translations).
"""

import httpx


def _build_payload(model: str, system_prompt: str, text: str, temperature: float) -> dict:
    return {
        "model": model,
        "temperature": temperature,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": text},
        ],
    }


def _build_headers(api_key: str) -> dict:
    return {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }


def _stage2_prompt(target_lang: str) -> str:
    return (
        f"Translate the user's Chinese text to natural conversational {target_lang}. "
        f"Reply with only the translation — no quotes, no markdown, no prose."
    )


class LLMSession:
    """Sync httpx.Client with connection pooling for stage-2 translation."""

    def __init__(self, *, api_key: str, model: str, endpoint: str,
                 timeout_s: float = 5.0, temperature: float = 0.3):
        if not api_key:
            raise ValueError("api_key required")
        self.api_key = api_key
        self.model = model
        self.endpoint = endpoint
        self.timeout_s = timeout_s
        self.temperature = temperature
        self._client = httpx.Client(
            timeout=timeout_s,
            limits=httpx.Limits(max_keepalive_connections=2, max_connections=4),
        )

    def chinese_to_target(self, chinese: str, target_lang: str) -> str:
        if not chinese.strip():
            return ""
        payload = _build_payload(
            self.model, _stage2_prompt(target_lang), chinese, self.temperature)
        headers = _build_headers(self.api_key)
        resp = self._client.post(self.endpoint, json=payload, headers=headers)
        resp.raise_for_status()
        raw = resp.json()["choices"][0]["message"]["content"].strip()
        # Strip surrounding quotes / fences if model added them
        return raw.strip("\"'`")

    def close(self):
        try:
            self._client.close()
        except Exception:
            pass
