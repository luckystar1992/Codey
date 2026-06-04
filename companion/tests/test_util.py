import unittest

from codey.util import clamp_pct, project_name, truncate


class TestUtil(unittest.TestCase):
    def test_clamp_pct(self):
        self.assertEqual(clamp_pct(-5), 0)
        self.assertEqual(clamp_pct(47.6), 48)
        self.assertEqual(clamp_pct(150), 100)
        self.assertEqual(clamp_pct("x"), 0)

    def test_truncate(self):
        self.assertEqual(truncate("abcdef", 4), "abc…")
        self.assertEqual(truncate("短", 4), "短")
        self.assertEqual(truncate("项目名称很长啊", 4), "项目名…")

    def test_project_name(self):
        self.assertEqual(project_name("/Users/zyc/code/Codey"), "Codey")
        self.assertEqual(project_name("/Users/zyc/code/Codey/"), "Codey")
        self.assertEqual(project_name(""), "")


if __name__ == "__main__":
    unittest.main()
