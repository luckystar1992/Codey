import unittest

from codey.derive import derive_status


class TestDerive(unittest.TestCase):
    def test_priority(self):
        self.assertEqual(derive_status(done=True), "done")
        self.assertEqual(derive_status(has_active_descendant=True), "executing")
        self.assertEqual(derive_status(pending_tool=True), "executing")
        self.assertEqual(derive_status(model_generating=True), "thinking")
        self.assertEqual(derive_status(), "waiting")
        self.assertEqual(derive_status(pending_tool=True, model_generating=True), "executing")


if __name__ == "__main__":
    unittest.main()
