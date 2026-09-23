#!/usr/bin/env python3
"""Проверка/обновление вендорных библиотек из git.

В дерево вендорятся только исходники, минимально необходимые сервисам:
тесты, бенчмарки, examples, CMakeLists.txt и BUILD.bazel апстрима не
переносятся (см. пути в KEEP и комментарии к ним). Для каждого пакета клон
создаётся во временном каталоге на пине (тег/коммит/ветка), после чего
сравниваются только файлы, уже присутствующие в локальном подмножестве.

По умолчанию скрипт ничего не записывает — выводит отчёт о соответствии
локального дерева пинам. Запись разрешена только для пакетов, у которых
явно задан новый ref через --ref (апгрейд); при этом локально пропатченные
файлы (смотрите PATCHED) не перезаписываются без --force.

Для cpp-httplib дополнительно вызывается штатный scripts/split.py этого же
тега: из single-header httplib.h собирается пара httplib.h + httplib.cc
(реализация выносится в .cc), и эти два файла являются артефактами синка.

Usage:
  python3 scripts/update-vendored-libs.py                # отчёт по текущим пинам
  python3 scripts/update-vendored-libs.py --only nats.c  # отчёт по одной либе
  python3 scripts/update-vendored-libs.py --ref cpp-httplib=v0.57.0 \
      --ref nlohmann-json=v3.13.0 -v                    # апгрейд + копирование
  python3 scripts/update-vendored-libs.py --ref json=v3.13.0 --force
  python3 scripts/update-vendored-libs.py --prune       # в апгрейде удалять файлы,
                                                         # исчезнувшие в апстриме
  python3 scripts/update-vendored-libs.py --dry-run --ref ...  # отчёт без записи

Exit code 0 = ok, 1 = ошибка (нет сети/ref/пути).
"""

import argparse
import filecmp
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SRC_DIR = REPO_ROOT / "src"

# (апстрим-путь, локальный путь относительно src/) — синхронизируются только
# они; внутри каталогов учитываются лишь файлы, уже лежащие в локальном дереве.
KEEP = {
    "nlohmann-json": [
        ("single_include/nlohmann", "json/nlohmann"),
    ],
    "prometheus-cpp": [
        ("core/include", "prometheus-cpp/core/include"),
        ("core/src", "prometheus-cpp/core/src"),
        ("pull/include", "prometheus-cpp/pull/include"),
        ("pull/src", "prometheus-cpp/pull/src"),
        ("push/include", "prometheus-cpp/push/include"),
        ("push/src", "prometheus-cpp/push/src"),
        ("util/include", "prometheus-cpp/util/include"),
        ("LICENSE", "prometheus-cpp/LICENSE"),
        ("README.md", "prometheus-cpp/README.md"),
    ],
    "civetweb": [
        # prometheus-cpp не носит civetweb в своём дереве (подтягивает при
        # сборке); здесь он завендорен отдельно для pull-экспозера
        ("include", "prometheus-cpp/3rdparty/civetweb/include"),
        ("src", "prometheus-cpp/3rdparty/civetweb/src"),
        ("LICENSE.md", "prometheus-cpp/3rdparty/civetweb/LICENSE.txt"),
    ],
    "nats.c": [
        # примеры и тесты апстрима не переносятся; добавление их в сборку
        # штатного CMakeLists отключается локальной правкой (см. PATCHED)
        ("CLAUDE.md", "nats/CLAUDE.md"),
        ("CMakeLists.txt", "nats/CMakeLists.txt"),
        ("CODE-OF-CONDUCT.md", "nats/CODE-OF-CONDUCT.md"),
        ("GOVERNANCE.md", "nats/GOVERNANCE.md"),
        ("LICENSE", "nats/LICENSE"),
        ("MAINTAINERS.md", "nats/MAINTAINERS.md"),
        ("README.md", "nats/README.md"),
        ("codecov.yml", "nats/codecov.yml"),
        ("dependencies.md", "nats/dependencies.md"),
        ("src", "nats/src"),
    ],
    "cpp-httplib": [
        # артефакты split.py этого же тега (реализация вынесена в .cc)
        ("httplib.h", "httplib/httplib.h"),
        ("httplib.cc", "httplib/httplib.cc"),
    ],
    "base64": [
        ("include/base64.hpp", "base64/base64.hpp"),
    ],
    "odpi": [
        ("LICENSE.txt", "odpi/LICENSE.txt"),
        ("NOTICE.txt", "odpi/NOTICE.txt"),
        ("README.md", "odpi/README.md"),
        ("embed", "odpi/embed"),
        ("include", "odpi/include"),
        ("src", "odpi/src"),
    ],
}

# Пины по умолчанию = тому, что сейчас лежит в дереве.
REFS = {
    "nlohmann-json": "v3.12.0",
    "prometheus-cpp": "00c13295d53cf900c1e8614e4e63b53df52d6cc5",
    "civetweb": "v1.16",
    "nats.c": "v3.14.0",
    "cpp-httplib": "v0.58.0",
    "base64": "master",
    "odpi": "v26.0.0",
}

GIT_REPOS = {
    "nlohmann-json": "https://github.com/nlohmann/json.git",
    "prometheus-cpp": "https://github.com/jupp0r/prometheus-cpp.git",
    "civetweb": "https://github.com/civetweb/civetweb.git",
    "nats.c": "https://github.com/nats-io/nats.c.git",
    "cpp-httplib": "https://github.com/yhirose/cpp-httplib.git",
    "base64": "https://github.com/tobiaslocker/base64.git",
    "odpi": "https://github.com/oracle/odpi.git",
}

LIB_NAMES = list(GIT_REPOS)

# Локально пропатченные файлы: отличаются от апстрима намеренными правками и
# при апгрейде не перезаписываются, пока не задан --force.
PATCHED = {
    "json/nlohmann/json.hpp":
        "NOLINT-комментарии, GCC C++20 modules workaround, SPDX-хедер",
    "json/nlohmann/json_fwd.hpp": "точечные правки, как у json.hpp",
    "nats/CMakeLists.txt": "examples/test за опциями (каталоги не вендорены)",
    "prometheus-cpp/3rdparty/civetweb/src/civetweb.c":
        "локальный снимок civetweb: не совпадает ни с одним тегом (макрос "
        "версии 1.16, но часть файлов из более новых коммитов)",
}


def run(cmd, cwd=None, verbose=False):
    res = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(
            f"команда не удалась: {' '.join(cmd)}\n{res.stderr.strip()}")
    if verbose and res.stdout.strip():
        print(res.stdout.strip())
    return res


def clone_repo(url, ref, dest, verbose=False):
    try:
        run(["git", "clone", "--depth", "1", "--branch", ref, url, str(dest)],
            verbose=verbose)
        return
    except RuntimeError as first_err:
        if verbose:
            print(f"  (shallow clone не удался, пробую fetch по SHA: "
                  f"{first_err})")
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    run(["git", "init", "-q", str(dest)])
    run(["git", "-C", str(dest), "remote", "add", "origin", url])
    run(["git", "-C", str(dest), "fetch", "--depth", "1", "origin", ref],
        verbose=verbose)
    run(["git", "-C", str(dest), "checkout", "-q", "FETCH_HEAD"])


def files_under(root):
    for path in sorted(root.rglob("*")):
        if path.is_file() and ".git" not in path.parts:
            yield path


def collect_pairs(up_path, local_path):
    """Пары (upstream, local) существующих локально файлов + лишние в апстриме."""
    pairs, missing_up, upstream_only = [], [], []
    if up_path.is_dir():
        if not local_path.is_dir():
            local_path.mkdir(parents=True)
        local_files = {p.relative_to(local_path) for p in files_under(local_path)}
        for rel in sorted(local_files):
            up = up_path / rel
            if up.is_file():
                pairs.append((up, local_path / rel))
            else:
                missing_up.append(local_path / rel)
        for up_file in files_under(up_path):
            rel = up_file.relative_to(up_path)
            if rel not in local_files:
                upstream_only.append(rel)
    else:
        if local_path.exists():
            pairs.append((up_path, local_path))
        elif up_path.exists():
            upstream_only.append(local_path.name)
    return pairs, missing_up, upstream_only


def apply_lib(name, ref, args):
    if ref != REFS[name]:
        print(f"== {name} (pin {REFS[name]} -> {ref}) ==")
    else:
        print(f"== {name} (pin {REFS[name]}) ==")
    with tempfile.TemporaryDirectory(prefix=f"vendored-{name}-") as tmp:
        tmp_dir = Path(tmp)
        repo_dir = tmp_dir / "repo"
        clone_repo(GIT_REPOS[name], ref, repo_dir, verbose=args.verbose)
        sha = run(["git", "-C", str(repo_dir), "rev-parse", "HEAD"]).stdout.strip()
        print(f"  upstream HEAD: {sha}")

        if name == "cpp-httplib":
            out_dir = tmp_dir / "out"
            out_dir.mkdir()
            run([sys.executable, str(repo_dir / "split.py"), "-o", str(out_dir)],
                verbose=args.verbose)
            root = out_dir
        else:
            root = repo_dir
        for src_rel, dst_rel in KEEP[name]:
            src = root / src_rel
            if not src.exists():
                raise RuntimeError(f"в апстриме {ref} отсутствует {src_rel}")
            local_dst = SRC_DIR / dst_rel
            if src.is_dir() and not local_dst.is_dir():
                local_dst.mkdir(parents=True)
            elif not local_dst.exists():
                local_dst.parent.mkdir(parents=True, exist_ok=True)
            pairs, missing_up, upstream_only = collect_pairs(src, local_dst)

            same = changed = written = skipped = 0
            for up_file, local_file in pairs:
                rel = local_file.relative_to(SRC_DIR)
                if filecmp.cmp(up_file, local_file, shallow=False):
                    same += 1
                    continue
                changed += 1
                note = PATCHED.get(str(rel))
                apply_to = args.apply and not args.dry_run
                if note and apply_to and not args.force:
                    skipped += 1
                    if args.verbose:
                        print(f"  [~] changed {rel} [local patch: {note}, "
                              f"пропущено; нужно --force]")
                    continue
                if apply_to:
                    shutil.copy2(up_file, local_file)
                    written += 1
                if args.verbose:
                    label = f" [local patch: {note}]" if note else ""
                    print(f"  [~] changed {rel}{label}")
            pruned = 0
            if args.apply and args.prune and not args.dry_run:
                for local_file in missing_up:
                    if (str(local_file.relative_to(SRC_DIR)) in PATCHED
                            and not args.force):
                        continue
                    local_file.unlink()
                    pruned += 1
                    if args.verbose:
                        print(f"  [-] removed {local_file.relative_to(SRC_DIR)}")
            summary = (
                f"  -> {same} same, {changed} changed, {len(missing_up)} "
                f"missing-upstream, {len(upstream_only)} upstream-only "
                f"(вне минимального набора)"
            )
            if args.apply and not args.dry_run:
                summary += f", {written} записано, {skipped} патчей пропущено"
            if pruned:
                summary += f", {pruned} удалено"
            print(summary)
            args._written += written
            args._skipped += skipped
    return args._written


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true",
                        help="только отчёт, даже при --ref не писать")
    parser.add_argument("--force", action="store_true",
                        help="в апгрейде перезаписывать и локальные патчи (PATCHED)")
    parser.add_argument("--prune", action="store_true",
                        help="в апгрейде удалять локальные файлы, отсутствующие "
                             "в апстриме")
    parser.add_argument("--only", choices=LIB_NAMES,
                        help="обработать только одну либу")
    parser.add_argument("--ref", action="append", default=[],
                        metavar="NAME=REF",
                        help="апгрейд либы: переопределить ref и применить "
                             "изменения (если не --dry-run)")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="печатать каждый изменившийся файл")
    args = parser.parse_args()
    args.apply = bool(args.ref)
    args._written = 0
    args._skipped = 0

    if not shutil.which("git"):
        print("git не найден", file=sys.stderr)
        return 1

    refs = dict(REFS)
    for item in args.ref:
        name, _, ref = item.partition("=")
        if name not in refs:
            parser.error(f"неизвестная либа {name!r} (выбирайте из {LIB_NAMES})")
        refs[name] = ref

    for name in LIB_NAMES:
        if args.only and name != args.only:
            continue
        try:
            apply_lib(name, refs[name], args)
        except RuntimeError as exc:
            print(f"  ERROR: {exc}", file=sys.stderr)
            return 1

    if args.apply and not args.dry_run:
        msg = (f"\n> Записано {args._written} файлов."
               if args._written else "\n> Файлы не изменялись.")
        if args._skipped:
            msg += (f" {args._skipped} локальных патчей пропущено "
                    f"(для перезаписи --force).")
        msg += (" Далее по AGENTS.md: ./rebuild-and-run.sh + message_counter.py "
                "+ запись в HISTORY.md (и VENDORED-LIBS.md при смене пинов).")
        print(msg)
    else:
        print("\n> Только отчёт: файлы не изменялись. Чтобы применить апгрейд, "
              f"укажите --ref NAME=REF (для патченных файлов ещё --force).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

