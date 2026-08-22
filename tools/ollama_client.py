#!/usr/bin/env python3
"""Ollama client: text and vision generation with JSON-schema constrained output."""
import base64
import json
import re
import urllib.error
import urllib.request
from pathlib import Path


class OllamaError(RuntimeError):
    pass


class OllamaClient:
    def __init__(self, base_url="http://127.0.0.1:11434", model="qwen3.8:27b", timeout=600):
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.timeout = timeout

    def check_health(self):
        """Return (ok, models_or_error)."""
        try:
            with urllib.request.urlopen(f"{self.base_url}/api/tags", timeout=5) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            return True, [m.get("name") for m in data.get("models", [])]
        except (urllib.error.URLError, OSError, ValueError) as exc:
            return False, str(exc)

    @staticmethod
    def _encode_images(images):
        encoded = []
        for img in images or []:
            if isinstance(img, bytes):
                encoded.append(base64.b64encode(img).decode("ascii"))
                continue
            path = Path(str(img))
            if path.exists():
                encoded.append(base64.b64encode(path.read_bytes()).decode("ascii"))
            else:
                encoded.append(str(img))  # assume it is already base64
        return encoded

    def unload(self):
        """Drop the model from memory now instead of in five minutes.

        Ollama keeps a model resident long after a request, which is the
        difference between ComfyUI having the card to itself and the two of them
        thrashing.
        """
        try:
            self._post({"model": self.model, "prompt": "", "keep_alive": 0})
            return True
        except Exception:
            return False

    def generate(self, prompt, system=None, format=None, temperature=0.6,
                 images=None, num_predict=2048, think=None, num_ctx=None):
        """Call /api/generate.

        `format` may be "json" or a JSON schema dict - the schema path is what
        keeps the planner's output parseable without heroic regex recovery.
        `think=False` disables the reasoning preamble on thinking models, which
        both speeds planning up and stops <think> blocks leaking into the JSON.
        """
        payload = {
            "model": self.model,
            "prompt": prompt,
            "stream": False,
            "options": {"temperature": float(temperature), "num_predict": int(num_predict)},
        }
        if num_ctx:
            # Planning a map costs a fraction of what rendering one does, so the
            # window is sized for the whole system prompt, the style catalogue
            # and a long answer rather than trimmed to be quick. A plan that got
            # truncated is a plan somebody has to do again.
            payload["options"]["num_ctx"] = int(num_ctx)
        if system:
            payload["system"] = system
        if format is not None:
            payload["format"] = format
        if think is not None:
            payload["think"] = bool(think)
        encoded = self._encode_images(images)
        if encoded:
            payload["images"] = encoded

        try:
            return self._post(payload)
        except OllamaError as exc:
            # Not every model accepts `think`; retry once without it rather than
            # failing the whole plan.
            if think is not None and "think" in str(exc).lower():
                payload.pop("think", None)
                return self._post(payload)
            raise

    def _post(self, payload):
        request = urllib.request.Request(
            f"{self.base_url}/api/generate",
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST")
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as resp:
                result = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise OllamaError(f"Ollama HTTP {exc.code}: {detail}") from exc
        except urllib.error.URLError as exc:
            raise OllamaError(f"cannot reach Ollama at {self.base_url}: {exc.reason}") from exc
        except (ValueError, OSError) as exc:
            raise OllamaError(f"Ollama request failed: {exc}") from exc

        text = result.get("response", "")
        if not text:
            text = result.get("thinking", "")
        return text

    @staticmethod
    def extract_json(raw_text):
        """Pull a JSON object out of model output, tolerating think blocks,
        code fences and trailing commas."""
        if not raw_text:
            return None
        cleaned = re.sub(r"<think>.*?</think>", "", raw_text, flags=re.DOTALL).strip()

        for match in re.finditer(r"```(?:json)?\s*([\s\S]*?)\s*```", cleaned):
            block = match.group(1).strip()
            parsed = _try_parse(block)
            if parsed is not None:
                return parsed

        parsed = _try_parse(cleaned)
        if parsed is not None:
            return parsed

        start, end = cleaned.find("{"), cleaned.rfind("}")
        if start != -1 and end > start:
            snippet = cleaned[start:end + 1]
            snippet = re.sub(r"//[^\n]*", "", snippet)
            snippet = re.sub(r"/\*.*?\*/", "", snippet, flags=re.DOTALL)
            snippet = re.sub(r",\s*([}\]])", r"\1", snippet)
            parsed = _try_parse(snippet)
            if parsed is not None:
                return parsed
        raise ValueError(f"no JSON object found in model output:\n{raw_text[:500]}")


def _try_parse(text):
    text = (text or "").strip()
    if not text:
        return None
    try:
        return json.loads(text)
    except ValueError:
        pass
    start = text.find("{")
    if start == -1:
        return None
    try:
        obj, _ = json.JSONDecoder().raw_decode(text, start)
        return obj
    except ValueError:
        return None
