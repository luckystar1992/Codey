import unittest

from codey.proctree import descendants_of, parse_lsof_listen, parse_ps

PS = """  PID  PPID    RSS %CPU COMM
    1     0   1000  0.0 /sbin/launchd
  100     1  50000  7.5 node
  200   100  20000  0.1 npm
  300   200  10000  0.0 esbuild
  400     1   5000  0.0 other"""

LSOF = """COMMAND   PID USER   FD   TYPE             DEVICE SIZE/OFF NODE NAME
node      300 zyc   25u  IPv4 0x1234      0t0  TCP *:3000 (LISTEN)
node      300 zyc   26u  IPv6 0x5678      0t0  TCP [::1]:5173 (LISTEN)
node      999 zyc   27u  IPv4 0x9999      0t0  TCP 127.0.0.1:8787 (LISTEN)"""


class TestProcTree(unittest.TestCase):
    def test_parse_ps(self):
        m = parse_ps(PS)
        self.assertEqual(m[100]["ppid"], 1)
        self.assertEqual(m[100]["cpu"], 7.5)
        self.assertEqual(m[100]["rss_kb"], 50000)
        self.assertEqual(m[100]["comm"], "node")

    def test_descendants(self):
        m = parse_ps(PS)
        self.assertEqual(sorted(descendants_of(m, 100)), [200, 300])
        self.assertEqual(descendants_of(m, 400), [])

    def test_lsof(self):
        p = parse_lsof_listen(LSOF)
        self.assertEqual(sorted(p[300]), [3000, 5173])
        self.assertEqual(p[999], [8787])


if __name__ == "__main__":
    unittest.main()
