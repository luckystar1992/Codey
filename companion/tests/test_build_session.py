import unittest

from codey.build_session import build_claude_session, build_codex_session


class TestBuildSession(unittest.TestCase):
    def test_claude(self):
        parsed = {
            "total_input": 180, "total_output": 30, "total_cache_read": 600, "total_cache_create": 500,
            "last_context_tokens": 94000, "max_context_tokens": 94000, "turn_count": 23,
            "model": "claude-opus-4-8", "current_task": "Edit companion/server.js",
            "git_branch": "main", "first_prompt": "帮我改", "last_user_ts_ms": 0, "pending_tool": True,
        }
        s = build_claude_session(
            session_id="sid1", cwd="/Users/zyc/code/Codey", started_at=1780000000000,
            parsed=parsed, status="executing", git={"branch": "main", "added": 3, "modified": 12},
            ports=[3000, 5173], subagents=2, effort="high", memory_kb=131072)
        self.assertEqual(s["id"], "sid1")
        self.assertEqual(s["memory"], 131072)
        self.assertEqual(s["name"], "Codey")
        self.assertEqual(s["status"], "executing")
        self.assertEqual(s["model"], "claude-opus-4-8")
        self.assertEqual(s["context_window"], 200000)
        self.assertEqual(s["context_tokens"], 94000)
        self.assertEqual(s["context_pct"], 47)
        self.assertEqual(s["tokens_total"], 180 + 30 + 600 + 500)
        self.assertEqual(s["turn"], 23)
        self.assertEqual(s["git"], {"branch": "main", "added": 3, "modified": 12})
        self.assertEqual(s["current_task"], "Edit companion/server.js")
        self.assertEqual(s["ports"], [3000, 5173])
        self.assertEqual(s["subagents"], 2)
        self.assertEqual(s["effort"], "high")
        self.assertEqual(s["started_at"], 1780000000)

    def test_codex(self):
        parsed = {
            "session_id": "cx1", "cwd": "/Users/zyc/code/webapp", "model": "gpt-5.1-codex",
            "effort": "high", "context_window": 272000, "git_branch": "dev", "current_task": "shell ls",
            "total_input": 400, "total_output": 200, "total_cache_read": 600,
            "last_context_tokens": 136000, "turn_count": 5,
        }
        s = build_codex_session(parsed=parsed, started_at=1780000000000, status="thinking",
                                git={"branch": "dev", "added": 0, "modified": 0}, ports=[], subagents=0)
        self.assertEqual(s["id"], "cx1")
        self.assertEqual(s["name"], "webapp")
        self.assertEqual(s["context_window"], 272000)
        self.assertEqual(s["context_pct"], 50)
        self.assertEqual(s["tokens_total"], 400 + 200 + 600)
        self.assertEqual(s["turn"], 5)


if __name__ == "__main__":
    unittest.main()
