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
