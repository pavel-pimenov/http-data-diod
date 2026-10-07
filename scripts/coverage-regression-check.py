#!/usr/bin/env python3
"""Пер-файловый гейт регрессии покрытия: gcovr JSON ↔ закоммиченный baseline.

Сравнивает построчное покрытие каждого production-файла с baseline из git и
падает, если файл понизил свой процент (потерянные покрытые строки или новые
непокрытые). Тестовые TU (`test_*.cpp`) не сравниваются и в baseline не пишутся:
их качество контролируют глобальные гейты строки/ветвей в src/Dockerfile.

Baseline генерируется тем же скриптом из отчёта, который рисует coverage-стадия
(`coverage-report/cov.json`), и обновляется осознанно, вместе с кодом.

Usage:
  scripts/coverage-regression-check.py                 # проверка
  scripts/coverage-regression-check.py --report X.json # путь к отчёту gcovr
  scripts/coverage-regression-check.py --max-drop 0.5  # допуск в п.п.
  scripts/coverage-regression-check.py --update        # переписать baseline

Exit codes: 0 — ок, 1 — регрессия или новый файл вне baseline, 2 — ошибка
чтения отчёта/baseline.
"""

import argparse
import json
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_REPORT = REPO_ROOT / "coverage-report" / "cov.json"
DEFAULT_BASELINE = REPO_ROOT / "docs" / "coverage-baseline.json"

BASELINE_COMMENT = (
    "Пер-файловая база покрытия production-файлов (отчёт gcovr --json). "
    "Сверяется скриптом scripts/coverage-regression-check.py; обновляй: "
    "scripts/coverage-regression-check.py --update"
)


def is_test_file(name):
    return pathlib.PurePosixPath(name).name.startswith("test_")


def summarize_file(entry):
    lines = covered = branches = branches_covered = 0
    for line in entry.get("lines", []):
        if line.get("gcovr/noncode"):
            continue
        lines += 1
        if line.get("count", 0) > 0:
            covered += 1
        for branch in line.get("branches", []):
            branches += 1
            if branch.get("count", 0) > 0:
                branches_covered += 1
    return {
        "lines": lines,
        "covered": covered,
        "branches": branches,
        "branches_covered": branches_covered,
    }


def load_report(path):
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    return {entry["file"]: summarize_file(entry) for entry in data.get("files", [])}


def load_baseline(path):
    if not path.exists():
        return {}
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    return data.get("files", {})


def write_baseline(path, files):
    payload = {
        "comment": BASELINE_COMMENT,
        "files": {name: files[name] for name in sorted(files)},
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, ensure_ascii=False, indent=2)
        fh.write("\n")


def line_pct(stat):
    if not stat.get("lines"):
        return 0.0
    return 100.0 * stat.get("covered", 0) / stat["lines"]


def compare(baseline, current, max_drop):
    """Возвращает (regressions, new_files, removed) в production-файлах."""
    regressions = []
    new_files = []
    for name in sorted(current):
        if is_test_file(name):
            continue
        now = current[name]
        if not now.get("lines"):
            continue
        if name not in baseline:
            new_files.append(name)
            continue
        old = baseline[name]
        if not old.get("lines"):
            continue
        old_pct = line_pct(old)
        now_pct = line_pct(now)
        drop = old_pct - now_pct
        if drop > max_drop + 1e-9:
            regressions.append((name, old_pct, now_pct, drop))
    removed = sorted(name for name in baseline
                     if name not in current and not is_test_file(name))
    return regressions, new_files, removed


def production_stats(files):
    total = covered = 0
    for name, stat in files.items():
        if is_test_file(name):
            continue
        total += stat.get("lines", 0)
        covered += stat.get("covered", 0)
    return total, covered


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Гейт регрессии построчного покрытия (gcovr JSON vs baseline)")
    parser.add_argument("--report", default=str(DEFAULT_REPORT),
                        help="отчёт gcovr в формате --json")
    parser.add_argument("--baseline", default=str(DEFAULT_BASELINE),
                        help="путь к baseline-файлу")
    parser.add_argument("--max-drop", type=float, default=0.0,
                        help="допустимое снижение покрытия файла в п.п. (по умолчанию 0)")
    parser.add_argument("--update", action="store_true",
                        help="переписать baseline из текущего отчёта")
    args = parser.parse_args(argv)

    report_path = pathlib.Path(args.report)
    baseline_path = pathlib.Path(args.baseline)
    try:
        current = load_report(report_path)
    except (OSError, ValueError, KeyError) as exc:
        print(f"не удалось прочитать отчёт {report_path}: {exc}", file=sys.stderr)
        return 2

    production = {name: stat for name, stat in current.items()
                  if not is_test_file(name)}
    total, covered = production_stats(production)
    pct = (100.0 * covered / total) if total else 0.0
    print(f"production-файлы: {len(production)}, строк {covered}/{total} "
          f"= {pct:.2f}%")

    if args.update:
        try:
            write_baseline(baseline_path, production)
        except OSError as exc:
            print(f"не удалось записать baseline {baseline_path}: {exc}",
                  file=sys.stderr)
            return 2
        print(f"baseline обновлён: {baseline_path} ({len(production)} файлов)")
        return 0

    try:
        baseline = load_baseline(baseline_path)
    except (OSError, ValueError, KeyError) as exc:
        print(f"не удалось прочитать baseline {baseline_path}: {exc}",
              file=sys.stderr)
        return 2

    if not baseline:
        print(f"baseline пуст или не найден ({baseline_path}) — "
              "запусти --update", file=sys.stderr)
        return 1

    regressions, new_files, removed = compare(baseline, production, args.max_drop)
    for name, old_pct, now_pct, drop in regressions:
        print(f"REGRESSION {name}: {old_pct:.2f}% -> {now_pct:.2f}% "
              f"(-{drop:.2f} п.п.)")
    for name in new_files:
        print(f"NEW {name}: файла нет в baseline — "
              "прими его: coverage-regression-check.py --update")
    for name in removed:
        print(f"REMOVED {name}: есть в baseline, нет в отчёте — "
              "обнови baseline: coverage-regression-check.py --update")

    if regressions or new_files:
        print(f"FAIL: {len(regressions)} регрессий, {len(new_files)} новых "
              "файлов вне baseline")
        return 1
    print(f"OK: регрессий покрытия нет (допуск {args.max_drop} п.п.)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
