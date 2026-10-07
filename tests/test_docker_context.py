#!/usr/bin/env python3
"""Unit tests for scripts/docker-context-check.py (build-context gate).

No docker, no git needed: the pure matcher/extractor functions are fed with
synthetic ignore specs and Dockerfile texts via importlib.

Run with: python3 -m unittest discover -s tests
"""

import importlib.util
import pathlib
import sys
import unittest

sys.path.insert(0, ".")
REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SCRIPT_PATH = REPO_ROOT / "scripts" / "docker-context-check.py"


def load_dockcheck():
    spec = importlib.util.spec_from_file_location("docker_context_check",
                                                  SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


dc = load_dockcheck()


class ExcludedByIgnoreTest(unittest.TestCase):
    def test_last_match_wins(self):
        spec = ["*.sh", "!generate_version.sh"]
        self.assertFalse(
            dc.excluded_by_ignore(spec, "generate_version.sh"))  # re-included
        self.assertTrue(dc.excluded_by_ignore(spec, "setup.sh"))

    def test_negation_does_not_rescue_unmatched(self):
        spec = ["*.py", "!setup.py"]
        self.assertFalse(dc.excluded_by_ignore(spec, "setup.py"))
        self.assertTrue(dc.excluded_by_ignore(spec, "other.py"))

    def test_no_slash_pattern_matches_any_depth(self):
        self.assertTrue(dc.excluded_by_ignore(["*.log"], "build/out/run.log"))

    def test_dir_only_pattern_trailing_slash(self):
        spec = ["build/"]
        self.assertTrue(dc.excluded_by_ignore(spec, "build/main.o"))
        self.assertFalse(dc.excluded_by_ignore(spec, "src/build/main.o"))

    def test_bare_dir_name_matches_any_depth(self):
        # docker: a bare name matches a file/dir with that name at any depth.
        spec = ["build"]
        self.assertTrue(dc.excluded_by_ignore(spec, "nats/build/x.o"))
        self.assertTrue(dc.excluded_by_ignore(spec, "build/x.o"))

    def test_double_star_matches_across_dirs(self):
        spec = ["docker-memory-analysis/**"]
        self.assertTrue(
            dc.excluded_by_ignore(spec, "docker-memory-analysis/log.txt"))

    def test_inline_slash_path_is_relative_to_root(self):
        spec = ["test/"]
        self.assertTrue(dc.excluded_by_ignore(spec, "test/foo.cpp"))
        self.assertFalse(dc.excluded_by_ignore(spec, "src/test/foo.cpp"))


class ReferencedSourcesTest(unittest.TestCase):
    def test_copy_source_captured(self):
        text = "COPY nats ./nats\n"
        self.assertEqual(dc.referenced_src_paths(text), ["nats"])

    def test_copy_from_stage_and_absolute_paths_skipped(self):
        text = ("COPY --from=builder /app/out/l2-proxy .\n"
                "COPY docker-entrypoint.sh /app/docker-entrypoint.sh\n")
        self.assertEqual(dc.referenced_src_paths(text),
                         ["docker-entrypoint.sh"])

    def test_copy_dot_is_whole_context(self):
        self.assertEqual(dc.referenced_src_paths("COPY . .\n"), [])

    def test_run_local_script_captured(self):
        text = "RUN if [ ! -f ./x.h ]; then ./generate_version.sh > ./x.h; fi\n"
        self.assertEqual(dc.referenced_src_paths(text), ["generate_version.sh"])

    def test_run_binary_artifact_not_a_context_ref(self):
        text = ('RUN cd build-cov && ./test_components && ./test_proxy_core\n'
                'CMD ["sh", "-c", "exec ./l2-proxy"]\n')
        self.assertEqual(dc.referenced_src_paths(text), [])


class ApplyIgnoreRegressionTest(unittest.TestCase):
    """The regression this guard exists for: dockerignore silently dropping a
    script the Dockerfile's builder stage executes."""

    def test_generate_version_sh_would_be_detected_as_missing(self):
        keep = dc.apply_ignore(["*.sh"], ["generate_version.sh", "main.cpp"])
        self.assertEqual(keep, ["main.cpp"])
        self.assertNotIn("generate_version.sh", keep)

    def test_negation_restores_the_script(self):
        spec = ["build", "*.sh", "!docker-entrypoint.sh", "!generate_version.sh"]
        keep = dc.apply_ignore(spec, ["generate_version.sh",
                                      "docker-entrypoint.sh",
                                      "main.cpp", "build/out.o"])
        self.assertEqual(keep, ["generate_version.sh", "docker-entrypoint.sh",
                                "main.cpp"])


if __name__ == "__main__":
    unittest.main()
