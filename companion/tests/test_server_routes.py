import json, os, tempfile, unittest
from codey import server
from codey import config as cfg


class TestServerRoutes(unittest.TestCase):
    def setUp(self):
        # 隔离每个测试到独立的 config.json,避免污染真实 companion/data/config.json
        self._dir = tempfile.mkdtemp()
        self._prev_cfg = cfg.CONFIG_PATH
        cfg.CONFIG_PATH = os.path.join(self._dir, "config.json")

    def tearDown(self):
        cfg.CONFIG_PATH = self._prev_cfg

    def test_state_includes_asr_url_default_empty(self):
        app = server.App()
        st = app.state()
        self.assertIn("asr_url", st)
        self.assertEqual(st["asr_url"], "")

    def test_state_includes_display_columns(self):
        app = server.App()
        st = app.state()
        self.assertIn("display", st)
        self.assertIn("columns", st["display"])
        self.assertTrue(st["display"]["columns"]["status"])

    # --- /codey/config ---

    def test_config_get_masks_secret_and_has_flag(self):
        cfg.save({"doubao_api_key": "sk-secret-123", "asr_engine": "sherpa"})
        payload = server.config_get_payload()
        self.assertIn("values", payload)
        self.assertIn("schema", payload)
        self.assertEqual(payload["values"]["doubao_api_key"], "")        # 遮罩
        self.assertTrue(payload["values"]["has_doubao_api_key"])          # 已设置
        self.assertEqual(payload["values"]["asr_engine"], "sherpa")       # 非密钥明文
        self.assertIn("asr_engine", payload["schema"])
        self.assertEqual(payload["schema"]["asr_engine"]["type"], "select")

    def test_config_get_has_flag_false_when_unset(self):
        payload = server.config_get_payload()
        self.assertEqual(payload["values"]["doubao_api_key"], "")
        self.assertFalse(payload["values"]["has_doubao_api_key"])

    def test_config_post_persists_valid(self):
        body = json.dumps({"asr_engine": "sherpa"}).encode()
        code, resp = server.config_post(body, len(body))
        self.assertEqual(code, 200)
        self.assertTrue(resp["ok"])
        self.assertEqual(cfg.get("asr_engine"), "sherpa")
        self.assertEqual(resp["values"]["asr_engine"], "sherpa")

    def test_config_post_drops_bad_engine(self):
        body = json.dumps({"asr_engine": "bogus"}).encode()
        code, resp = server.config_post(body, len(body))
        self.assertEqual(code, 200)
        self.assertEqual(cfg.get("asr_engine"), "auto")                  # 非法被丢弃

    def test_config_post_empty_secret_keeps_existing(self):
        cfg.save({"doubao_api_key": "sk-keep-me"})
        body = json.dumps({"doubao_api_key": "", "asr_engine": "doubao"}).encode()
        code, resp = server.config_post(body, len(body))
        self.assertEqual(code, 200)
        self.assertEqual(cfg.get("doubao_api_key"), "sk-keep-me")        # 空密钥=不动
        self.assertEqual(cfg.get("asr_engine"), "doubao")
        self.assertEqual(resp["values"]["doubao_api_key"], "")           # 回包仍遮罩
        self.assertTrue(resp["values"]["has_doubao_api_key"])

    def test_config_post_too_large_400(self):
        code, resp = server.config_post(b"{}", server.MAX_CONFIG_BYTES + 1)
        self.assertEqual(code, 400)
        self.assertIn("error", resp)

    def test_safe_content_length_rejects_bad_values(self):
        # 负数/非数字/超限 -> None(do_POST 据此拒绝,防 rfile.read(-1) 内存 DoS)
        class H(dict):
            def get(self, k, d=None): return dict.get(self, k, d)
        self.assertIsNone(server.safe_content_length(H({"Content-Length": "-1"}), 100))
        self.assertIsNone(server.safe_content_length(H({"Content-Length": "abc"}), 100))
        self.assertIsNone(server.safe_content_length(H({"Content-Length": "101"}), 100))
        self.assertEqual(server.safe_content_length(H({"Content-Length": "50"}), 100), 50)
        self.assertEqual(server.safe_content_length(H({}), 100), 0)

    def test_config_post_bad_json_400(self):
        body = b"{not json"
        code, resp = server.config_post(body, len(body))
        self.assertEqual(code, 400)
        self.assertIn("error", resp)

    def test_mask_keeps_non_secret_and_blanks_secret(self):
        masked = server.mask_config({"doubao_api_key": "x", "doubao_app_id": "app", "paste": True})
        self.assertEqual(masked["doubao_api_key"], "")
        self.assertTrue(masked["has_doubao_api_key"])
        self.assertEqual(masked["doubao_app_id"], "app")                 # app_id 非密钥,明文
        self.assertEqual(masked["paste"], True)

    def test_parse_history_n(self):
        self.assertEqual(server.parse_history_n("/codey/history"), 100)
        self.assertEqual(server.parse_history_n("/codey/history?n=20"), 20)
        self.assertEqual(server.parse_history_n("/codey/history?n=abc"), 100)
        self.assertEqual(server.parse_history_n("/codey/history?n=99999"), server.HISTORY_MAX)

    def test_static_bytes_and_ctype(self):
        d = tempfile.mkdtemp()
        with open(os.path.join(d, "a.html"), "w") as f:
            f.write("<h1>hi</h1>")
        body, ctype = server.read_static(os.path.join(d, "a.html"))
        self.assertEqual(body, b"<h1>hi</h1>")
        self.assertEqual(ctype, "text/html; charset=utf-8")
        self.assertEqual(server.read_static(os.path.join(d, "missing.html")), (None, None))

    # --- /kindle ---

    def test_schema_and_values_include_kindle_refresh(self):
        payload = server.config_get_payload()
        self.assertIn("kindle_refresh_s", payload["schema"])
        self.assertEqual(payload["schema"]["kindle_refresh_s"]["type"], "int")
        self.assertEqual(payload["schema"]["kindle_refresh_s"]["min"], 5)
        self.assertEqual(payload["schema"]["kindle_refresh_s"]["max"], 3600)
        self.assertEqual(payload["values"]["kindle_refresh_s"], 30)   # config.all() 自动带上

    def test_kindle_route_returns_html(self):
        # 起真实 HTTPServer 服务一次请求,验证 do_GET 分支真的接上了
        import threading, urllib.request
        from http.server import HTTPServer
        app = server.App()                       # 不 start_background,不起后台线程
        httpd = HTTPServer(("127.0.0.1", 0), server.make_handler(app))
        port = httpd.server_address[1]
        t = threading.Thread(target=httpd.handle_request, daemon=True)
        t.start()
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/kindle" % port, timeout=5) as resp:
                self.assertEqual(resp.status, 200)
                self.assertIn("text/html", resp.headers.get("Content-Type", ""))
                body = resp.read().decode("utf-8")
        finally:
            t.join(timeout=5)
            httpd.server_close()
        self.assertIn("CODEY MONITOR", body)
        self.assertIn('http-equiv="refresh" content="30"', body)     # 默认 30s
        self.assertNotIn("<script", body)                             # 零 JS
