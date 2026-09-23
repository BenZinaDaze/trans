"""Native business contracts using real files and a local HTTP peer; no Qt runtime."""

import argparse
import copy
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
import unittest


DRIVER = None


def isolated_environment():
    environment = os.environ.copy()
    # A developer's proxy configuration must not route fixture credentials away
    # from the ephemeral loopback peer.
    for name in tuple(environment):
        if name.lower() in {"http_proxy", "https_proxy", "all_proxy", "no_proxy"}:
            del environment[name]
    environment["NO_PROXY"] = "127.0.0.1,localhost"
    return environment


class HttpPeer:
    def __init__(self):
        self.requests = queue.Queue()
        self.responses = queue.Queue()
        peer = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_):
                pass

            def do_GET(self):
                self.respond()

            def do_POST(self):
                self.respond()

            def respond(self):
                size = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(size)
                peer.requests.put({"path": self.path, "method": self.command,
                                   "headers": {key.lower(): value for key, value in self.headers.items()},
                                   "body": json.loads(body) if body else None})
                try:
                    status, headers, payload, delay = peer.responses.get(timeout=5)
                except queue.Empty:
                    return
                if delay:
                    time.sleep(delay)
                try:
                    self.send_response(status)
                    self.send_header("Content-Length", str(len(payload)))
                    self.send_header("Content-Type", "application/json")
                    for key, value in headers.items():
                        self.send_header(key, value)
                    self.end_headers()
                    self.wfile.write(payload)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.endpoint = f"http://127.0.0.1:{self.server.server_port}"

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def reply(self, body, *, status=200, headers=None, delay=0):
        payload = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.responses.put((status, headers or {}, payload, delay))

    def received(self):
        return self.requests.get(timeout=5)


class BusinessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if DRIVER is None:
            raise unittest.SkipTest("Pass --driver with the native business test executable")

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name) / "配置目录"
        self.root.mkdir()
        self.settings = self.call("defaults")

    def call(self, operation, **fields):
        request = {"operation": operation, **fields}
        if operation not in {"defaults", "load"}:
            request.setdefault("settings", self.settings)
        process = subprocess.run([DRIVER], input=json.dumps(request) + "\n", text=True,
                                 encoding="utf-8", capture_output=True, timeout=10,
                                 env=isolated_environment())
        self.assertEqual(process.returncode, 0, process.stderr)
        return json.loads(process.stdout)

    def peer(self, provider="openai", mode="chat"):
        peer = HttpPeer()
        self.addCleanup(peer.close)
        self.settings["providerId"] = provider
        config = self.settings["providerConfigs"][provider]
        config.update(endpoint=peer.endpoint + "/proxy/v1/", apiKey="fixture-private-key",
                      model="fixture-model", apiMode=mode)
        return peer, config

    @staticmethod
    def translation(text="译文"):
        return {"choices": [{"message": {"content": text}, "finish_reason": "stop"}]}

    def test_ini_import_preserves_unicode_escaping_and_old_openai_protocol(self):
        # QSettings writes nested paths as backslashes, escaped quotes/backslashes
        # within values, quoted comma/semicolon/equal-bearing strings, and @@ for @.
        legacy = r'''[translation]
provider=deepseek
sourceLanguage=en
targetLanguage=ja
systemPrompt="Translate {{sourceLanguage}} to {{targetLanguage}}.\n保留段落; \"引用\", \\path"
timeoutSeconds=61
maxInputChars=9001
maxResponseKiB=501
[desktop]
shortcut=Ctrl+Alt+Y
screenshotShortcut=Ctrl+Alt+O
[window]
fontSize=23
stayOnTop=false
restoreFocus=false
position=cursor
[ocr]
baidu\apiKey="@@ocr-key;with,delimiters="
baidu\secretKey="密钥\\秘密"
[pro%76iders]
openai\endpoint=https://proxy.example/v1
openai\model=legacy-model
openai\api%4Bey=@@literal-at-key
openai\temperatureEnabled=true
openai\temperature=0.7
openai\maxOutputTokens=1024
openai\headersJson="{\"X-Project\":\"中文; a,b=c\\\\path\"}"
openai\optionsJson="{\"top_p\":0.8}"
deepseek\endpoint=https://deepseek.example
deepseek\model=custom-deepseek
deepseek\apiKey=retained-deepseek-key
deepseek\apiMode=chat
deepseek\reasoning=max
'''.encode("utf-8")
        ini = self.root / "settings.ini"
        ini.write_bytes(b"\xef\xbb\xbf" + legacy.replace(b"\n", b"\r\n"))
        original = ini.read_bytes()
        loaded = self.call("load", directory=str(self.root))
        self.assertEqual(loaded["error"], "")
        settings = loaded["settings"]
        self.assertEqual(settings["providerId"], "deepseek")
        self.assertEqual(settings["sourceLanguage"], "en")
        self.assertEqual(settings["targetLanguage"], "ja")
        self.assertEqual(settings["systemPrompt"],
                         'Translate {{sourceLanguage}} to {{targetLanguage}}.\n保留段落; "引用", \\path')
        for name, value in {"timeoutSeconds": 61, "maxInputChars": 9001, "maxResponseKiB": 501,
                            "fontSize": 23, "stayOnTop": False, "restoreFocus": False,
                            "popupPosition": "cursor", "shortcut": "Ctrl+Alt+Y",
                            "screenshotShortcut": "Ctrl+Alt+O"}.items():
            self.assertEqual(settings[name], value)
        self.assertEqual(settings["ocrApiKey"], "@ocr-key;with,delimiters=")
        self.assertEqual(settings["ocrSecretKey"], "密钥\\秘密")
        openai = settings["providerConfigs"]["openai"]
        self.assertEqual(openai["endpoint"], "https://proxy.example/v1")
        self.assertEqual(openai["model"], "legacy-model")
        self.assertEqual(openai["apiKey"], "@literal-at-key")
        self.assertEqual(openai["apiMode"], "chat")
        self.assertEqual(openai["reasoning"], "default")
        self.assertEqual(openai["temperature"], 0.7)
        self.assertTrue(openai["temperatureEnabled"])
        self.assertEqual(openai["maxOutputTokens"], 1024)
        self.assertEqual(json.loads(openai["headersJson"]), {"X-Project": "中文; a,b=c\\path"})
        self.assertEqual(json.loads(openai["optionsJson"]), {"top_p": 0.8})
        self.assertEqual(settings["providerConfigs"]["deepseek"]["apiKey"], "retained-deepseek-key")
        self.assertEqual(settings["providerConfigs"]["deepseek"]["reasoning"], "max")
        self.assertEqual(ini.read_bytes(), original)
        self.assertFalse((self.root / "settings.json").exists())

    def test_ini_hex_surrogates_continuations_and_explicit_responses_mode(self):
        ini = self.root / "settings.ini"
        ini.write_bytes(br'''[translation]
systemPrompt="\x4e2d\xd83d\xde00 {{targetLanguage}} \
continued"
[providers]
openai\endpoint=https://api.openai.com/v1
openai\apiMode=responses
openai\reasoning=high
''')
        loaded = self.call("load", directory=str(self.root))
        self.assertEqual(loaded["error"], "")
        self.assertEqual(loaded["settings"]["systemPrompt"], "中😀 {{targetLanguage}} continued")
        config = loaded["settings"]["providerConfigs"]["openai"]
        self.assertEqual(config["apiMode"], "responses")
        self.assertEqual(config["reasoning"], "high")

    def test_json_preferred_without_overwriting_or_reimporting_ini(self):
        (self.root / "settings.ini").write_text("[providers]\nopenai\\apiKey=old-key\n", encoding="utf-8")
        self.settings["providerConfigs"]["openai"]["apiKey"] = "new-private-key"
        self.settings["providerConfigs"]["openai"]["apiMode"] = "responses"
        path = self.root / "settings.json"
        path.write_text(json.dumps(self.settings), encoding="utf-8")
        original = path.read_bytes()
        loaded = self.call("load", directory=str(self.root))
        self.assertEqual(loaded["error"], "")
        self.assertEqual(loaded["settings"]["providerConfigs"]["openai"]["apiKey"], "new-private-key")
        self.assertEqual(path.read_bytes(), original)

    def test_damaged_future_and_incomplete_json_never_fall_back_to_legacy(self):
        (self.root / "settings.ini").write_text("[providers]\nopenai\\apiKey=old-secret-sentinel\n", encoding="utf-8")
        path = self.root / "settings.json"
        future = copy.deepcopy(self.settings)
        future["schemaVersion"] = 99
        future["providerConfigs"]["openai"]["apiKey"] = "future-secret-sentinel"
        incomplete = copy.deepcopy(self.settings)
        del incomplete["providerConfigs"]["openai"]["apiKey"]
        malformed = b'{"schemaVersion":1,"providerConfigs":{"apiKey":"do-not-leak-secret"},broken}'
        for payload in [malformed, json.dumps(future).encode(), json.dumps(incomplete).encode()]:
            with self.subTest(payload=payload[:24]):
                path.write_bytes(payload)
                loaded = self.call("load", directory=str(self.root))
                self.assertNotEqual(loaded["error"], "")
                self.assertNotIn("old-secret-sentinel", json.dumps(loaded))
                self.assertNotIn("do-not-leak-secret", loaded["error"])
                self.assertEqual(path.read_bytes(), payload)
                if payload == malformed:
                    self.assertIsNone(loaded["settings"])
        path.write_text(json.dumps(future), encoding="utf-8")
        self.assertEqual(self.call("load", directory=str(self.root))["settings"]
                         ["providerConfigs"]["openai"]["apiKey"], "future-secret-sentinel")

    def test_malformed_ini_is_reported_without_writing_new_settings(self):
        path = self.root / "settings.ini"
        for payload in [b'[providers]\nopenai\\apiKey="unterminated-private-key',
                        b'[translation]\ntimeoutSeconds=not-a-number\n']:
            with self.subTest(payload=payload[:24]):
                path.write_bytes(payload)
                loaded = self.call("load", directory=str(self.root))
                self.assertNotEqual(loaded["error"], "")
                self.assertNotIn("unterminated-private-key", loaded["error"])
                self.assertEqual(path.read_bytes(), payload)
                self.assertFalse((self.root / "settings.json").exists())

    def test_responses_protocol_ignores_reasoning_and_preserves_complete_text(self):
        peer, config = self.peer(mode="responses")
        self.settings.update(sourceLanguage="en", targetLanguage="ja",
                             systemPrompt="Translate {{sourceLanguage}} to {{targetLanguage}}. Keep lines.")
        config.update(temperatureEnabled=True, temperature=0.4, maxOutputTokens=1024,
                      reasoning="low", headersJson='{"X-Project":"trans","Accept":"application/custom+json"}',
                      optionsJson='{"top_p":0.9}')
        peer.reply({"status": "completed", "output": [
            {"type": "reasoning", "summary": [{"text": "never show reasoning"}]},
            {"type": "message", "role": "assistant", "content": [
                {"type": "output_text", "text": "第一段"}, {"type": "output_text", "text": "第二段"}]}]})
        result = self.call("translate", text="Hello\n世界")
        self.assertEqual(result["error"], "")
        self.assertEqual(result["text"], "第一段\n第二段")
        request = peer.received()
        self.assertEqual(request["path"], "/proxy/v1/responses")
        self.assertEqual(request["headers"]["authorization"], "Bearer fixture-private-key")
        self.assertEqual(request["headers"]["accept"], "application/custom+json")
        self.assertEqual(request["headers"]["x-project"], "trans")
        self.assertEqual(request["body"], {
            "model": "fixture-model", "stream": False, "store": False,
            "input": "Hello\n世界", "instructions": "Translate en to ja. Keep lines.",
            "temperature": 0.4, "max_output_tokens": 1024, "reasoning": {"effort": "low"}, "top_p": 0.9})

    def test_chat_and_deepseek_use_distinct_token_and_thinking_parameters(self):
        for provider in ["openai", "deepseek"]:
            with self.subTest(provider=provider):
                peer, config = self.peer(provider=provider)
                config.update(maxOutputTokens=512, reasoning="high")
                response = self.translation("正确译文")
                response["choices"][0]["message"]["reasoning_content"] = "private chain of thought"
                peer.reply(response)
                result = self.call("translate", text="Hello")
                self.assertEqual(result["error"], "")
                self.assertEqual(result["text"], "正确译文")
                request = peer.received()
                self.assertEqual(request["path"], "/proxy/v1/chat/completions")
                body = request["body"]
                self.assertEqual(body["messages"][1], {"role": "user", "content": "Hello"})
                self.assertIn("its detected language", body["messages"][0]["content"])
                self.assertEqual(body["reasoning_effort"], "high")
                self.assertNotIn("temperature", body)
                if provider == "deepseek":
                    self.assertEqual(body["max_tokens"], 512)
                    self.assertEqual(body["thinking"], {"type": "enabled"})
                    self.assertNotIn("max_completion_tokens", body)
                    config.update(reasoning="none", maxOutputTokens=0)
                    peer.reply(self.translation())
                    self.assertEqual(self.call("translate", text="Hello")["error"], "")
                    disabled = peer.received()["body"]
                    self.assertEqual(disabled["thinking"], {"type": "disabled"})
                    self.assertNotIn("reasoning_effort", disabled)
                    self.assertNotIn("max_tokens", disabled)
                else:
                    self.assertEqual(body["max_completion_tokens"], 512)
                    self.assertNotIn("max_tokens", body)
                    self.assertNotIn("thinking", body)

    def test_incomplete_responses_and_chat_refusals_never_publish_partial_text(self):
        peer, config = self.peer(mode="responses")
        peer.reply({"status": "incomplete", "output": [{"type": "message", "role": "assistant", "content": [
            {"type": "output_text", "text": "partial-secret-sentinel"}]}]})
        result = self.call("translate", text="Hello")
        self.assertNotEqual(result["error"], "")
        self.assertEqual(result["text"], "")
        peer.received()
        config["apiMode"] = "chat"
        for response in [{"choices": [{"message": {"content": "partial"}, "finish_reason": "length"}]},
                         {"choices": [{"message": {"content": None, "refusal": "refused"}, "finish_reason": "stop"}]}]:
            peer.reply(response)
            result = self.call("translate", text="Hello")
            self.assertNotEqual(result["error"], "")
            self.assertEqual(result["text"], "")
            peer.received()

    def test_utf16_length_and_unicode_trim_at_input_boundary(self):
        peer, _ = self.peer()
        self.settings["maxInputChars"] = 3
        peer.reply(self.translation())
        self.assertEqual(self.call("translate", text="\u00a0中😀\u3000")["error"], "")
        self.assertEqual(peer.received()["body"]["messages"][1]["content"], "中😀")
        self.settings["maxInputChars"] = 2
        result = self.call("translate", text="中😀")
        self.assertNotEqual(result["error"], "")
        self.assertEqual(result["text"], "")
        empty = self.call("translate", text="\u00a0\n\t\u3000")
        self.assertNotEqual(empty["error"], "")
        self.assertTrue(peer.requests.empty())

    def test_unsaved_model_configuration_and_empty_model_are_supported(self):
        peer, config = self.peer(provider="deepseek")
        config["model"] = ""
        peer.reply({"data": [{"id": "custom"}, {"id": "another"}, {"id": "custom"}]})
        result = self.call("models", providerId="deepseek")
        self.assertEqual(result, {"error": "", "models": ["another", "custom"]})
        request = peer.received()
        self.assertEqual(request["method"], "GET")
        self.assertEqual(request["path"], "/proxy/v1/models")
        self.assertEqual(request["headers"]["authorization"], "Bearer fixture-private-key")
        peer.reply({"data": []})
        self.assertEqual(self.call("models", providerId="deepseek"), {"error": "", "models": []})
        peer.received()
        peer.reply({"data": "invalid"})
        failed = self.call("models", providerId="deepseek")
        self.assertNotEqual(failed["error"], "")
        self.assertEqual(failed["models"], [])
        self.assertFalse((self.root / "settings.json").exists())

    def test_managed_headers_options_and_shortcut_collisions_are_rejected(self):
        peer, config = self.peer()
        for field, value in [("headersJson", '{"AUTHORIZATION":"stolen"}'),
                             ("headersJson", '{"X-Test":"injected\\r\\nHost: elsewhere"}'),
                             ("optionsJson", '{"stream":true}'),
                             ("optionsJson", '{"messages":[]}'),
                             ("apiKey", "private\x00hidden")]:
            with self.subTest(field=field, value=value[:20]):
                original = config[field]
                config[field] = value
                result = self.call("translate", text="Hello")
                self.assertNotEqual(result["error"], "")
                self.assertEqual(result["text"], "")
                config[field] = original
        self.assertTrue(peer.requests.empty())
        self.settings.update(shortcut="Ctrl+Alt+Y", screenshotShortcut="Alt+Ctrl+y")
        self.assertNotEqual(self.call("validate")["error"], "")
        self.settings["screenshotShortcut"] = ""
        self.assertEqual(self.call("validate")["error"], "")

    def test_auth_errors_are_redacted_and_redirects_are_not_followed(self):
        peer, _ = self.peer()
        for status in [401, 403, 429, 500]:
            with self.subTest(status=status):
                peer.reply({"error": {"message": "server-secret-sentinel fixture-private-key"}}, status=status)
                result = self.call("translate", text="Hello")
                self.assertNotEqual(result["error"], "")
                self.assertEqual(result["text"], "")
                self.assertNotIn("server-secret-sentinel", result["error"])
                self.assertNotIn("fixture-private-key", result["error"])
                peer.received()
        peer.reply({}, status=302, headers={"Location": peer.endpoint + "/credential-leak"})
        result = self.call("translate", text="Hello")
        self.assertNotEqual(result["error"], "")
        self.assertEqual(peer.received()["path"], "/proxy/v1/chat/completions")
        self.assertTrue(peer.requests.empty())

    def test_malformed_and_oversized_compressed_responses_are_bounded(self):
        peer, _ = self.peer()
        self.settings["maxResponseKiB"] = 16
        for body, headers in [(b'{"server-secret-sentinel":broken}', {}),
                              (b"x" * (16 * 1024 + 1), {}),
                              (gzip.compress(b"x" * (16 * 1024 + 1)), {"Content-Encoding": "gzip"}),
                              (b'{"nested":' + b"[" * 129 + b"0" + b"]" * 129 + b"}", {})]:
            peer.reply(body, headers=headers)
            result = self.call("translate", text="Hello")
            self.assertNotEqual(result["error"], "")
            self.assertNotIn("server-secret-sentinel", result["error"])
            self.assertEqual(result["text"], "")
            peer.received()

    def test_timeout_and_cancellation_while_waiting_for_response(self):
        peer, _ = self.peer()
        self.settings["timeoutSeconds"] = 1
        peer.reply(self.translation("too late"), delay=2)
        timed_out = self.call("translate", text="Hello")
        self.assertNotEqual(timed_out["error"], "")
        self.assertEqual(timed_out["text"], "")
        peer.received()
        self.settings["timeoutSeconds"] = 30
        peer.reply(self.translation("cancelled late result"), delay=3)
        process = subprocess.Popen([DRIVER], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, encoding="utf-8",
                                   env=isolated_environment())
        self.addCleanup(lambda: process.poll() is None and process.kill())
        process.stdin.write(json.dumps({"operation": "translate", "settings": self.settings,
                                       "text": "Hello", "waitForCancel": True}) + "\n")
        process.stdin.flush()
        peer.received()  # Cancellation happens only after the HTTP request reaches the peer.
        start = time.monotonic()
        process.stdin.write("cancel\n")
        process.stdin.flush()
        stdout, stderr = process.communicate(timeout=2)
        self.assertEqual(process.returncode, 0, stderr)
        self.assertLess(time.monotonic() - start, 2)
        result = json.loads(stdout)
        self.assertNotEqual(result["error"], "")
        self.assertEqual(result["text"], "")
        self.assertNotIn("cancelled late result", json.dumps(result))

    def test_ocr_rejects_invalid_images_before_sending_credentials(self):
        self.settings.update(ocrApiKey="ocr-private-key", ocrSecretKey="ocr-private-secret")
        # Both are rejected before the hard-coded HTTPS Baidu endpoint is used.
        invalid = self.call("recognize", png=[1, 2, 3])
        self.assertNotEqual(invalid["error"], "")
        png = bytearray(b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR")
        png += (14).to_bytes(4, "big") + (40).to_bytes(4, "big") + bytes(9)
        too_small = self.call("recognize", png=list(png))
        self.assertNotEqual(too_small["error"], "")
        self.assertEqual(too_small["text"], "")
        for output in [invalid, too_small]:
            self.assertNotIn("ocr-private", output["error"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", required=True)
    arguments, remaining = parser.parse_known_args()
    DRIVER = str(Path(arguments.driver).resolve())
    unittest.main(argv=[sys.argv[0], *remaining])
