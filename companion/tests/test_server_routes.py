import json, os, tempfile, unittest
from codey import server
from codey import config as cfg
from codey import usb_frames as uf
from codey import usb_link


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

    # --- /device/config, /device/wifi(路径 B:USB 直连配置)---

    def test_device_config_503_when_usb_offline(self):
        orig = usb_link.send_config_request
        usb_link.send_config_request = lambda *a, **k: None
        try:
            code, resp = server.device_config_payload()
        finally:
            usb_link.send_config_request = orig
        self.assertEqual(code, 503)
        self.assertFalse(resp["ok"])

    def test_device_config_passes_through_device_json(self):
        orig = usb_link.send_config_request
        usb_link.send_config_request = lambda ftype, payload, **k: (
            uf.CFG_STATE, b'{"ssid":"home","wifi":true,"ip":"192.168.1.5","bright":200,"history":[]}')
        try:
            code, resp = server.device_config_payload()
        finally:
            usb_link.send_config_request = orig
        self.assertEqual(code, 200)
        self.assertEqual(resp["ssid"], "home")
        self.assertEqual(resp["ip"], "192.168.1.5")

    def test_device_wifi_post_requires_ssid(self):
        body = json.dumps({"pass": "x"}).encode()
        code, resp = server.device_wifi_post(body, len(body))
        self.assertEqual(code, 400)
        self.assertIn("error", resp)

    def test_device_wifi_post_bad_json_400(self):
        code, resp = server.device_wifi_post(b"{not json", 9)
        self.assertEqual(code, 400)

    def test_device_wifi_post_too_large_400(self):
        code, resp = server.device_wifi_post(b"{}", server.MAX_DEVICE_WIFI_BYTES + 1)
        self.assertEqual(code, 400)

    def test_device_wifi_post_503_when_usb_offline(self):
        orig = usb_link.send_config_request
        usb_link.send_config_request = lambda *a, **k: None
        try:
            body = json.dumps({"ssid": "home", "pass": "secret"}).encode()
            code, resp = server.device_wifi_post(body, len(body))
        finally:
            usb_link.send_config_request = orig
        self.assertEqual(code, 503)
        self.assertFalse(resp["ok"])

    def test_device_wifi_post_forwards_ssid_and_pass_to_device(self):
        seen = {}

        def fake_send(ftype, payload, **k):
            seen["ftype"] = ftype
            seen["payload"] = json.loads(payload.decode("utf-8"))
            return (uf.CFG_ACK, b'{"ok":true,"ssid":"home","message":"connected"}')

        orig = usb_link.send_config_request
        usb_link.send_config_request = fake_send
        try:
            body = json.dumps({"ssid": "home", "pass": "secret"}).encode()
            code, resp = server.device_wifi_post(body, len(body))
        finally:
            usb_link.send_config_request = orig
        self.assertEqual(code, 200)
        self.assertTrue(resp["ok"])
        self.assertEqual(seen["ftype"], uf.CFG_SET)
        self.assertEqual(seen["payload"], {"wifi_ssid": "home", "wifi_pass": "secret"})

    def test_admin_html_has_device_config_panel(self):
        # 回归守卫:锁住设备配置 tab 与关键控件,防未来误删/改错
        body, ctype = server.read_static(os.path.join(server.WEB_DIR, "admin.html"))
        self.assertIsNotNone(body)
        text = body.decode("utf-8")
        self.assertIn('data-tab="device"', text)
        self.assertIn('id="devSsid"', text)
        self.assertIn('id="devSave"', text)
        self.assertIn("/device/config", text)
        self.assertIn("/device/wifi", text)

    # --- 采集重构:_collect_once / start_collectors ---

    def test_collect_once_updates_cache(self):
        from codey import collect
        app = server.App()
        fake = {"claude": [{"tokens_total": 1000}], "codex": []}
        orig = collect.collect_sessions
        collect.collect_sessions = lambda: fake
        try:
            app._collect_once()
        finally:
            collect.collect_sessions = orig
        self.assertEqual(app.session_cache, fake)
        self.assertEqual(app.tok_rate["claude"]["val"], 0)          # 首次:prev=None → 0
        self.assertIsNotNone(app.tok_rate["claude"]["prev"])         # prev 已记录
        self.assertEqual(app.tok_rate["claude"]["prev"]["tokens"], 1000)

    def test_start_collectors_returns_live_daemon_thread(self):
        from codey import collect
        app = server.App()
        orig = collect.collect_sessions
        collect.collect_sessions = lambda: {"claude": [], "codex": []}   # 让后台线程做轻活
        try:
            t = app.start_collectors()
            self.assertTrue(t.is_alive())
            self.assertTrue(t.daemon)
        finally:
            collect.collect_sessions = orig
