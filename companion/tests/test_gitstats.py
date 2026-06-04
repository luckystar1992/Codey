import unittest

from codey.gitstats import parse_git_shortstat, read_git_stats


class TestGitStats(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(parse_git_shortstat(" 3 files changed, 12 insertions(+), 5 deletions(-)"),
                         {"added": 12, "modified": 5})
        self.assertEqual(parse_git_shortstat(" 1 file changed, 4 insertions(+)"),
                         {"added": 4, "modified": 0})
        self.assertEqual(parse_git_shortstat(""), {"added": 0, "modified": 0})

    def test_empty_cwd_no_leak(self):
        # 空 cwd 必须返回空,不得泄漏 companion 自身仓库
        self.assertEqual(read_git_stats(""), {"branch": "", "added": 0, "modified": 0})


if __name__ == "__main__":
    unittest.main()
