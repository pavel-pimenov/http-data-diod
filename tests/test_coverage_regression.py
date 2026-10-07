#!/usr/bin/env python3
"""Unit tests for scripts/coverage-regression-check.py.

No docker, no network: gcovr JSON reports are built from strings and fed to
the checker through temp files.

Run with: python3 -m unittest discover -s tests
"""

import contextlib
import importlib.util
import io
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, ".")
REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CHECKER_PATH = REPO_ROOT / "scripts" / "coverage-regression-check.py"


def load_checker():
    spec = importlib.util.spec_from_file_location("coverage_regression_check",
                                                  CHECKER_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


crc = load_checker()


def line(count, noncode=False, branches=None):
    entry = {"count": count, "gcovr/noncode": noncode}
    if branches is not None:
        entry["branches"] = [{"count": c} for c in branches]
    return entry


def report(files):
    """files: {name: [line entries]} -> gcovr --json document."""
    return {"files": [{"file": name, "lines": lines}
                      for name, lines in files.items()]}


def stat(lines, covered, branches=0, branches_covered=0):
    return {"lines": lines, "covered": covered, "branches": branches,
            "branches_covered": branches_covered}


class SummarizeTest(unittest.TestCase):
    def test_counts_covered_lines_and_branches(self):
        entry = {"lines": [line(1, branches=[1, 0]),
                           line(0, branches=[0, 0]),
                           line(1, noncode=True, branches=[1])]}
        got = crc.summarize_file(entry)
        self.assertEqual(got["lines"], 2)
        self.assertEqual(got["covered"], 1)
        self.assertEqual(got["branches"], 4)
        self.assertEqual(got["branches_covered"], 1)

    def test_load_report_indexes_by_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "cov.json"
            path.write_text(json.dumps(report({
                "app_context.cpp": [line(1), line(0)],
                "test_proxy_handlers.cpp": [line(1)],
            })), encoding="utf-8")
            loaded = crc.load_report(path)
        self.assertEqual(loaded["app_context.cpp"]["lines"], 2)
        self.assertEqual(loaded["app_context.cpp"]["covered"], 1)


class CompareTest(unittest.TestCase):
    def baseline(self):
        return {"app_context.cpp": stat(100, 95),
                "request_handler.cpp": stat(400, 300),
                "gone.cpp": stat(50, 50)}

    def test_detects_regression(self):
        current = dict(self.baseline())
        current["app_context.cpp"] = stat(100, 90)
        current.pop("gone.cpp")
        regressions, new_files, removed = crc.compare(self.baseline(), current, 0.0)
        self.assertEqual([r[0] for r in regressions], ["app_context.cpp"])
        self.assertAlmostEqual(regressions[0][3], 5.0)
        self.assertEqual(new_files, [])
        self.assertEqual(removed, ["gone.cpp"])

    def test_new_lines_without_coverage_are_a_regression(self):
        current = dict(self.baseline())
        current["app_context.cpp"] = stat(110, 95)
        regressions, _, _ = crc.compare(self.baseline(), current, 0.0)
        self.assertEqual(len(regressions), 1)

    def test_same_coverage_and_improvement_pass(self):
        current = dict(self.baseline())
        current["app_context.cpp"] = stat(110, 105)
        regressions, new_files, _ = crc.compare(self.baseline(), current, 0.0)
        self.assertEqual(regressions, [])
        self.assertEqual(new_files, [])

    def test_max_drop_tolerates_small_drop(self):
        current = dict(self.baseline())
        current["app_context.cpp"] = stat(100, 94)
        regressions = crc.compare(self.baseline(), current, 0.0)[0]
        self.assertEqual([r[0] for r in regressions], ["app_context.cpp"])
        self.assertEqual(crc.compare(self.baseline(), current, 1.0)[0], [])
        regressions = crc.compare(self.baseline(), current, 0.5)[0]
        self.assertEqual([r[0] for r in regressions], ["app_context.cpp"])

    def test_new_production_file_is_reported(self):
        current = dict(self.baseline())
        current["new_feature.cpp"] = stat(10, 10)
        _, new_files, _ = crc.compare(self.baseline(), current, 0.0)
        self.assertEqual(new_files, ["new_feature.cpp"])

    def test_test_files_are_ignored_in_both_directions(self):
        current = dict(self.baseline())
        current["test_regression.cpp"] = stat(100, 10)
        current["test_new.cpp"] = stat(50, 50)
        regressions, new_files, _ = crc.compare(self.baseline(), current, 0.0)
        self.assertEqual(regressions, [])
        self.assertEqual(new_files, [])

    def test_is_test_file(self):
        self.assertTrue(crc.is_test_file("test_l2_worker.cpp"))
        self.assertTrue(crc.is_test_file("sub/test_helpers.cpp"))
        self.assertFalse(crc.is_test_file("l2_worker.cpp"))


class WriteBaselineTest(unittest.TestCase):
    def test_roundtrip_and_skips_test_files(self):
        files = {"request_handler.cpp": stat(10, 8),
                 "test_x.cpp": stat(10, 10)}
        production = {name: value for name, value in files.items()
                      if not crc.is_test_file(name)}
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "nested" / "coverage-baseline.json"
            crc.write_baseline(path, production)
            loaded = crc.load_baseline(path)
            raw = json.loads(path.read_text(encoding="utf-8"))
        self.assertIn("comment", raw)
        self.assertEqual(list(loaded), ["request_handler.cpp"])
        self.assertEqual(loaded["request_handler.cpp"]["lines"], 10)

    def test_missing_baseline_is_empty(self):
        self.assertEqual(crc.load_baseline(pathlib.Path("/nonexistent/x.json")), {})


class MainTest(unittest.TestCase):
    def write_report(self, path, files):
        path.write_text(json.dumps(report(files)), encoding="utf-8")

    @staticmethod
    def run_main(args):
        with contextlib.redirect_stdout(io.StringIO()):
            return crc.main(args)

    def test_update_then_check_passes_then_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            report_path = pathlib.Path(tmp) / "cov.json"
            baseline_path = pathlib.Path(tmp) / "baseline.json"
            good = {"app_context.cpp": [line(1), line(1), line(0)]}
            self.write_report(report_path, good)

            rc = self.run_main(["--report", str(report_path),
                           "--baseline", str(baseline_path), "--update"])
            self.assertEqual(rc, 0)
            self.assertTrue(baseline_path.exists())

            rc = self.run_main(["--report", str(report_path),
                           "--baseline", str(baseline_path)])
            self.assertEqual(rc, 0)

            bad = {"app_context.cpp": [line(1), line(0), line(0)]}
            self.write_report(report_path, bad)
            rc = self.run_main(["--report", str(report_path),
                           "--baseline", str(baseline_path)])
            self.assertEqual(rc, 1)

    def test_new_file_without_baseline_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            report_path = pathlib.Path(tmp) / "cov.json"
            baseline_path = pathlib.Path(tmp) / "baseline.json"
            self.write_report(report_path, {"app_context.cpp": [line(1)]})
            self.run_main(["--report", str(report_path),
                      "--baseline", str(baseline_path), "--update"])
            self.write_report(report_path, {"app_context.cpp": [line(1)],
                                            "brand_new.cpp": [line(1)]})
            rc = self.run_main(["--report", str(report_path),
                           "--baseline", str(baseline_path)])
            self.assertEqual(rc, 1)

    def test_empty_baseline_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            report_path = pathlib.Path(tmp) / "cov.json"
            baseline_path = pathlib.Path(tmp) / "baseline.json"
            self.write_report(report_path, {"app_context.cpp": [line(1)]})
            rc = self.run_main(["--report", str(report_path),
                           "--baseline", str(baseline_path)])
            self.assertEqual(rc, 1)

    def test_unreadable_report_returns_two(self):
        with contextlib.redirect_stderr(io.StringIO()):
            rc = crc.main(["--report", "/nonexistent/cov.json"])
        self.assertEqual(rc, 2)

    def test_test_only_report_without_baseline_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            report_path = pathlib.Path(tmp) / "cov.json"
            baseline_path = pathlib.Path(tmp) / "baseline.json"
            self.write_report(report_path, {"test_only.cpp": [line(1)]})
            rc = self.run_main(["--report", str(report_path),
                           "--baseline", str(baseline_path)])
            self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()
