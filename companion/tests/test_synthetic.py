import unittest

from codey.synthetic import is_synthetic_user_message


class TestSynthetic(unittest.TestCase):
    def test_real_prompt_not_synthetic(self):
        self.assertFalse(is_synthetic_user_message(
            {"type": "user", "message": {"content": "帮我改 bug"}}))
        self.assertFalse(is_synthetic_user_message(
            {"type": "user", "message": {"content": [{"type": "text", "text": "hi"}]}}))

    def test_synthetic_variants(self):
        self.assertTrue(is_synthetic_user_message(
            {"type": "user", "isMeta": True, "message": {"content": "x"}}))
        self.assertTrue(is_synthetic_user_message(
            {"type": "user", "message": {"content": [{"type": "tool_result", "content": "ok"}]}}))
        self.assertTrue(is_synthetic_user_message(
            {"type": "user", "message": {"content": "<command-name>/clear</command-name>"}}))
        self.assertTrue(is_synthetic_user_message(
            {"type": "user", "message": {"content": [{"type": "text", "text": "<bash-input>ls</bash-input>"}]}}))


if __name__ == "__main__":
    unittest.main()
