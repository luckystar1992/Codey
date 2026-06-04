import unittest

from codey.context_window import context_window_for


class TestContextWindow(unittest.TestCase):
    def test_standard(self):
        self.assertEqual(context_window_for("claude-opus-4-8", 0), 200000)
        self.assertEqual(context_window_for("sonnet", 150000), 200000)

    def test_one_million(self):
        self.assertEqual(context_window_for("sonnet[1m]", 0), 1000000)
        self.assertEqual(context_window_for("whatever", 250000), 1000000)


if __name__ == "__main__":
    unittest.main()
