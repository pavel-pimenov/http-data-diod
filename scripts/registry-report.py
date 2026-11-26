#!/usr/bin/env python3
"""Отчёт по репозиториям Artifactory Docker-реестров.

Читает список Artifactory repo-ключей из config/registries.json, для каждого
подключается через Docker Registry API v2
(https://repos.dp.nlmk.com/artifactory/api/docker/<key>/v2/), суммирует
размеры слоёв всех тегов и выводит суммарный размер в ГБ.

Использование:
    python3 scripts/registry-report.py [--config config/registries.json]

Креды берутся из env по user_env/password_env из конфига (или дефолтные
datp-replication-ro).
"""

import json
import os
import ssl
import base64
import argparse
import urllib.request
import urllib.error


def api(base, user, password, path, accept=None):
    """Запрос к Registry API v2 через Artifactory. Возвращает dict."""
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    req = urllib.request.Request(f"https://{base}{path}")
    auth = base64.b64encode(f"{user}:{password}".encode()).decode()
    req.add_header("Authorization", f"Basic {auth}")
    if accept:
        req.add_header("Accept", accept)
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=30) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        return {"error": f"HTTP {e.code}"}
    except Exception as e:
        return {"error": str(e)}


# Media types пробуются по очереди: некоторые репозитории хранят манифесты в
# OCI-формате (vnd.oci.image.manifest), а не Docker v2 — без правильного Accept
# registry отвечает 404 даже при полных read-правах.
MEDIA_TYPES = [
    "application/vnd.docker.distribution.manifest.v2+json",
    "application/vnd.oci.image.manifest.v1+json",
    "application/vnd.docker.distribution.manifest.list.v2+json",
    "application/vnd.oci.image.index.v1+json",
    "*/*",
]

ARTIFACTORY_HOST = "repos.dp.nlmk.com/artifactory"


def _manifest(base, user, password, repo, tag):
    """Возвращает манифест (слои или список) независимо от media type."""
    for mt in MEDIA_TYPES:
        m = api(base, user, password, f"/v2/{repo}/manifests/{tag}", mt)
        if "error" in m:
            continue
        if "layers" in m or "manifests" in m:
            return m
    return {}


def layer_sizes(base, user, password, repo, tag):
    """Суммарный размер слоёв тега (manifest v2 / OCI). 0 если манифест недоступен."""
    m = _manifest(base, user, password, repo, tag)
    if not m:
        return 0
    # OCI index / manifest list (multi-arch): суммируем слои всех платформ
    if "manifests" in m:
        total = 0
        for sub in m.get("manifests", []):
            digest = sub.get("digest", "")
            sm = api(base, user, password,
                     f"/v2/{repo}/manifests/{digest}",
                     "application/vnd.docker.distribution.manifest.v2+json")
            if "layers" not in sm:
                sm = api(base, user, password,
                         f"/v2/{repo}/manifests/{digest}",
                         "application/vnd.oci.image.manifest.v1+json")
            if "layers" in sm:
                total += sum(int(l.get("size", 0)) for l in sm.get("layers", []))
        return total
    return sum(int(l.get("size", 0)) for l in m.get("layers", []))


def report_repo_key(repo_key, user, password):
    """Отчёт по одному Artifactory repo-ключу. Возвращает (rows, total)."""
    base = f"{ARTIFACTORY_HOST}/api/docker/{repo_key}"
    catalog = api(base, user, password, "/v2/_catalog")
    reps = catalog.get("repositories", [])

    rows = []
    grand_total = 0
    for repo in sorted(reps):
        tl = api(base, user, password, f"/v2/{repo}/tags/list")
        tags = tl.get("tags") or []
        repo_size = 0
        tag_rows = []
        for t in sorted(tags):
            s = layer_sizes(base, user, password, repo, t)
            repo_size += s
            tag_rows.append({"tag": t, "size": s})
            grand_total += s
        rows.append({"repo": repo, "tags": tags, "tag_rows": tag_rows,
                     "size": repo_size})

    rows.sort(key=lambda r: r["size"], reverse=True)
    return rows, grand_total


def load_config(path="config/registries.json"):
    """Читает config/registries.json. Возвращает (repo_keys, user, password)."""
    with open(path) as f:
        cfg = json.load(f)
    user = os.getenv(cfg.get("user_env", "REGISTRY_IA_URSU_USER"),
                     "datp-replication-ro")
    password = os.getenv(cfg.get("password_env", "REGISTRY_IA_URSU_PASSWORD"),
                         "az1evfb7YwwF8l9ed")
    return cfg.get("repos", []), user, password


def main():
    parser = argparse.ArgumentParser(description="Artifactory docker registry report")
    parser.add_argument("--config", default="config/registries.json")
    parser.add_argument("--user", default=None)
    parser.add_argument("--password", default=None)
    args = parser.parse_args()

    repo_keys, cfg_user, cfg_pass = load_config(args.config)
    user = args.user or cfg_user
    password = args.password or cfg_pass

    grand_total = 0
    print(f"Artifactory repo-ключей: {len(repo_keys)}\n")
    for key in repo_keys:
        rows, total = report_repo_key(key, user, password)
        print(f"{key}: {len(rows)} репозиториев, "
              f"{total // 1024 // 1024} MB ({total // 1024 // 1024 // 1024} GB)")
        grand_total += total

    print(f"\nИТОГО: {grand_total // 1024 // 1024} MB "
          f"({grand_total // 1024 // 1024 // 1024} GB)")


if __name__ == "__main__":
    main()
