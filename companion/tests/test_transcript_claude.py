import json
import unittest

from codey.transcript_claude import parse_claude_transcript


def A(usage, content, model="claude-opus-4-8", ts="2026-06-03T10:00:00.000Z"):
    return json.dumps({"type": "assistant", "timestamp": ts,
                       "message": {"model": model, "usage": usage, "content": content}})


def U(content, **extra):
    e = {"type": "user", "timestamp": "2026-06-03T10:00:05.000Z",
         "version": "1.2.3", "gitBranch": "main", "message": {"content": content}}
    e.update(extra)
    return json.dumps(e)


class TestTranscriptClaude(unittest.TestCase):
    def test_totals_turn_model(self):
        text = "\n".join([
            U("帮我改 server.js"),
            A({"input_tokens": 100, "output_tokens": 20, "cache_read_input_tokens": 0,
               "cache_creation_input_tokens": 500}, [{"type": "text", "text": "好的"}]),
            U([{"type": "tool_result", "content": "done"}]),
            A({"input_tokens": 80, "output_tokens": 10, "cache_read_input_tokens": 600,
               "cache_creation_input_tokens": 0},
              [{"type": "tool_use", "name": "Edit",
                "input": {"file_path": "/Users/zyc/code/Codey/companion/server.js"}}]),
        ])
        r = parse_claude_transcript(text)
        self.assertEqual(r["turn_count"], 2)
        self.assertEqual(r["model"], "claude-opus-4-8")
        self.assertEqual(r["total_input"], 180)
        self.assertEqual(r["total_output"], 30)
        self.assertEqual(r["total_cache_read"], 600)
        self.assertEqual(r["total_cache_create"], 500)
        self.assertEqual(r["version"], "1.2.3")
        self.assertEqual(r["git_branch"], "main")
        self.assertEqual(r["first_prompt"], "帮我改 server.js")

    def test_current_task(self):
        text = A({"input_tokens": 80, "output_tokens": 10, "cache_read_input_tokens": 600,
                  "cache_creation_input_tokens": 0},
                 [{"type": "tool_use", "name": "Edit",
                   "input": {"file_path": "/Users/zyc/code/Codey/companion/server.js"}}])
        r = parse_claude_transcript(text)
        self.assertEqual(r["current_task"], "Edit companion/server.js")
        self.assertTrue(r["pending_tool"])

    def test_last_context_tokens(self):
        fresh = parse_claude_transcript(A(
            {"input_tokens": 100, "output_tokens": 0, "cache_read_input_tokens": 0,
             "cache_creation_input_tokens": 500}, [{"type": "text", "text": "x"}]))
        self.assertEqual(fresh["last_context_tokens"], 600)
        warm = parse_claude_transcript(A(
            {"input_tokens": 80, "output_tokens": 0, "cache_read_input_tokens": 600,
             "cache_creation_input_tokens": 0}, [{"type": "text", "text": "x"}]))
        self.assertEqual(warm["last_context_tokens"], 680)

    def test_model_generating_and_bad_lines(self):
        text = "\n".join([
            "this is not json",
            A({"input_tokens": 10, "output_tokens": 5, "cache_read_input_tokens": 0,
               "cache_creation_input_tokens": 0}, [{"type": "text", "text": "hi"}]),
            U("再帮我看一下"),
        ])
        r = parse_claude_transcript(text)
        self.assertGreater(r["last_user_ts_ms"], 0)
        self.assertFalse(r["pending_tool"])

    def test_synthetic_tail_not_generating(self):
        text = "\n".join([
            A({"input_tokens": 10, "output_tokens": 5, "cache_read_input_tokens": 0,
               "cache_creation_input_tokens": 0},
              [{"type": "tool_use", "name": "Bash", "input": {"command": "npm test\n更多"}}]),
            U([{"type": "tool_result", "content": "ok"}]),
        ])
        r = parse_claude_transcript(text)
        self.assertEqual(r["last_user_ts_ms"], 0)


    def test_compaction_count(self):
        # 三个 assistant turn:94k -> 120k(涨,不算)-> 40k(掉 >30%,算 1 次)
        def asst(ctx_in):
            return A({"input_tokens": ctx_in, "output_tokens": 1,
                      "cache_read_input_tokens": 0, "cache_creation_input_tokens": 0},
                     [{"type": "text", "text": "ok"}])
        text = "\n".join([asst(94000), asst(120000), asst(40000)])
        r = parse_claude_transcript(text)
        self.assertEqual(r["compactions"], 1)

    def test_compaction_none_when_growing(self):
        def asst(ctx_in):
            return A({"input_tokens": ctx_in, "output_tokens": 1,
                      "cache_read_input_tokens": 0, "cache_creation_input_tokens": 0},
                     [{"type": "text", "text": "ok"}])
        r = parse_claude_transcript("\n".join([asst(1000), asst(2000), asst(3000)]))
        self.assertEqual(r["compactions"], 0)

    def test_compaction_boundary_exact_30pct(self):
        # 1000 -> 700 是恰好 30% 跌幅:不计(条件是严格 >30%)
        def asst(ctx_in):
            return A({"input_tokens": ctx_in, "output_tokens": 1,
                      "cache_read_input_tokens": 0, "cache_creation_input_tokens": 0},
                     [{"type": "text", "text": "ok"}])
        r = parse_claude_transcript("\n".join([asst(1000), asst(700)]))
        self.assertEqual(r["compactions"], 0)


if __name__ == "__main__":
    unittest.main()
