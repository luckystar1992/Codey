import unittest
from unittest import mock

from codey import state as state_mod


class TestState(unittest.TestCase):
    def test_build_state_shape(self):
        cache = {
            "claude": [{"id": "c1", "status": "executing", "context_pct": 47,
                        "tokens_total": 1000, "model": "Opus 4.8",
                        "git": {"added": 1, "modified": 0}}],
            "codex": [],
        }
        tok = {"claude": {"prev": None, "val": 1234}, "codex": {"prev": None, "val": 0}}
        with mock.patch.object(state_mod, "read_real_usage", return_value=None), \
             mock.patch.object(state_mod, "read_codex_usage", return_value=None):
            st = state_mod.build_state(cache, tok)

        self.assertEqual([p["id"] for p in st["providers"]], ["claude", "codex"])
        claude = st["providers"][0]
        self.assertEqual(len(claude["sessions"]), 1)
        self.assertEqual(claude["active_count"], 1)
        self.assertEqual(claude["agg"]["tokens_per_min"], 1234)
        self.assertEqual(claude["agg"]["dirty_repos"], 1)
        self.assertIn("session", claude)
        self.assertIn("weekly", claude)
        self.assertEqual(st["providers"][1]["sessions"], [])
        self.assertIn("ts", st)
        self.assertIn("battery", st)

    def test_sort_active_first(self):
        cache = {"claude": [
            {"id": "a", "status": "waiting", "context_pct": 5, "tokens_total": 0, "git": {"added": 0, "modified": 0}, "model": ""},
            {"id": "b", "status": "executing", "context_pct": 10, "tokens_total": 0, "git": {"added": 0, "modified": 0}, "model": ""},
            {"id": "c", "status": "thinking", "context_pct": 80, "tokens_total": 0, "git": {"added": 0, "modified": 0}, "model": ""},
        ], "codex": []}
        tok = {"claude": {"prev": None, "val": 0}, "codex": {"prev": None, "val": 0}}
        with mock.patch.object(state_mod, "read_real_usage", return_value=None), \
             mock.patch.object(state_mod, "read_codex_usage", return_value=None):
            st = state_mod.build_state(cache, tok)
        self.assertEqual([s["id"] for s in st["providers"][0]["sessions"]], ["b", "c", "a"])


if __name__ == "__main__":
    unittest.main()
