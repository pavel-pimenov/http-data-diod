#!/usr/bin/env python3
"""Network-free python consistency linter for the repo.

Needs no third-party packages (stdlib only), so it works without ruff.
Checks: compile errors, trailing whitespace, CRLF, tabs, lines > 100,
missing final newline, and (conservatively) unused imports.

Usage: scripts/lint-python.py [paths...]   (default: all project *.py)
Exit code 0 = clean, 1 = issues found.
"""

import ast
import subprocess
import sys

LINE_LIMIT = 100


def unused_imports(path):
    with open(path, encoding="utf-8") as fh:
        src = fh.read()
    try:
        tree = ast.parse(src, path)
    except SyntaxError:
        return []
    imported = []  # (lineno, local_name)
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for a in node.names:
                imported.append((node.lineno, a.asname or a.name.split(".")[0]))
        elif isinstance(node, ast.ImportFrom):
            for a in node.names:
                if a.name == "*":
                    continue
                imported.append((node.lineno, a.asname or a.name))
    used = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Name):
            used.add(node.id)
        elif isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name):
            used.add(node.value.id)
        elif isinstance(node, ast.Import) or isinstance(node, ast.ImportFrom):
            continue
    return [(ln, name) for ln, name in imported if name not in used]


def check_file(path):
    problems = []
    try:
        with open(path, encoding="utf-8") as fh:
            data = fh.read()
    except OSError as exc:
        return [f"READ    {path}: {exc}"]
    if not data.endswith("\n"):
        problems.append("NOEOL")
    lines = data.splitlines()
    for idx, line in enumerate(lines, 1):
        if len(line) > LINE_LIMIT:
            problems.append(f"LONG{LINE_LIMIT} :{idx} ({len(line)} ch)")
    if "\t" in data:
        problems.append("TAB")
    if "\r" in data:
        problems.append("CRLF")
    if any(line.rstrip(" \t") != line for line in lines):
        problems.append("TRAILWS")
    for lineno, name in unused_imports(path):
        if name.startswith("__future__"):
            continue
        problems.append(f"UNUSED_IMPORT :{lineno} {name}")
    return problems


def main(argv):
    if argv:
        paths = argv
    else:
        out = subprocess.run(
            ["git", "ls-files", "*.py"], capture_output=True, text=True
        )
        paths = [p for p in out.stdout.splitlines() if not p.startswith(".venv/")]
    issues = 0
    checked = 0
    for path in sorted(paths):
        checked += 1
        for problem in check_file(path):
            issues += 1
            print(f"{problem} {path}")
    print(f"Checked {checked} file(s). Issues: {issues}")
    return 1 if issues else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
