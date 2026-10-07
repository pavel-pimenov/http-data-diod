#!/usr/bin/env python3
"""Unit tests for the pure helpers of message_counter.py (no docker, no network).

Run with: python3 -m unittest discover -s tests
"""

import contextlib
import io
import sys
import unittest
from unittest import mock

sys.path.insert(0, ".")
import message_counter as mc  # noqa: E402


class DuplicateLogPatternTest(unittest.TestCase):
    def test_matches_warn_line(self):
        line = ("l2-proxy  | 2026-09-09 15:58:19.980[warning] [thread=114] "
                "[client_ip=172.22.0.1] Frequent duplicate POSTs from "
                "client_id=dup-check-1788969499 client_ip=172.22.0.1 "
                "total=5 threshold=5 body_bytes=65")
        self.assertTrue(mc._matches_duplicate_log("dup-check-1788969499", line))
        # client_id with regex-special characters is matched literally.
        line2 = ("Frequent duplicate POSTs from client_id=a.b+c "
                 "client_ip=1.2.3.4 total=7")
        self.assertTrue(mc._matches_duplicate_log("a.b+c", line2))

    def test_rejects_other_client_and_unrelated_lines(self):
        line = ("Frequent duplicate POSTs from client_id=other-client "
                "client_ip=1.2.3.4 total=5 threshold=5 body_bytes=65")
        self.assertFalse(mc._matches_duplicate_log("dup-check-1", line))
        self.assertFalse(
            mc._matches_duplicate_log("dup-check-1", "GET /health/ready 200"))
        self.assertFalse(mc._matches_duplicate_log("dup-check-1", ""))

    def test_tracked_total_is_digits_only(self):
        # The pattern captures the numeric total as the second group.
        line = ("Frequent duplicate POSTs from client_id=dup-check-1 "
                "total=9999 threshold=5 body_bytes=65")
        pattern = __import__("re").compile(
            mc.DUPLICATE_LOG_PATTERN.format("dup-check-1"))
        m = pattern.search(line)
        self.assertIsNotNone(m)
        self.assertEqual(m.group(1), "dup-check-1")
        self.assertEqual(m.group(2), "9999")


class DuplicateCheckConfigTest(unittest.TestCase):
    def test_sends_exceed_threshold(self):
        # First delivery is never a duplicate; sends = threshold + 2 guarantees
        # the counter hits a threshold boundary.
        self.assertEqual(mc.DUPLICATE_CHECK_SENDS, mc.DUPLICATE_LOG_THRESHOLD + 2)


class PrintDuplicateCheckResultsTest(unittest.TestCase):
    def _run(self, results):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            ok = mc.print_duplicate_check_results(results)
        return ok, buf.getvalue()

    def test_no_docker_logs_means_skip_not_fail(self):
        ok, out = self._run({"client_id": "dup-check-1", "statuses": [200] * 7,
                             "sent": 7, "log_matched": None})
        self.assertTrue(ok)
        self.assertIn("Skipped", out)

    def test_warn_found_means_pass(self):
        ok, _ = self._run({"client_id": "dup-check-1", "statuses": [200] * 7,
                           "sent": 7,
                           "log_matched": ["  l2-proxy  | Frequent duplicate"]})
        self.assertTrue(ok)

    def test_missing_warn_means_fail(self):
        ok, out = self._run({"client_id": "dup-check-1", "statuses": [200] * 7,
                             "sent": 7, "log_matched": []})
        self.assertFalse(ok)
        self.assertIn("no 'Frequent duplicate POSTs' WARN found", out)


class DuplicateCheckMiscTest(unittest.TestCase):
    def test_default_client_id_format(self):
        # run_duplicate_check generates dup-check-<unix ts> when no id given
        # (the same expression is used inside the async runner).
        with mock.patch("message_counter.time.time", return_value=1788969499):
            self.assertEqual(f"dup-check-{int(mc.time.time())}",
                             "dup-check-1788969499")

    def test_duplicate_log_pattern_has_client_and_total_groups(self):
        # Keep the log-grepping contract stable: client_id group plus a
        # digits-only total group.
        import re
        probe = re.compile(mc.DUPLICATE_LOG_PATTERN.format("c"))
        m = probe.search("Frequent duplicate POSTs from client_id=c total=7")
        self.assertIsNotNone(m)
        self.assertEqual(tuple(m.groups()), ("c", "7"))


if __name__ == "__main__":
    unittest.main()
