#!/usr/bin/env python3
"""Docker build-context gate: files referenced by src/Dockerfile vs .dockerignore.

Reconstructs the effective build context for ``./src`` (the files git tracks,
with ``src/.dockerignore`` applied, docker last-match semantics) and asserts
that every local file/dir the Dockerfile references (COPY/ADD sources and
``./script.sh`` invocations in RUN/CMD) is present in it. This catches the
regression class where a pattern like ``*.sh`` silently drops a script the
builder needs (e.g. ``generate_version.sh``).

Run: python3 scripts/docker-context-check.py
Exit: 0 when all referenced context files survive .dockerignore, else 1.
"""

import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = REPO_ROOT / "src"
DOCKERFILE = SRC_DIR / "Dockerfile"
DOCKERIGNORE = SRC_DIR / ".dockerignore"

COPY_RE = re.compile(r"^\s*(COPY|ADD)\s+(.+)$", re.M)
RUN_LOCAL_SCRIPT_RE = re.compile(r"\./([A-Za-z0-9_.-]+\.(?:sh|py))\b")


def referenced_src_paths(dockerfile_text):
    """Local source files/dirs the Dockerfile needs from the build context.

    COPY/ADD sources are collected from the first whitespace token after the
    keyword, skipping ``--from=`` flags and absolute ``/path`` container paths
    (and ``.`` = whole context).  RUN/CMD/ENTRYPOINT are scanned only for
    ``./script.sh``/``./script.py`` invocations: the binaries the image builds
    (``./l2-proxy``, ``./test_components``) are never context files.
    """
    refs = set()
    for line in dockerfile_text.splitlines():
        match = COPY_RE.match(line)
        if match:
            for token in match.group(2).split():
                if token.startswith("--") or token.startswith("/"):
                    continue
                token = token.lstrip("./")
                if not token or token == ".":
                    continue
                refs.add(token.rstrip("/"))
            continue
        for match in RUN_LOCAL_SCRIPT_RE.finditer(line):
            refs.add(match.group(1))
    return sorted(refs)


def _pattern_regex(pattern):
    """A '**'-aware glob for a (supposedly) slash-normalised pattern."""
    out = []
    i = 0
    n = len(pattern)
    while i < n:
        c = pattern[i]
        if c == "*":
            if pattern[i:i + 2] == "**":
                out.append(".*")
                i += 2
            else:
                out.append("[^/]*")
                i += 1
        elif c == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(c))
            i += 1
    return "^" + "".join(out) + "$"


def _pattern_matches(pattern, relpath):
    """Match one dockerignore pattern against a slash-normalised relpath.

    - A trailing ``/`` marks a directory pattern: it matches the directory and
      everything under it, root-relative.
    - A pattern containing a ``/`` is matched against the full relative path
      from the context root.
    - A pattern with no ``/`` matches any single path *segment* (docker matches
      file/dir names at any depth, e.g. ``*.sh`` anywhere, bare ``build`` and
      every ``build`` dir in the tree).
    """
    dir_only = pattern.endswith("/")
    if dir_only:
        pattern = pattern[:-1]
    regex = re.compile(_pattern_regex(pattern))
    if "/" in pattern:
        if regex.match(relpath):
            return True
        if dir_only and regex.match(relpath.rsplit("/", 1)[0]):
            return True
        return False
    if dir_only:
        # Trailing-slash bare name: root-relative directory pattern.
        return bool(regex.match(relpath.split("/", 1)[0]))
    for segment in relpath.split("/"):
        if regex.match(segment):
            return True
    return False


def excluded_by_ignore(ignore_lines, relpath, is_dir=False):
    """docker semantics: last matching pattern wins; '!' re-includes."""
    result = False
    for line in ignore_lines:
        if line.startswith("!"):
            if _pattern_matches(line[1:], relpath):
                result = False
        elif _pattern_matches(line, relpath):
            result = True
    return result


def apply_ignore(ignore_lines, tracked_paths):
    """Return the subset of tracked_paths that survives .dockerignore."""
    kept = []
    for relpath in tracked_paths:
        if not excluded_by_ignore(ignore_lines, relpath):
            kept.append(relpath)
    return kept


def dir_present(context, prefix):
    """True if any tracked path lives under prefix (dir reference exists)."""
    return any(p.startswith(prefix.rstrip("/") + "/") for p in context)


def main() -> int:
    ignore_spec = DOCKERIGNORE.read_text(errors="ignore").splitlines()
    ignore_lines = [ln.strip() for ln in ignore_spec
                    if ln.strip() and not ln.lstrip().startswith("#")]
    dockerfile_text = DOCKERFILE.read_text(errors="ignore")

    tracked = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "ls-files", "src"],
        capture_output=True, text=True, check=True).stdout.splitlines()
    relpaths = sorted(p.removeprefix("src/") for p in tracked
                      if "/src/" in "/" + p or p.startswith("src/"))
    relpaths = [p for p in relpaths if p]
    context = apply_ignore(ignore_lines, relpaths)

    missing = []
    context_prefixes = set()
    for p in context:
        context_prefixes.add(p.rsplit("/", 1)[0] if "/" in p else ".")
        context_prefixes.add(".")
    for ref in referenced_src_paths(dockerfile_text):
        ref = ref.lstrip("./")
        if not ref:
            continue
        ref = ref.rstrip("/")
        if ref in {".", ""}:
            continue
        present = (ref in context or any(p.startswith(ref + "/")
                                          for p in context))
        if not present:
            missing.append(ref)

    print(f"docker-context: {len(context)} files tracked"
          f" ({len(relpaths) - len(context)} excluded by .dockerignore), "
          f"{len(set(referenced_src_paths(dockerfile_text)))} refs in Dockerfile")
    if missing:
        print("FAIL: Dockerfile references missing from the build context:")
        for ref in missing:
            print(f"  - {ref}  (excluded by .dockerignore or not tracked)")
        return 1
    print("OK: every Dockerfile context reference survives .dockerignore")
    return 0


if __name__ == "__main__":
    sys.exit(main())