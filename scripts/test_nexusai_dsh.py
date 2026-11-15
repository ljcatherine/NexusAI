from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
import os
from pathlib import Path
import stat
import tempfile
import threading
import unittest
from unittest import mock

MODULE_PATH = Path(__file__).with_name("nexusai-dsh.py")
SPEC = importlib.util.spec_from_file_location("nexusai_dsh", MODULE_PATH)
dsh = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dsh)

LISTING = {
    "object": "list",
    "data": [
        {"id": "/data/PS5LM/models/Example-27B-Q2_0.gguf", "meta": {"n_ctx": 65536}},
        {"id": "small", "meta": {}},
    ],
}


class _Handler(BaseHTTPRequestHandler):
    def log_message(self, _format, *_args):
        pass

    def do_GET(self):
        if self.path != "/v1/models":
            self.send_error(404)
            return
        body = json.dumps(LISTING).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class UrlRuleTest(unittest.TestCase):
    def test_private_http_and_https_are_allowed(self):
        self.assertEqual(dsh.validate_base_url("http://192.168.1.50:8081"), "http://192.168.1.50:8081/v1")
        self.assertEqual(dsh.validate_base_url("http://127.0.0.1:8081/v1/"), "http://127.0.0.1:8081/v1")
        self.assertEqual(dsh.validate_base_url("http://100.101.1.2:8081/v1"), "http://100.101.1.2:8081/v1")
        self.assertEqual(dsh.validate_base_url("https://ai.example.com/v1"), "https://ai.example.com/v1")

    def test_public_cleartext_and_extras_are_refused(self):
        for url in ("http://8.8.8.8:8081/v1", "http://example.com/v1", "ftp://10.0.0.1/v1",
                    "http://user:pw@10.0.0.1/v1", "http://10.0.0.1/v1?x=1", "10.0.0.1:8081"):
            with self.subTest(url=url), self.assertRaises(dsh.SetupError):
                dsh.validate_base_url(url)


class PatchTest(unittest.TestCase):
    def test_models_take_context_from_server_and_fall_back(self):
        models = dsh.models_from_listing(LISTING)
        self.assertEqual(models[0]["name"], "Example-27B-Q2_0")
        self.assertEqual(models[0]["contextWindow"], 65536)
        self.assertEqual(models[0]["maxTokens"], dsh.DEFAULT_MAX_TOKENS)
        self.assertEqual(models[1]["contextWindow"], dsh.DEFAULT_CONTEXT)

    def test_no_models_is_an_error(self):
        with self.assertRaises(dsh.SetupError):
            dsh.models_from_listing({"data": []})

    def test_patch_declares_route_and_default_model(self):
        text = dsh.patch_yaml("http://10.0.0.5:8081/v1", dsh.models_from_listing(LISTING))
        self.assertIn("- id: llm-pi-ai", text)
        self.assertIn("      nexusai:", text)
        self.assertIn("        apiKeyEnv: NEXUSAI_API_KEY", text)
        self.assertIn("        api: openai-completions", text)
        self.assertIn('        baseURL: "http://10.0.0.5:8081/v1"', text)
        self.assertIn("- id: agent-default-model", text)
        self.assertIn('    model: "/data/PS5LM/models/Example-27B-Q2_0.gguf"', text)
        self.assertNotIn(dsh.API_KEY_PLACEHOLDER, text)

    def test_patch_quotes_hostile_model_ids(self):
        text = dsh.patch_yaml("http://10.0.0.5:8081/v1", [
            {"id": 'evil"\n- id: x', "name": "n", "contextWindow": 1, "maxTokens": 1}])
        self.assertIn(r'id: "evil\"\n- id: x"', text)
        self.assertNotIn("\n- id: x", text)

    def test_unknown_default_model_is_refused(self):
        with self.assertRaises(dsh.SetupError):
            dsh.patch_yaml("http://10.0.0.5:8081/v1", dsh.models_from_listing(LISTING), "missing")


class EndToEndTest(unittest.TestCase):
    def setUp(self):
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def test_writes_owner_only_overlay_from_live_listing(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "sub" / "nexusai.patch.yml"
            self.assertEqual(dsh.main(["--base-url", self.base, "--out", str(out), "--model", "small"]), 0)
            self.assertEqual(stat.S_IMODE(out.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(out.parent.stat().st_mode), 0o700)
            text = out.read_text()
            self.assertIn(f'baseURL: "{self.base}/v1"', text)
            self.assertIn('    model: "small"', text)

    def test_smoke_sets_placeholder_key_and_reads_final_event(self):
        with tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp) / "dsh"
            fake.write_text("#!/bin/sh\n"
                            "test \"$NEXUSAI_API_KEY\" = nexusai-no-auth || exit 3\n"
                            "echo '{\"type\":\"session\",\"sessionId\":\"s\"}'\n"
                            "echo '{\"type\":\"final\",\"reason\":{\"kind\":\"completed\"},\"text\":\"OK\"}'\n")
            fake.chmod(0o700)
            env = {k: v for k, v in os.environ.items() if k != "NEXUSAI_API_KEY"}
            with mock.patch.dict(os.environ, env, clear=True):
                self.assertEqual(dsh.smoke(str(fake), Path(tmp) / "p.yml", 30), "OK")

    def test_smoke_reports_dsh_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp) / "dsh"
            fake.write_text("#!/bin/sh\necho '{\"type\":\"final\",\"reason\":{\"kind\":\"error\","
                            "\"error\":{\"message\":\"No API key\"}},\"text\":\"\"}'\nexit 1\n")
            fake.chmod(0o700)
            with self.assertRaisesRegex(dsh.SetupError, "No API key"):
                dsh.smoke(str(fake), Path(tmp) / "p.yml", 30)


if __name__ == "__main__":
    unittest.main()
