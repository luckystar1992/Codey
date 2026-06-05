import json, os, unittest, tempfile
from codey import asr_history


class TestAsrHistory(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile(suffix=".jsonl", delete=False)
        self.tmp.close()
        os.environ["CODEY_ASR_HISTORY"] = self.tmp.name

    def tearDown(self):
        os.environ.pop("CODEY_ASR_HISTORY", None)
        os.unlink(self.tmp.name)

    def test_append_then_recent_with_timecode(self):
        e = asr_history.append("你好世界", engine="sherpa", pasted=True, at_ms=1780000000000)
        self.assertEqual(e["text"], "你好世界")
        self.assertEqual(e["ts"], 1780000000000)
        self.assertIn("time", e)
        self.assertTrue(e["pasted"])
        rows = asr_history.recent(10)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["text"], "你好世界")

    def test_empty_text_skipped(self):
        self.assertIsNone(asr_history.append("   ", engine="sherpa"))
        self.assertEqual(asr_history.recent(10), [])

    def test_recent_returns_last_n_in_order(self):
        for i in range(5):
            asr_history.append(f"t{i}", at_ms=1780000000000 + i)
        rows = asr_history.recent(3)
        self.assertEqual([r["text"] for r in rows], ["t2", "t3", "t4"])
