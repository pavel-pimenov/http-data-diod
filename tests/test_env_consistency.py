#!/usr/bin/env python3
"""Unit tests for scripts/env-consistency-check.py (env gate vs compose).

No docker, no network: fake C++ trees and compose texts are built in a temp
dir and the pure functions are exercised via importlib.

Run with: python3 -m unittest discover -s tests
"""

import importlib.util
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, ".")
REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SCRIPT_PATH = REPO_ROOT / "scripts" / "env-consistency-check.py"


def load_envcheck():
    spec = importlib.util.spec_from_file_location("env_consistency_check",
                                                  SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ec = load_envcheck()


class ComposeParsingTest(unittest.TestCase):
    def test_list_form_environment_key(self):
        text = ("services:\n  svc:\n"
                "    environment:\n"
                "      - LOG_LEVEL=${LOG_LEVEL:-INFO}\n"
                "      - MODE=proxy\n")
        self.assertEqual(ec.compose_env_vars(text), ["LOG_LEVEL", "MODE"])

    def test_mapping_form_environment_key(self):
        text = ("services:\n  svc:\n"
                "    environment:\n"
                "      DEFAULT_VAR: \"value\"\n"
                "      INTERP_VAR: ${INTERP_VAR:-x}\n")
        self.assertEqual(ec.compose_env_vars(text),
                         ["DEFAULT_VAR", "INTERP_VAR"])

    def test_build_args_are_counted(self):
        text = ("services:\n  svc:\n"
                "    build:\n"
                "      args:\n"
                "        BASE_IMAGE: ${BASE_IMAGE:-ubuntu}\n"
                "        CACHE_BUST: ${CACHE_BUST:-1}\n")
        self.assertEqual(ec.compose_env_vars(text), ["BASE_IMAGE", "CACHE_BUST"])

    def test_interpolation_only(self):
        text = ("services:\n  svc:\n"
                "    mem_limit: ${SVC_MEM_LIMIT:-256m}\n"
                "    restart: unless-stopped\n")
        self.assertEqual(ec.compose_env_vars(text), ["SVC_MEM_LIMIT"])


class CppParsingTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def _write(self, rel, content):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def test_get_env_directive_captured(self):
        self._write("config.cpp", 'get_env_string("LOG_LEVEL", ...);\n'
                                  'get_env_bool("ENABLE_TRACING", false);\n')
        self.assertEqual(ec.cpp_env_vars(self.root),
                         ["ENABLE_TRACING", "LOG_LEVEL"])

    def test_getenv_literal_captured(self):
        self._write("main.cpp", 'const char *d = std::getenv("CRASH_DUMP_DIR");\n'
                                'const char *a = std::getenv ("ASAN_OPTIONS");\n')
        self.assertIn("CRASH_DUMP_DIR", ec.cpp_env_vars(self.root))
        self.assertIn("ASAN_OPTIONS", ec.cpp_env_vars(self.root))

    def test_test_files_are_ignored(self):
        self._write("test_components.cpp", 'get_env_bool("TEST_ONLY", false);\n')
        self.assertEqual(ec.cpp_env_vars(self.root), [])

    def test_vendored_dirs_are_ignored(self):
        self._write("httplib/httplib.cc", 'get_env_string("HTTPLIB_ENV", "");\n')
        self._write("odpi/src/dpi.c", 'get_env_string("DPI_ENV", "");\n')
        self._write("prometheus-cpp/civetweb/civetweb.c",
                    'getenv("PATH");\n')
        self.assertEqual(ec.cpp_env_vars(self.root), [])


class EnvConsistencyCheckTest(unittest.TestCase):
    def test_missing_var_is_reported(self):
        cpp = ["A_VAR", "B_VAR"]
        compose = ["A_VAR"]
        missing, orphans = ec.check(cpp, compose)
        self.assertEqual(missing, ["B_VAR"])
        self.assertEqual(orphans, [])

    def test_present_vars_pass(self):
        missing, orphans = ec.check(["A_VAR", "B_VAR"],
                                    ["B_VAR", "A_VAR", "EXTRA"])
        self.assertEqual(missing, [])
        self.assertEqual(orphans, ["EXTRA"])

    def test_orphans_never_affect_missing(self):
        cpp = ["A_VAR"]
        compose = ["A_VAR", "X", "Y"]
        missing, orphans = ec.check(cpp, compose)
        self.assertEqual(missing, [])
        self.assertEqual(set(orphans), {"X", "Y"})


if __name__ == "__main__":
    unittest.main()