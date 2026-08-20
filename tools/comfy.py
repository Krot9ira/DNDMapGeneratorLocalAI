#!/usr/bin/env python3
"""Minimal ComfyUI HTTP client: health, upload, queue, wait, download."""
import json
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path


class ComfyError(RuntimeError):
    pass


class ComfyClient:
    def __init__(self, base_url="http://127.0.0.1:8188", timeout=60):
        self.base = base_url.rstrip("/")
        self.timeout = timeout

    # -- transport ----------------------------------------------------
    def _request(self, method, path, data=None, headers=None, raw=False, timeout=None):
        hdrs = dict(headers or {})
        body = None
        if isinstance(data, (dict, list)):
            body = json.dumps(data).encode("utf-8")
            hdrs.setdefault("Content-Type", "application/json")
        elif isinstance(data, bytes):
            body = data

        req = urllib.request.Request(self.base + path, data=body, headers=hdrs, method=method)
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as resp:
                payload = resp.read()
                return payload if raw else json.loads(payload)
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            try:
                parsed = json.loads(detail)
                # ComfyUI reports validation failures here; surfacing the node
                # and reason is the difference between a fixable error and a
                # mystery.
                if isinstance(parsed, dict) and "error" in parsed:
                    msg = parsed["error"]
                    if isinstance(msg, dict):
                        msg = msg.get("message", msg)
                    extra = parsed.get("node_errors") or {}
                    detail = f"{msg} {json.dumps(extra, ensure_ascii=False)}" if extra else str(msg)
            except ValueError:
                pass
            raise ComfyError(f"HTTP {exc.code} on {path}: {detail}") from exc
        except urllib.error.URLError as exc:
            raise ComfyError(f"cannot reach {self.base}: {exc.reason}") from exc

    # -- api ----------------------------------------------------------
    def health(self):
        """Return (ok, detail). Used to fail fast with a helpful message."""
        try:
            stats = self._request("GET", "/system_stats", timeout=5)
            version = (stats.get("system") or {}).get("comfyui_version", "unknown")
            return True, f"ComfyUI {version}"
        except ComfyError as exc:
            return False, str(exc)

    def free_memory(self):
        """Ask ComfyUI to drop its models and hand the card back.

        Both services want most of the graphics card, so whichever one is about
        to work asks the other to let go first. Failure is not interesting: it
        only means the render or the plan will be slower.
        """
        try:
            # The endpoint answers with an empty body, so no JSON parsing.
            self._request("POST", "/free", raw=True,
                          data={"unload_models": True, "free_memory": True}, timeout=30)
            return True
        except Exception:
            return False

    def upload_image(self, path, overwrite=True):
        path = Path(path)
        boundary = uuid.uuid4().hex
        head = (f"--{boundary}\r\n"
                f'Content-Disposition: form-data; name="image"; filename="{path.name}"\r\n'
                f"Content-Type: image/png\r\n\r\n").encode()
        tail = (f"\r\n--{boundary}\r\n"
                f'Content-Disposition: form-data; name="overwrite"\r\n\r\n'
                f"{'true' if overwrite else 'false'}\r\n"
                f"--{boundary}--\r\n").encode()
        body = head + path.read_bytes() + tail
        result = self._request("POST", "/upload/image", data=body,
                               headers={"Content-Type":
                                        f"multipart/form-data; boundary={boundary}"})
        if isinstance(result, dict) and result.get("name"):
            sub = result.get("subfolder") or ""
            return f"{sub}/{result['name']}" if sub else result["name"]
        return path.name

    def queue_prompt(self, graph, client_id=None):
        payload = {"prompt": graph, "client_id": client_id or uuid.uuid4().hex}
        resp = self._request("POST", "/prompt", data=payload)
        prompt_id = resp.get("prompt_id")
        if not prompt_id:
            raise ComfyError(f"ComfyUI did not return a prompt_id: {resp}")
        return prompt_id

    def _queue_position(self, prompt_id):
        try:
            q = self._request("GET", "/queue", timeout=10)
        except ComfyError:
            return None
        for item in q.get("queue_running", []):
            if len(item) > 1 and item[1] == prompt_id:
                return "running"
        for index, item in enumerate(q.get("queue_pending", [])):
            if len(item) > 1 and item[1] == prompt_id:
                return f"queued (position {index + 1})"
        return None

    def wait(self, prompt_id, timeout=1800, poll=1.5, on_progress=None):
        start = time.time()
        last_note = None
        while time.time() - start < timeout:
            try:
                history = self._request("GET", f"/history/{prompt_id}", timeout=15)
            except ComfyError:
                history = {}
            entry = history.get(prompt_id) if isinstance(history, dict) else None
            if entry:
                status = entry.get("status", {}) or {}
                for message in status.get("messages", []) or []:
                    if isinstance(message, (list, tuple)) and message[0] == "execution_error":
                        raise ComfyError(f"ComfyUI execution error: {message[1]}")
                if status.get("completed") or status.get("status_str") == "success":
                    return entry.get("outputs", {}) or {}
                if status.get("status_str") == "error":
                    raise ComfyError(f"ComfyUI reported an error: {status}")

            if on_progress:
                note = self._queue_position(prompt_id) or "rendering"
                elapsed = int(time.time() - start)
                text = f"{note}, {elapsed}s elapsed"
                if text != last_note:
                    on_progress(text)
                    last_note = text
            time.sleep(poll)
        raise ComfyError(f"timed out after {timeout}s waiting for {prompt_id}")

    def get_images(self, outputs, out_dir):
        out_dir = Path(out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        saved = []
        for node_out in outputs.values():
            for img in node_out.get("images", []) or []:
                if img.get("type") == "temp":
                    continue
                params = (f"filename={urllib.parse.quote(img['filename'])}"
                          f"&subfolder={urllib.parse.quote(img.get('subfolder', ''))}"
                          f"&type={img.get('type', 'output')}")
                data = self._request("GET", f"/view?{params}", raw=True, timeout=120)
                target = out_dir / img["filename"]
                target.write_bytes(data)
                saved.append(str(target))
        return saved
