import unittest

from codey.collect import (aggregate_provider, has_active_descendant, ports_for_tree,
                          tokens_per_min)


class TestCollect(unittest.TestCase):
    def test_has_active_descendant(self):
        pmap = {100: {"pid": 100, "ppid": 1, "cpu": 0.0},
                200: {"pid": 200, "ppid": 100, "cpu": 7.0}}
        self.assertTrue(has_active_descendant(pmap, 100, 5))
        self.assertFalse(has_active_descendant(pmap, 100, 9))

    def test_ports_for_tree(self):
        pmap = {100: {"pid": 100, "ppid": 1, "cpu": 0},
                200: {"pid": 200, "ppid": 100, "cpu": 0}}
        ports = {100: [5173], 200: [3000, 5173]}
        self.assertEqual(ports_for_tree(pmap, ports, 100), [3000, 5173])
        self.assertEqual(ports_for_tree(pmap, ports, 999), [])

    def test_aggregate_provider(self):
        sessions = [
            {"status": "executing", "git": {"added": 1, "modified": 2}, "tokens_total": 1000},
            {"status": "waiting", "git": {"added": 0, "modified": 0}, "tokens_total": 500},
            {"status": "thinking", "git": {"added": 0, "modified": 3}, "tokens_total": 700},
        ]
        agg = aggregate_provider(sessions)
        self.assertEqual(agg["active_count"], 2)
        self.assertEqual(agg["dirty_repos"], 2)
        self.assertEqual(agg["tokens_total"], 2200)

    def test_tokens_per_min(self):
        self.assertEqual(tokens_per_min({"tokens": 100000, "at": 0}, {"tokens": 130000, "at": 60000}), 30000)
        self.assertEqual(tokens_per_min({"tokens": 100, "at": 0}, {"tokens": 50, "at": 60000}), 0)
        self.assertEqual(tokens_per_min(None, {"tokens": 100, "at": 60000}), 0)
        self.assertEqual(tokens_per_min({"tokens": 100, "at": 0}, None), 0)
        self.assertEqual(tokens_per_min({"tokens": 100, "at": 5000}, {"tokens": 200, "at": 5000}), 0)


if __name__ == "__main__":
    unittest.main()
