#!/usr/bin/env python3
"""Unit tests for the registry-report UI inline JavaScript.

The UI serves a single in-page <script> embedded in registry-report-server.py.
A syntax error there (e.g. a Python `# noqa` comment leaking into the JS, or a
stray token) silently kills the whole page: the registry combo-box stays empty
and the "Generate report" button never enables. These tests catch regressions.

The no-node assertions run everywhere (stdlib only). The `node --check` gate
needs node on PATH; it is skipped when node is absent (it normally runs in the
registry-report-server container via scripts/check-registry-report-js.sh).

Run with: python3 -m unittest discover -s tests
"""

import pathlib
import re
import shutil
import subprocess
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SERVER_PY = REPO_ROOT / "scripts" / "registry-report-server.py"

# JS tokens that are valid Python (as `# noqa` comments) but invalid inside a
# <script> block. If any appears in the JS, the whole page's script breaks.
FORBIDDEN_JS_TOKENS = ["# noqa", "# noqa: E501"]


def extract_js():
    """Return the concatenated <script> bodies from registry-report-server.py."""
    src = SERVER_PY.read_text(encoding="utf-8")
    blocks = re.findall(r"<script>(.*?)</script>", src, re.S)
    return "\n;\n".join(blocks)


class RegistryReportJsTest(unittest.TestCase):
    def test_script_block_present(self):
        self.assertTrue(SERVER_PY.exists(), f"{SERVER_PY} missing")
        self.assertGreater(len(extract_js()), 100,
                           "expected a non-trivial <script> block in the UI")

    def test_no_python_noqa_leaks_into_js(self):
        # Regression: Python `# noqa: E501` comments once leaked into the JS
        # (via the embedded HTML string), which is a syntax error that killed
        # the whole page (empty combo-box, disabled button).
        js = extract_js()
        for tok in FORBIDDEN_JS_TOKENS:
            self.assertNotIn(tok, js,
                             f"'{tok}' must not appear inside the <script> "
                             "block (it is a Python comment that breaks JS)")

    @unittest.skipUnless(shutil.which("node"),
                         "node not on PATH (JS syntax check runs in the "
                         "registry-report-server container)")
    def test_js_syntax_with_node(self):
        js = extract_js()
        proc = subprocess.run(["node", "--check", "-"],
                              input=js, text=True,
                              capture_output=True)
        self.assertEqual(proc.returncode, 0,
                         f"node --check failed:\n{proc.stderr}")


if __name__ == "__main__":
    unittest.main()
