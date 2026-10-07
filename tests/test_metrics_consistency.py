#!/usr/bin/env python3
"""Unit tests for the pure helpers of scripts/metrics-consistency-check.py.

No docker, no network: /metrics bodies are parsed from strings and the runtime
check is exercised with an injected fake scraper.

Run with: python3 -m unittest discover -s tests
"""

import importlib.util
import pathlib
import sys
import tempfile
import unittest
import urllib.error

sys.path.insert(0, ".")
REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CHECKER_PATH = REPO_ROOT / "scripts" / "metrics-consistency-check.py"


def load_checker():
    spec = importlib.util.spec_from_file_location("metrics_consistency_check",
                                                  CHECKER_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mc = load_checker()


class NormalizeTest(unittest.TestCase):
    def test_strips_histogram_suffixes(self):
        for suffix in ("_bucket", "_sum", "_count"):
            self.assertEqual(
                mc.normalize("l2_proxy_nats_request_duration_seconds" + suffix),
                "l2_proxy_nats_request_duration_seconds")

    def test_keeps_plain_names(self):
        for name in ("l2_proxy_client_requests_total", "l2_http_pool_tokens",
                     "l2_worker_nats_connected"):
            self.assertEqual(mc.normalize(name), name)

    def test_strips_a_single_trailing_suffix(self):
        self.assertEqual(mc.normalize("l2_x_bucket_sum"), "l2_x_bucket")


class CollectSeriesBlockTest(unittest.TestCase):
    def test_returns_balanced_block_with_nested_braces(self):
        text = ('DynamicLabeledFamily<int>::Series> series{'
                '{"l2_a_total", 1}, {"l2_b_total", 2}};\nnext')
        block = mc.collect_series_block(text, text.index("> series"))
        self.assertEqual(block, '{{"l2_a_total", 1}, {"l2_b_total", 2}}')
        self.assertNotIn("next", block)

    def test_stops_at_first_balanced_close(self):
        text = '>{"l2_a_total", 1}); trailing "l2_b_total"'
        self.assertEqual(mc.collect_series_block(text, 1),
                         '{"l2_a_total", 1}')

    def test_unbalanced_input_returns_partial_block(self):
        self.assertEqual(mc.collect_series_block('>{"l2_a_total", 1}', 1),
                         '{"l2_a_total", 1}')

    def test_no_brace_returns_empty(self):
        self.assertEqual(mc.collect_series_block("nothing here", 0), "")

    def test_window_limit_is_respected(self):
        text = ">{" + '"l2_a_total",' + (" " * 50) + '"l2_b_total"}'
        block = mc.collect_series_block(text, 1, window=10)
        self.assertNotIn("l2_b_total", block)


class CppRegisteredMetricsTest(unittest.TestCase):
    def _write(self, directory, name, body):
        (directory / name).write_text(body, encoding="utf-8")

    def test_finds_create_calls_in_hpp_and_cpp(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp)
            self._write(src, "app_context.cpp",
                        'MetricsManager::create_counter(\n'
                        '    m_proxy_registry, "l2_proxy_requests_total",\n'
                        '    "help");\n')
            self._write(src, "metrics.hpp",
                        'MetricsManager::create_gauge(\n'
                        '    m_registry,\n'
                        '    "l2_worker_tokens", "help");\n')
            registered, literals = mc.cpp_registered_metrics(src)
            self.assertEqual(registered,
                             {"l2_proxy_requests_total", "l2_worker_tokens"})
            self.assertEqual(literals, registered)

    def test_dynamic_labeled_family_series_are_registrations(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp)
            self._write(src, "proxy_init.cpp",
                        'DynamicLabeledFamily<int>::Series> series{\n'
                        '    {"l2_proxy_per_ip_requests_total", 0},\n'
                        '    {"l2_proxy_per_ip_rejected_total", 0}};\n'
                        'MetricsManager::create_gauge(\n'
                        '    m_registry, "l2_proxy_per_ip_rate_limiter_ips_tracked", "x");\n')
            registered, literals = mc.cpp_registered_metrics(src)
            self.assertEqual(registered, {
                "l2_proxy_per_ip_requests_total",
                "l2_proxy_per_ip_rejected_total",
                "l2_proxy_per_ip_rate_limiter_ips_tracked",
            })
            self.assertEqual(registered, literals)

    def test_sentry_fingerprint_is_a_literal_but_not_a_registration(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp)
            self._write(src, "server.cpp",
                        'fingerprint = "l2_server_call_error";\n'
                        'MetricsManager::create_counter(\n'
                        '    m_registry, "l2_server_requests_total", "x");\n')
            registered, literals = mc.cpp_registered_metrics(src)
            self.assertIn("l2_server_call_error", literals)
            self.assertNotIn("l2_server_call_error", registered)

    def test_test_files_are_skipped(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp)
            self._write(src, "test_metrics.cpp",
                        'MetricsManager::create_counter(\n'
                        '    m_registry, "l2_test_only_metric", "x");\n')
            registered, literals = mc.cpp_registered_metrics(src)
            self.assertEqual(registered, set())
            self.assertEqual(literals, set())

    def test_real_sources_exclude_sentry_fingerprint(self):
        registered, literals = mc.cpp_registered_metrics()
        self.assertIn("l2_server_call_error", literals)
        self.assertNotIn("l2_server_call_error", registered)
        self.assertTrue(registered.issubset(literals))


class ParseExpositionTest(unittest.TestCase):
    def test_parses_labelled_bare_and_histogram_lines(self):
        body = (
            '# HELP l2_proxy_client_requests_total help\n'
            '# TYPE l2_proxy_client_requests_total counter\n'
            'l2_proxy_client_requests_total{vm="l2-proxy"} 42\n'
            'l2_http_pool_tokens 7\n'
            'l2_server_request_duration_seconds_bucket{le="0.005"} 3\n'
            'l2_server_request_duration_seconds_bucket{le="+Inf"} 4\n'
            'l2_server_request_duration_seconds_sum{vm="l2-server"} 1.5\n'
            'l2_server_request_duration_seconds_count{vm="l2-server"} 4\n'
            'nginx_http_requests_total 9\n'
        )
        self.assertEqual(mc.parse_exposition(body), {
            "l2_proxy_client_requests_total",
            "l2_http_pool_tokens",
            "l2_server_request_duration_seconds",
        })

    def test_handles_empty_and_comment_only_bodies(self):
        self.assertEqual(mc.parse_exposition(""), set())
        self.assertEqual(mc.parse_exposition("# HELP x y\n"), set())

    def test_ignores_label_values_that_look_like_metrics(self):
        body = 'l2_proxy_requests_total{endpoint="/l2_server_call_error"} 1\n'
        self.assertEqual(mc.parse_exposition(body), {"l2_proxy_requests_total"})


class ReadmeMetricsTest(unittest.TestCase):
    def test_reads_table_rows_and_ignores_prose(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "README.md"
            path.write_text(
                "В тексте есть `l2_not_a_metric`, но это не строка таблицы.\n"
                "| Метрика | Тип |\n"
                "| --- | --- |\n"
                "| `l2_proxy_client_requests_total` | counter |\n"
                "| `l2_server_requests_total`, `l2_worker_db_requests_total` | counter |\n",
                encoding="utf-8")
            self.assertEqual(mc.readme_metrics(path), {
                "l2_proxy_client_requests_total",
                "l2_server_requests_total",
                "l2_worker_db_requests_total",
            })

    def test_real_readme_is_non_empty(self):
        self.assertGreater(len(mc.readme_metrics()), 50)


class CompareTest(unittest.TestCase):
    def test_equal_sets_produce_a_note_and_no_problem(self):
        problems, notes = [], []
        mc.compare("a", {"x"}, "b", {"x"}, problems, notes)
        self.assertEqual(problems, [])
        self.assertEqual(notes, ["a == b (1 metrics)"])

    def test_reports_both_directions(self):
        problems, notes = [], []
        mc.compare("a", {"x", "y"}, "b", {"y", "z"}, problems, notes)
        self.assertEqual(len(problems), 2)
        self.assertIn("x", problems[0])
        self.assertIn("z", problems[1])
        self.assertEqual(notes, [])

    def test_is_symmetric_in_detection(self):
        for left, right in (({"a"}, set()), (set(), {"a"})):
            problems, notes = [], []
            mc.compare("a", left, "b", right, problems, notes)
            self.assertEqual(len(problems), 1)


class SplitLazyTest(unittest.TestCase):
    def test_traffic_dependent_families_are_notes_not_problems(self):
        problems, notes = [], []
        mc.split_lazy({"l2_proxy_per_ip_requests_total", "l2_worker_db_requests_total"},
                      problems, notes, "label: registered but not exported", "label: ")
        self.assertEqual(problems, [])
        self.assertEqual(len(notes), 1)
        self.assertIn("lazy", notes[0])

    def test_non_lazy_gap_is_a_problem(self):
        problems, notes = [], []
        mc.split_lazy({"l2_proxy_client_requests_total"}, problems, notes,
                      "label: registered but not exported", "label: ")
        self.assertEqual(len(problems), 1)
        self.assertIn("l2_proxy_client_requests_total", problems[0])
        self.assertEqual(notes, [])

    def test_mixed_split_keeps_both_channels(self):
        problems, notes = [], []
        mc.split_lazy({"l2_proxy_per_ip_requests_total", "l2_proxy_nats_requests_total"},
                      problems, notes, "label: registered but not exported", "label: ")
        self.assertEqual(len(problems), 1)
        self.assertIn("l2_proxy_nats_requests_total", problems[0])
        self.assertNotIn("l2_proxy_per_ip_requests_total", problems[0])
        self.assertEqual(len(notes), 1)


class GoldenMetricsTest(unittest.TestCase):
    def test_merges_catalog_and_conditional(self):
        class FakeGolden:
            CATALOG = ["l2_a_total"]
            CONDITIONAL = ["l2_b_total"]

        self.assertEqual(mc.golden_metrics(FakeGolden),
                         {"l2_a_total", "l2_b_total"})


class RuntimeCheckTest(unittest.TestCase):
    ENDPOINTS = {
        "proxy": ("http://proxy/metrics", ("l2_proxy_",)),
        "worker": ("http://worker/metrics", ("l2_worker_",)),
    }

    def _run(self, scraper, registered=None, dashboards=None):
        registered = registered if registered is not None else {
            "l2_proxy_client_requests_total", "l2_proxy_nats_requests_total",
            "l2_worker_requests_processed_total",
            "l2_tracing_spans_sent_total",
        }
        dashboards = dashboards if dashboards is not None else set(registered)
        problems, notes = [], []
        mc.runtime_check(registered, dashboards, 5.0, problems, notes,
                         endpoints=self.ENDPOINTS, scraper=scraper)
        return problems, notes

    def test_matching_stack_produces_no_problems(self):
        exports = {
            "http://proxy/metrics": {"l2_proxy_client_requests_total",
                                     "l2_proxy_nats_requests_total",
                                     "l2_tracing_spans_sent_total"},
            "http://worker/metrics": {"l2_worker_requests_processed_total",
                                      "l2_tracing_spans_sent_total"},
        }
        problems, notes = self._run(lambda url, timeout: exports[url])
        self.assertEqual(problems, [])
        self.assertTrue(any("matches registration" in note for note in notes))
        self.assertTrue(any("every metric is exported by some service"
                            in note for note in notes))

    def test_shared_families_do_not_count_as_foreign(self):
        exports = {
            "http://proxy/metrics": {"l2_proxy_client_requests_total",
                                     "l2_proxy_nats_requests_total",
                                     "l2_tracing_spans_sent_total"},
            "http://worker/metrics": {"l2_worker_requests_processed_total",
                                      "l2_tracing_spans_sent_total"},
        }
        problems, _ = self._run(lambda url, timeout: exports[url])
        self.assertEqual([p for p in problems if "registered elsewhere" in p], [])

    def test_dashboard_metric_absent_everywhere_is_a_problem(self):
        exports = {
            "http://proxy/metrics": {"l2_proxy_client_requests_total",
                                     "l2_proxy_nats_requests_total",
                                     "l2_tracing_spans_sent_total"},
            "http://worker/metrics": {"l2_worker_requests_processed_total",
                                      "l2_tracing_spans_sent_total"},
        }
        registered = {"l2_proxy_client_requests_total",
                      "l2_proxy_nats_requests_total",
                      "l2_worker_requests_processed_total",
                      "l2_tracing_spans_sent_total"}
        dashboards = registered | {"l2_proxy_dashboard_only_ghost"}
        problems, notes = self._run(lambda url, timeout: exports[url],
                                    registered=registered, dashboards=dashboards)
        self.assertEqual(len(problems), 1)
        self.assertIn("never exported by any service", problems[0])
        self.assertIn("l2_proxy_dashboard_only_ghost", problems[0])

    def test_registered_but_not_exported_is_a_problem(self):
        exports = {
            "http://proxy/metrics": {"l2_proxy_nats_requests_total"},
            "http://worker/metrics": set(),
        }
        problems, notes = self._run(lambda url, timeout: exports[url])
        self.assertTrue(any("registered but not exported" in p
                            for p in problems))

    def test_lazy_missing_family_is_only_a_note(self):
        registered = {"l2_proxy_client_requests_total",
                      "l2_proxy_per_ip_requests_total"}
        exports = {
            "http://proxy/metrics": {"l2_proxy_client_requests_total"},
            "http://worker/metrics": set(),
        }
        problems, notes = self._run(lambda url, timeout: exports[url],
                                    registered=registered,
                                    dashboards=registered)
        self.assertEqual([p for p in problems if "l2_proxy_per_ip" in p], [])
        self.assertTrue(any("lazy" in note for note in notes))

    def test_stack_down_reports_one_actionable_problem(self):
        def scraper(url, timeout):
            raise urllib.error.URLError("connection refused")

        problems, _ = self._run(scraper)
        self.assertEqual(len(problems), 1)
        self.assertIn("no /metrics endpoint is reachable", problems[0])
        self.assertIn("--offline", problems[0])

    def test_single_role_down_is_reported_individually(self):
        def scraper(url, timeout):
            if url == "http://worker/metrics":
                raise urllib.error.URLError("connection refused")
            return {"l2_proxy_client_requests_total",
                    "l2_proxy_nats_requests_total",
                    "l2_tracing_spans_sent_total"}

        problems, notes = self._run(scraper)
        self.assertTrue(any("cannot scrape worker" in p for p in problems))
        self.assertFalse(any("no /metrics endpoint is reachable" in p
                             for p in problems))


GOLDEN_PATH = REPO_ROOT / "scripts" / "metrics-golden-check.py"


def load_golden():
    spec = importlib.util.spec_from_file_location("metrics_golden_check",
                                                  GOLDEN_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


golden = load_golden()


class GoldenRequiredTest(unittest.TestCase):
    DB_FAMILIES = {
        "l2_proxy_db_requests_total",
        "l2_proxy_db_request_duration_seconds",
        "l2_proxy_db_nats_request_duration_seconds",
        "l2_worker_db_requests_total",
        "l2_worker_db_query_duration_seconds",
    }

    def test_default_requires_full_catalogue(self):
        self.assertEqual(golden.build_required(False, False), golden.CATALOG)

    def test_db_adds_exactly_the_db_gateway_families(self):
        db_only = set(golden.build_required(False, True)) - set(golden.CATALOG)
        self.assertEqual(db_only, self.DB_FAMILIES)

    def test_db_families_are_a_subset_of_conditional(self):
        self.assertEqual(self.DB_FAMILIES, set(golden.CONDITIONAL) & self.DB_FAMILIES)

    def test_db_and_all_do_not_duplicate_families(self):
        both = golden.build_required(True, True)
        self.assertEqual(len(both), len(set(both)))
        self.assertEqual(self.DB_FAMILIES, self.DB_FAMILIES & set(both))


class GoldenCatalogConsistencyTest(unittest.TestCase):
    """Pure invariants of the golden-check catalog.

    These keep the *internal* model coherent so that the --traffic/--db/--all
    combinations declared in CI stay meaningful: every family we assert to be
    non-zero must also be required by the default gate, lazy families must not
    shadow a catalog entry, and neither list may contain duplicates.
    """

    def test_traffic_queries_are_a_subset_of_catalog(self):
        missing = [q for q in golden.TRAFFIC_QUERIES
                   if q not in golden.CATALOG]
        self.assertEqual(missing, [])

    def test_conditional_does_not_shadow_catalog(self):
        overlap = set(golden.CONDITIONAL) & set(golden.CATALOG)
        self.assertEqual(overlap, set(),
                         "lazy family also listed in CATALOG: %r" % overlap)

    def test_catalog_has_no_duplicates(self):
        self.assertEqual(len(golden.CATALOG), len(set(golden.CATALOG)))

    def test_conditional_has_no_duplicates(self):
        self.assertEqual(len(golden.CONDITIONAL), len(set(golden.CONDITIONAL)))

    def test_traffic_queries_have_no_histogram_suffixes(self):
        for q in golden.TRAFFIC_QUERIES:
            self.assertEqual(mc.normalize(q), q)

    def test_traffic_queries_have_no_duplicates(self):
        self.assertEqual(len(golden.TRAFFIC_QUERIES),
                         len(set(golden.TRAFFIC_QUERIES)))

    def test_db_families_are_the_only_conditional_families_with_db_marker(self):
        db_marked = [f for f in golden.CONDITIONAL if "_db_" in f]
        self.assertEqual(set(db_marked), GoldenRequiredTest.DB_FAMILIES)

    def test_all_ordered_after_catalog_and_non_duplicated(self):
        required = golden.build_required(True, False)
        self.assertEqual(required[:len(golden.CATALOG)], golden.CATALOG)
        self.assertEqual(len(required), len(set(required)))


if __name__ == "__main__":
    unittest.main()
