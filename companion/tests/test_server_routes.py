import os, tempfile, unittest
from codey import server


class TestServerRoutes(unittest.TestCase):
    def test_state_includes_asr_url_default_empty(self):
        app = server.App()
        st = app.state()
        self.assertIn("asr_url", st)
        self.assertEqual(st["asr_url"], "")

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
