import json
import os
import tempfile
import unittest

from codey.discover import (count_subagents, discover_claude_sessions, encode_cwd,
                            pick_latest_transcript)


class TestDiscover(unittest.TestCase):
    def test_pick_latest(self):
        files = [
            {"sid": "old", "mtime_ms": 1000, "path": "/p/old.jsonl"},
            {"sid": "new", "mtime_ms": 5000, "path": "/p/new.jsonl"},
            {"sid": "claimed", "mtime_ms": 9000, "path": "/p/claimed.jsonl"},
        ]
        self.assertEqual(pick_latest_transcript(files, 2000, {"claimed"})["sid"], "new")

    def test_discover_in_tmp(self):
        with tempfile.TemporaryDirectory() as home:
            claude = os.path.join(home, ".claude")
            sessions = os.path.join(claude, "sessions")
            cwd = "/Users/zyc/code/Demo"
            proj = os.path.join(claude, "projects", encode_cwd(cwd))
            os.makedirs(sessions)
            os.makedirs(proj)
            pid = os.getpid()
            with open(os.path.join(sessions, f"{pid}.json"), "w") as fh:
                json.dump({"pid": pid, "sessionId": "sid-demo", "cwd": cwd, "startedAt": 1000}, fh)
            with open(os.path.join(proj, "sid-demo.jsonl"), "w") as fh:
                fh.write(json.dumps({"type": "assistant", "timestamp": "2026-06-03T10:00:00Z",
                    "message": {"model": "claude-opus-4-8",
                        "usage": {"input_tokens": 50, "output_tokens": 10,
                                  "cache_read_input_tokens": 0, "cache_creation_input_tokens": 0},
                        "content": [{"type": "text", "text": "hi"}]}}))
            out = discover_claude_sessions([claude], {pid})
            self.assertEqual(len(out), 1)
            self.assertEqual(out[0]["session_id"], "sid-demo")
            self.assertEqual(out[0]["cwd"], cwd)
            self.assertTrue(out[0]["transcript_path"].endswith("sid-demo.jsonl"))

    def test_count_subagents(self):
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, "a.meta.json"), "w").close()
            open(os.path.join(d, "b.meta.json"), "w").close()
            open(os.path.join(d, "note.txt"), "w").close()
            self.assertEqual(count_subagents(d), 2)
            self.assertEqual(count_subagents("/no/such/dir"), 0)


if __name__ == "__main__":
    unittest.main()
