#!/usr/bin/env python3
"""Unit tests for registry-report cleanup recommendations and txt report.

Exercises _cleanup_recommendations (keep/candidates by the "current +
previous" rule) and _render_txt (text report with layer sizes and the
recommendations section). The module needs flask; the whole suite is skipped
when flask is not installed (the host usually lacks it — it lives in the
registry-report-server image).

Run with: python3 -m unittest discover -s tests
"""

import importlib.util
import pathlib
import unittest

_here = pathlib.Path(__file__).resolve().parent
REPO_ROOT = _here.parent.parent
# Скрипт ищем в нескольких местах: в репо (scripts/) и рядом с тестом
# (контейнер registry-report-server кладёт его в /opt/registry-report/).
SERVER_PY = next(
    p for p in [
        REPO_ROOT / "scripts" / "registry-report-server.py",
        _here.parent / "scripts" / "registry-report-server.py",
        _here / "registry-report-server.py",
    ] if p.exists()
)


def _load_module():
    try:
        spec = importlib.util.spec_from_file_location("registry_report_server",
                                                       SERVER_PY)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod
    except ImportError:
        return None


m = _load_module()


@unittest.skipIf(m is None, "flask not installed (runs in the container)")
class RegistryReportTxtTest(unittest.TestCase):
    def test_tag_sort_key_current_first(self):
        # latest/stable считаются «текущими» и сортируются выше дат.
        self.assertGreater(m._tag_sort_key("latest"),
                           m._tag_sort_key("20230627112815"))
        self.assertGreater(m._tag_sort_key("stable"),
                           m._tag_sort_key("20230627112815"))
        # дата больше семантической версии
        self.assertGreater(m._tag_sort_key("20230627112815"),
                           m._tag_sort_key("v1.2.3"))
        # семантическая версия больше лексикографической строки
        self.assertGreater(m._tag_sort_key("v2.0.0"), m._tag_sort_key("foo"))

    def test_cleanup_keep_current_and_previous(self):
        repos = [{
            "repo": "app",
            "tag_rows": [
                {"tag": "20230101", "size": 100, "layers": []},
                {"tag": "20230201", "size": 100, "layers": []},
                {"tag": "20230301", "size": 100, "layers": []},
            ],
        }]
        recs = m._cleanup_recommendations(repos)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0]["keep"], ["20230201", "20230301"])
        self.assertEqual(recs[0]["candidates"], ["20230101"])

    def test_cleanup_latest_kept_even_if_old(self):
        repos = [{
            "repo": "app",
            "tag_rows": [
                {"tag": "latest", "size": 100, "layers": []},
                {"tag": "20230101", "size": 100, "layers": []},
                {"tag": "20230201", "size": 100, "layers": []},
                {"tag": "20230301", "size": 100, "layers": []},
            ],
        }]
        recs = m._cleanup_recommendations(repos)
        self.assertEqual(len(recs), 1)
        # latest всегда в keep; предыдущая = самый новый по дате
        self.assertIn("latest", recs[0]["keep"])
        self.assertIn("20230301", recs[0]["keep"])
        self.assertIn("20230101", recs[0]["candidates"])
        self.assertIn("20230201", recs[0]["candidates"])

    def test_cleanup_unique_layer_size(self):
        repos = [{
            "repo": "app",
            "tag_rows": [
                # кандидат
                {"tag": "20230101", "size": 200,
                 "layers": [{"digest": "sha256:aa", "size": 100},
                            {"digest": "sha256:bb", "size": 100}]},
                # keep
                {"tag": "20230201", "size": 150,
                 "layers": [{"digest": "sha256:aa", "size": 100},
                            {"digest": "sha256:cc", "size": 50}]},
                {"tag": "20230301", "size": 100,
                 "layers": [{"digest": "sha256:dd", "size": 100}]},
            ],
        }]
        recs = m._cleanup_recommendations(repos)
        c = recs[0]
        # суммарный размер кандидата = 200
        self.assertEqual(c["candidate_size"], 200)
        # уникальный (не пересекается с keep): только sha256:bb (100)
        self.assertEqual(c["candidate_unique_size"], 100)

    def test_render_txt_contains_sections(self):
        report = {
            "registry": "reg.example",
            "generated_at": "2026-01-01T00:00:00",
            "repo_count": 1,
            "total_size": 300,
            "repos": [{
                "repo": "app",
                "tags": ["v1", "v2"],
                "size": 300,
                "tag_rows": [
                    {"tag": "v1", "size": 150,
                     "layers": [{"digest": "sha256:aa", "size": 150}]},
                    {"tag": "v2", "size": 150,
                     "layers": [{"digest": "sha256:bb", "size": 150}]},
                ],
            }],
        }
        txt = m._render_txt(report)
        self.assertIn("РЕКОМЕНДАЦИИ ПО УДАЛЕНИЮ", txt)
        self.assertIn("sha256:aa", txt)
        self.assertIn("sha256:bb", txt)


if __name__ == "__main__":
    unittest.main()
