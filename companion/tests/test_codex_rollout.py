import json
import unittest

from codey.codex_rollout import parse_codex_rollout


class TestCodexRollout(unittest.TestCase):
    def test_full(self):
        text = "\n".join([
            json.dumps({"type": "session_meta", "payload": {
                "id": "abc", "cwd": "/Users/zyc/code/webapp", "cli_version": "0.9",
                "originator": "codex-cli", "git": {"branch": "dev"}}}),
            json.dumps({"type": "turn_context", "payload": {
                "model": "gpt-5.1-codex", "effort": "high", "model_context_window": 272000}}),
            json.dumps({"type": "event_msg", "payload": {"type": "user_message", "message": "加个接口"}}),
            json.dumps({"type": "response_item", "payload": {
                "type": "function_call", "name": "shell", "arguments": '{"command":"ls"}'}}),
            json.dumps({"type": "event_msg", "payload": {"type": "token_count",
                "info": {"total_token_usage": {"input_tokens": 1000, "cached_input_tokens": 600,
                                               "output_tokens": 200},
                         "last_token_usage": {"input_tokens": 800, "cached_input_tokens": 600}},
                "rate_limits": {
                    "primary": {"window_minutes": 300, "used_percent": 42, "resets_at": "2026-06-03T12:00:00Z"},
                    "secondary": {"window_minutes": 10080, "used_percent": 18, "resets_at": "2026-06-09T00:00:00Z"}}}}),
        ])
        r = parse_codex_rollout(text)
        self.assertEqual(r["session_id"], "abc")
        self.assertEqual(r["cwd"], "/Users/zyc/code/webapp")
        self.assertEqual(r["model"], "gpt-5.1-codex")
        self.assertEqual(r["effort"], "high")
        self.assertEqual(r["context_window"], 272000)
        self.assertEqual(r["git_branch"], "dev")
        self.assertEqual(r["first_prompt"], "加个接口")
        self.assertEqual(r["current_task"], "shell ls")
        self.assertEqual(r["total_input"], 400)
        self.assertEqual(r["total_cache_read"], 600)
        self.assertEqual(r["total_output"], 200)
        self.assertEqual(r["last_context_tokens"], 800)
        self.assertEqual(r["turn_count"], 1)
        self.assertEqual(r["five_hour_pct"], 42)
        self.assertEqual(r["weekly_pct"], 18)
        self.assertFalse(r["done"])

    def test_task_complete(self):
        text = json.dumps({"type": "event_msg", "payload": {"type": "task_complete"}})
        self.assertTrue(parse_codex_rollout(text)["done"])


if __name__ == "__main__":
    unittest.main()
