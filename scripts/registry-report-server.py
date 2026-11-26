#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Веб-сервер отчётов по Docker-реестрам (registry-report-server).

Собирает и показывает отчёты о репозиториях Docker-реестров (размер, теги),
сохраняет их на волюм и отдаёт HTML-страницу с выбором реестра, генерацией
отчёта и списком сохранённых проверок с датой.

Реестры и креды берутся из env по схеме:
    REGISTRY_<ИМЯ>_URL      — адрес реестра (напр. ia-ursu-docker-prod.dp.nlmk.com)
    REGISTRY_<ИМЯ>_USER     — read-only учётка
    REGISTRY_<ИМЯ>_PASSWORD — пароль

Отчёты сохраняются в каталог из env REPORT_DIR (default /data, волюм).

Запуск:
  python3 scripts/registry-report-server.py
  env: REPORT_SERVER_PORT (default 8083), REPORT_DIR (default /data)
"""
import json
import os
import ssl
import html
import datetime
import base64
import argparse
import threading
import urllib.request
import urllib.error
from concurrent.futures import ThreadPoolExecutor

from flask import Flask, jsonify, render_template_string, request, send_from_directory

# Media types пробуются по очереди: часть репозиториев хранит манифесты в
# OCI-формате (vnd.oci.image.manifest), а не Docker v2 — без правильного Accept
# registry отвечает 404 даже при полных read-правах.
MEDIA_TYPES = [
    "application/vnd.docker.distribution.manifest.v2+json",
    "application/vnd.oci.image.manifest.v1+json",
    "application/vnd.docker.distribution.manifest.list.v2+json",
    "application/vnd.oci.image.index.v1+json",
    "*/*",
]

REPORT_DIR = os.getenv("REPORT_DIR", "/data")
PORT = int(os.getenv("REPORT_SERVER_PORT", "8083"))

app = Flask(__name__)


ARTIFACTORY_HOST = "repos.dp.nlmk.com/artifactory"
CONFIG_PATH = os.getenv("REPORT_CONFIG", "config/registries.json")


def _load_config():
    """Читает config/registries.json: список Artifactory repo-ключей + креды.

    Возвращает (repo_keys, user, password). Креды из env (user_env/password_env).
    """
    cfg = {}
    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH) as f:
            cfg = json.load(f)
    user = os.getenv(cfg.get("user_env", "REGISTRY_IA_URSU_USER"),
                     "datp-replication-ro")
    password = os.getenv(cfg.get("password_env", "REGISTRY_IA_URSU_PASSWORD"),
                         "az1evfb7YwwF8l9ed")
    return cfg.get("repos", []), user, password


def _registry_cfgs():
    """Список реестров из конфига: [{'name': key, ...}]."""
    keys, user, password = _load_config()
    return [{"name": key, "user": user, "password": password} for key in keys]


def _api(base, user, password, path, accept=None):
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


def _manifest(base, user, password, repo, tag):
    """Манифест (слои или список) независимо от media type."""
    for mt in MEDIA_TYPES:
        m = _api(base, user, password, f"/v2/{repo}/manifests/{tag}", mt)
        if "error" in m:
            continue
        if "layers" in m or "manifests" in m:
            return m
    return {}


def _layer_details(base, user, password, repo, tag):
    """Слои тега: [(digest, size)] и суммарный размер (Docker v2 / OCI).

    Возвращает (total_size, layers), где layers — список {digest, size}.
    Пустые, если манифест недоступен.
    """
    m = _manifest(base, user, password, repo, tag)
    if not m:
        return 0, []
    layers = []
    if "manifests" in m:
        for sub in m.get("manifests", []):
            digest = sub.get("digest", "")
            sm = _api(base, user, password, f"/v2/{repo}/manifests/{digest}",
                      "application/vnd.docker.distribution.manifest.v2+json")
            if "layers" not in sm:
                sm = _api(base, user, password, f"/v2/{repo}/manifests/{digest}",
                          "application/vnd.oci.image.manifest.v1+json")
            for l in sm.get("layers", []):
                layers.append({"digest": l.get("digest", ""),
                               "size": int(l.get("size", 0))})
        total = sum(l["size"] for l in layers)
        return total, layers
    for l in m.get("layers", []):
        layers.append({"digest": l.get("digest", ""),
                       "size": int(l.get("size", 0))})
    total = sum(l["size"] for l in layers)
    return total, layers


def _layer_sizes(base, user, password, repo, tag):
    """Суммарный размер слоёв тега (Docker v2 / OCI). 0 если манифест недоступен."""
    total, _ = _layer_details(base, user, password, repo, tag)
    return total


def generate_report(reg_cfg, repo_filter=None, out_dir=None):
    """Собирает отчёт по Artifactory repo-ключу. Возвращает dict-отчёт."""
    key, user, password = reg_cfg["name"], reg_cfg["user"], reg_cfg["password"]
    base = f"{ARTIFACTORY_HOST}/api/docker/{key}"
    catalog = _api(base, user, password, "/v2/_catalog")
    repositories = catalog.get("repositories", [])

    if repo_filter:
        repositories = [r for r in repositories if repo_filter in r]

    rows = []
    grand_total = 0
    for repo in sorted(repositories):
        tl = _api(base, user, password, f"/v2/{repo}/tags/list")
        tags = tl.get("tags") or []
        repo_size = 0
        tag_rows = []
        for t in sorted(tags):
            s, layers = _layer_details(base, user, password, repo, t)
            repo_size += s
            tag_rows.append({"tag": t, "size": s, "layers": layers})
        rows.append({"repo": repo, "tags": tags, "tag_rows": tag_rows,
                     "size": repo_size})
        grand_total += repo_size

    rows.sort(key=lambda r: r["size"], reverse=True)
    return {
        "registry": f"{ARTIFACTORY_HOST}/api/docker/{key}",
        "repo_key": key,
        "generated_at": datetime.datetime.now().isoformat(),
        "repo_count": len(rows),
        "total_size": grand_total,
        "repos": rows,
    }


# Теги, которые всегда считаются «текущей» версией (вне зависимости от даты).
_CURRENT_TAG_PREFIXES = ("latest", "stable", "prod", "current", "master", "main")


def _tag_sort_key(tag):
    """Ключ сортировки тега: чем больше — тем «новее» (кандидат на keep).

    Приоритет:
    1. latest/stable/prod/current/master/main — «текущие» (максимальный ключ).
    2. Временная метка YYYYMMDDHHMMSS (или YYYYMMDD) — по дате (растёт со временем).
    3. Семантическая версия vMAJOR.MINOR.PATCH (или 1.2.3) — по числовым компонентам.
    4. Прочее — лексикографически по имени.
    """
    low = tag.lower()

    # «текущие» маркеры — выше всех
    for i, prefix in enumerate(_CURRENT_TAG_PREFIXES):
        if low == prefix or low.startswith(prefix):
            # максимально возможный ключ, упорядоченный по порядку префикса
            return (3, 10_000 - i, "")

    # YYYYMMDDHHMMSS / YYYYMMDD
    digits = "".join(ch for ch in tag if ch.isdigit())
    if 8 <= len(digits) <= 14:
        try:
            if len(digits) >= 8:
                year = int(digits[:4])
                if 2000 <= year <= 2100:
                    return (2, int(digits[:14].ljust(14, "0")), low)
        except ValueError:
            pass

    # vMAJOR.MINOR.PATCH / MAJOR.MINOR.PATCH
    ver = tag[1:] if low.startswith("v") else tag
    parts = ver.split(".")
    if 2 <= len(parts) <= 4 and all(p.isdigit() for p in parts):
        return (1, tuple(int(p) for p in parts), low)

    # лексикографически
    return (0, 0, low)


def _cleanup_recommendations(repos):
    """Кандидаты на удаление тегов по правилу «текущая + предыдущая».

    В каждом репозитории теги сортируются по `_tag_sort_key`: самый новый —
    «текущая» версия, второй — «предыдущая», все остальные — кандидаты на
    удаление. Теги latest/stable/prod считаются «текущими».

    Возвращает список {repo, keep, candidates} с суммарным объёмом.
    """
    out = []
    for r in repos:
        tags = sorted(r.get("tag_rows", []),
                      key=lambda t: _tag_sort_key(t["tag"]))
        keep = tags[-2:] if len(tags) >= 2 else tags
        keep_set = {t["tag"] for t in keep}
        candidates = [t for t in tags if t["tag"] not in keep_set]
        if candidates:
            total = sum(t["size"] for t in candidates)
            # Уникальный объём слоёв кандидатов: слои с одинаковым digest
            # дублируются между тегами, поэтому реально освобождается не сумма
            # размеров тегов, а только те слои, что не входят в keep-теги.
            unique = {}
            keep_digests = set()
            for k in keep:
                for l in k.get("layers", []):
                    if l.get("digest"):
                        keep_digests.add(l["digest"])
            for c in candidates:
                for l in c.get("layers", []):
                    d = l.get("digest")
                    if d and d not in keep_digests:
                        unique[d] = l.get("size", 0)
            out.append({"repo": r["repo"], "keep": [t["tag"] for t in keep],
                        "candidates": [t["tag"] for t in candidates],
                        "candidate_size": total,
                        "candidate_unique_size": sum(unique.values())})
    out.sort(key=lambda c: c["candidate_unique_size"], reverse=True)
    return out


def _save_report(reg_cfg, report, repo_filter=None):
    """Сохраняет отчёт в REPORT_DIR. Имя файла = имя репозитория (repo_key).

    Возвращает имя html-файла.
    """
    os.makedirs(REPORT_DIR, exist_ok=True)
    name = report.get("repo_key", reg_cfg["name"])
    ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    suffix = f"-{repo_filter.replace('/', '-')}" if repo_filter else ""
    base = f"{name}{suffix}-{ts}"

    md = _render_md(report, repo_filter)
    md_path = os.path.join(REPORT_DIR, f"{base}.md")
    with open(md_path, "w", encoding="utf-8") as f:
        f.write(md)

    txt = _render_txt(report, repo_filter)
    txt_path = os.path.join(REPORT_DIR, f"{base}.txt")
    with open(txt_path, "w", encoding="utf-8") as f:
        f.write(txt)

    html = _render_html(report, repo_filter)
    html_path = os.path.join(REPORT_DIR, f"{base}.html")
    with open(html_path, "w", encoding="utf-8") as f:
        f.write(html)

    json_path = os.path.join(REPORT_DIR, f"{base}.json")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)

    return f"{base}.html"


def _render_md(report, repo_filter=None):
    """Markdown-отчёт (аналог docs/registry-report.md)."""
    lines = [f"# Отчёт по Docker registry `{report['registry']}`",
             "",
             f"Дата: {report['generated_at']}",
             f"Скрипт: scripts/registry-report-server.py",
             ""]
    if repo_filter:
        lines.append(f"Фильтр: `{repo_filter}`")
        lines.append("")
    lines.append("## Сводка")
    lines.append("")
    lines.append(f"Всего репозиториев: {report['repo_count']}. "
                 f"**ИТОГО: ~{report['total_size'] // 1024 // 1024} MB "
                 f"({report['total_size'] // 1024 // 1024 // 1024} GB).**")
    lines.append("")
    lines.append("| Репозиторий | Тегов | Размер (MB) |")
    lines.append("|---|---|---|")
    for r in report["repos"]:
        lines.append(f"| {r['repo']} | {len(r['tags'])} | {r['size'] // 1024 // 1024} |")
    lines.append("")
    lines.append("## Детализация по-тегово (> 10 MB)")
    lines.append("")
    for r in report["repos"]:
        if r["size"] // 1024 // 1024 > 10:
            lines.append(f"### {r['repo']} ({r['size'] // 1024 // 1024} MB)")
            lines.append("")
            lines.append("| Тег | MB |")
            lines.append("|---|---|")
            for t in r["tag_rows"]:
                lines.append(f"| {t['tag']} | {t['size'] // 1024 // 1024} |")
            lines.append("")
    return "\n".join(lines)


def _render_txt(report, repo_filter=None):
    """Текстовый отчёт по слоям и размерам + кандидаты на удаление.

    Чистый ASCII-текст без markdown/HTML — удобно копировать в буфер обмена.
    """
    def mb(b):
        return f"{b // 1024 // 1024} MB"

    L = []
    L.append("=" * 70)
    L.append(f"ОТЧЁТ ПО DOCKER REGISTRY: {report['registry']}")
    L.append(f"Дата: {report['generated_at']}")
    L.append(f"Всего репозиториев: {report['repo_count']}")
    L.append(f"ИТОГО: {mb(report['total_size'])} "
             f"({report['total_size'] // 1024 // 1024 // 1024} GB)")
    if repo_filter:
        L.append(f"Фильтр: {repo_filter}")
    L.append("=" * 70)
    L.append("")

    for r in report["repos"]:
        L.append(f"### {r['repo']}  ({mb(r['size'])}, тегов: {len(r['tags'])})")
        L.append("-" * 70)
        for t in r.get("tag_rows", []):
            L.append(f"  ТЕГ {t['tag']}  [{mb(t['size'])}]")
            for l in t.get("layers", []):
                L.append(f"      {l['digest']}  {mb(l['size'])}")
        L.append("")

    # Рекомендации по удалению
    L.append("=" * 70)
    L.append("РЕКОМЕНДАЦИИ ПО УДАЛЕНИЮ")
    L.append("Правило: оставляем текущую и предыдущую версию тега,")
    L.append("всё, что старее — кандидаты на удаление.")
    L.append("=" * 70)
    recs = _cleanup_recommendations(report["repos"])
    total_candidates = sum(c["candidate_size"] for c in recs)
    total_unique = sum(c.get("candidate_unique_size", 0) for c in recs)
    L.append(f"Всего кандидатов на удаление: ~{mb(total_candidates)}")
    L.append(f"Из них уникальных слоёв (реально освободится): "
             f"~{mb(total_unique)}")
    L.append("")
    for c in recs:
        L.append(f"### {c['repo']}")
        L.append(f"  Оставить: {', '.join(c['keep'])}")
        uniq = c.get("candidate_unique_size")
        if uniq and uniq != c["candidate_size"]:
            L.append(f"  Кандидаты на удаление (~{mb(c['candidate_size'])} "
                     f"суммарно, ~{mb(uniq)} уникальных слоёв):")
        else:
            L.append(f"  Кандидаты на удаление (~{mb(c['candidate_size'])}):")
        for t in c["candidates"]:
            L.append(f"      - {t}")
        L.append("")
    return "\n".join(L)


def _render_html(report, repo_filter=None):
    """Самодостаточный HTML-отчёт (тёмная тема, таблицы, бары по размерам)."""
    rows = report["repos"]
    total_mb = report["total_size"] // 1024 // 1024
    total_gb = total_mb // 1024

    summary_rows = "".join(
        f"<tr><td>{r['repo']}</td><td>{len(r['tags'])}</td>"
        f"<td>{r['size'] // 1024 // 1024}</td></tr>" for r in rows)

    max_mb = max((r["size"] // 1024 // 1024 for r in rows), default=1)
    bar_rows = ""
    for r in rows:
        mb = r["size"] // 1024 // 1024
        pct = max(mb / max_mb * 100, 2) if max_mb else 0
        bar_rows += (f'<div class="br"><div class="bl" title="{r["repo"]}">{r["repo"]}</div>'
                     f'<div class="bt"><div class="bf" style="width:{pct:.0f}%"></div></div>'
                     f'<div class="bv">{mb} MB</div></div>')

    tag_rows = "".join(
        f"<tr><td>{r['repo']}</td><td>{len(r['tags'])}</td>"
        f"<td>{', '.join(t['tag'] for t in r['tag_rows'][:5])}</td></tr>"
        for r in rows if r["size"] // 1024 // 1024 > 10)

    filter_note = f"<p>Фильтр: <code>{repo_filter}</code></p>" if repo_filter else ""

    # Текстовый отчёт (слои + рекомендации) — встраивается внизу HTML-отчёта,
    # чтобы его можно было выделить и скопировать в буфер обмена.
    txt_report = _render_txt(report, repo_filter)

    return f"""<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Отчёт {report['registry']}</title>
<style>
  body {{ margin:0; font-family:system-ui,Segoe UI,Roboto,sans-serif;
    background:#0f172a; color:#e2e8f0; }}
  header {{ padding:1.5rem 2rem; border-bottom:1px solid #334155; background:#0b1220; }}
  header h1 {{ margin:0; font-size:1.4rem; color:#38bdf8; }}
  header p {{ margin:6px 0 0; color:#94a3b8; font-size:0.85rem; }}
  main {{ padding:1.5rem 2rem; max-width:1100px; margin:0 auto; }}
  h2 {{ color:#fbbf24; border-left:4px solid #fbbf24; padding-left:10px; font-size:1.05rem; }}
  table {{ width:100%; border-collapse:collapse; margin-bottom:1.5rem; }}
  th,td {{ text-align:left; padding:0.5rem 0.75rem;
    border-bottom:1px solid #334155; font-size:0.9rem; }}
  th {{ color:#94a3b8; }}
  code {{ background:#334155; padding:0.1rem 0.4rem; border-radius:4px; }}
  .br {{ display:flex; align-items:center; margin:0.3rem 0; font-size:0.85rem; }}
  .bl {{ width:230px; flex-shrink:0; text-align:right; padding-right:0.6rem;
    white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }}
  .bt {{ flex:1; background:#334155; border-radius:4px; height:18px; }}
  .bf {{ height:18px; border-radius:4px; background:#38bdf8; }}
  .bv {{ width:100px; color:#94a3b8; padding-left:0.6rem; }}
  .total {{ color:#4ade80; font-weight:600; }}
  .txt-block {{ background:#0b1220; border:1px solid #334155; border-radius:8px;
    padding:1rem; overflow:auto; max-height:600px; }}
  .txt-block pre {{ margin:0; white-space:pre; font-family:monospace;
    color:#e2e8f0; font-size:0.82rem; line-height:1.4; }}
</style></head>
<body>
<header>
  <h1>Отчёт по Docker registry <code>{report['registry']}</code></h1>
  <p>Дата: {report['generated_at']} · Всего репозиториев: {report['repo_count']} ·
     <span class="total">~{total_mb} MB ({total_gb} GB)</span></p>
</header>
<main>
  {filter_note}
  <h2>Сводка</h2>
  <table><tr><th>Репозиторий</th><th>Тегов</th><th>Размер (MB)</th></tr>{summary_rows}</table>
  <h2>Распределение по размерам</h2>
  {bar_rows}
  <h2>Теги крупнейших (> 10 MB)</h2>
  <table><tr><th>Репозиторий</th><th>Тегов</th><th>Теги</th></tr>{tag_rows}</table>
  <h2>Текстовый отчёт (слои и размеры, копируется)</h2>
  <p style="color:#94a3b8;font-size:0.85rem">Выделите и скопируйте текст ниже
     (включая рекомендации по удалению старых тегов).</p>
  <div class="txt-block"><pre>{html.escape(txt_report)}</pre></div>
</main>
</body></html>"""


# ---------- Routes ----------

@app.after_request
def _no_cache_html(response):
    # UI — единый HTML со встроенным JS: без явных no-cache заголовков браузер
    # может отдавать закешированную старую страницу, из-за чего комбобокс
    # реестров/сводка «не заполняются», хотя сервер уже отдаёт новую версию.
    if response.mimetype == "text/html":
        response.headers["Cache-Control"] = "no-cache, no-store, must-revalidate"
        response.headers["Pragma"] = "no-cache"
        response.headers["Expires"] = "0"
    return response


@app.route("/")
def index():
    return render_template_string(HTML)


@app.route("/api/registries")
def api_registries():
    regs = _registry_cfgs()
    return jsonify([{"name": r["name"], "url": r["name"]} for r in regs])


@app.route("/api/repos")
def api_repos():
    name = request.args.get("registry", "")
    reg = _find_registry(name)
    if not reg:
        return jsonify({"error": "registry not found"}), 404
    base = f"{ARTIFACTORY_HOST}/api/docker/{name}"
    catalog = _api(base, reg["user"], reg["password"], "/v2/_catalog")
    return jsonify(catalog.get("repositories", []))


@app.route("/api/generate", methods=["POST"])
def api_generate():
    data = request.get_json(silent=True) or {}
    name = data.get("registry", "")
    repo_filter = data.get("repo", "") or None
    reg = _find_registry(name)
    if not reg:
        return jsonify({"error": "registry not found"}), 404

    result = {"status": "running", "id": None}

    def worker():
        try:
            report = generate_report(reg, repo_filter)
            md_file = _save_report(reg, report, repo_filter)
            result["status"] = "done"
            result["id"] = md_file
        except Exception as e:
            result["status"] = "error"
            result["error"] = str(e)

    threading.Thread(target=worker, daemon=True).start()
    return jsonify(result), 202


@app.route("/api/generate/<path:job>")
def api_generate_status(job):
    return jsonify({"status": "unknown"}), 404


@app.route("/api/reports")
def api_reports():
    if not os.path.isdir(REPORT_DIR):
        return jsonify([])
    files = []
    for fn in os.listdir(REPORT_DIR):
        if fn.endswith(".html"):
            path = os.path.join(REPORT_DIR, fn)
            stat = os.stat(path)
            txt_name = fn[:-len(".html")] + ".txt"
            files.append({
                "file": fn,
                "txt": txt_name if os.path.exists(
                    os.path.join(REPORT_DIR, txt_name)) else None,
                "size": stat.st_size,
                "date": datetime.datetime.fromtimestamp(
                    stat.st_mtime).isoformat(),
            })
    files.sort(key=lambda f: f["date"], reverse=True)
    return jsonify(files)


@app.route("/reports/<path:filename>")
def report_file(filename):
    return send_from_directory(REPORT_DIR, filename)


def _find_registry(name):
    for r in _registry_cfgs():
        if r["name"] == name:
            return r
    return None


# Кеш сводки: {timestamp, data}. Обновляется фоновым потоком раз в SUMMARY_TTL.
# _summary_building — флаг, что пересборка уже идёт: не даём плодить по потоку
# на каждый запрос, пока сводка считается (она дорогая — перечисляет все репы/теги).
SUMMARY_TTL = 600  # сек (10 мин)
_summary_cache = {"ts": 0, "data": None}
_summary_building = False
_summary_lock = threading.Lock()


def _build_summary():
    """Собирает размер по каждому repo-ключу из конфига + все внутренние репы.

    Запросы к Artifactory — сетевые и медленные, поэтому размеры репозиториев
    собираются параллельно (ThreadPoolExecutor) — ускоряет первую загрузку
    с ~60 с до нескольких секунд.
    """
    keys, user, password = _load_config()
    entries = []  # repo_key -> размер
    all_repos = []  # (repo_key, repo) -> размер, для "показать все"

    def repo_size(key, base, repo):
        """Размер репо + освобождаемый объём кандидатов на удаление.

        Возвращает (name, total, cand_size). cand_size — сумма размеров тегов,
        не входящих в keep (2 последних по `_tag_sort_key`), без дедупликации
        слоёв (для сводки достаточно знать «есть кандидаты и сколько»).
        """
        tl = _api(base, user, password, f"/v2/{repo}/tags/list")
        tags = tl.get("tags") or []
        tag_sizes = [(t, _layer_sizes(base, user, password, repo, t))
                     for t in tags]
        total = sum(s for _, s in tag_sizes)
        # keep: 2 последних по умной сортировке
        ordered = sorted(tag_sizes, key=lambda ts: _tag_sort_key(ts[0]))
        keep_set = {t for t, _ in ordered[-2:]}
        cand_size = sum(s for t, s in tag_sizes if t not in keep_set)
        return f"{key}/{repo}", total, cand_size

    for key in keys:
        base = f"{ARTIFACTORY_HOST}/api/docker/{key}"
        catalog = _api(base, user, password, "/v2/_catalog")
        reps = catalog.get("repositories", [])
        key_total = 0
        key_cand = 0
        key_repos = []
        # Параллельный сбор размеров по всем репозиториям ключа
        with ThreadPoolExecutor(max_workers=8) as pool:
            futures = [pool.submit(repo_size, key, base, r) for r in sorted(reps)]
            for fut in futures:
                name, total, cand = fut.result()
                key_total += total
                key_cand += cand
                key_repos.append({"name": name, "size": total,
                                  "cand_size": cand})
        entries.append({"key": key, "size": key_total,
                        "cand_size": key_cand})
        all_repos.extend(key_repos)
    entries.sort(key=lambda e: e["size"], reverse=True)
    all_repos.sort(key=lambda e: e["size"], reverse=True)
    return {"keys": entries, "all_repos": all_repos}


def _get_summary():
    """Кешированная сводка; обновляет в фоне если устарела.

    Возвращает (data, building): data — закешированная сводка (может быть None,
    если ещё ни разу не собрана), building — идёт ли фоновая пересборка сейчас.
    Повторные вызовы во время пересборки не плодят новые потоки (один воркер).
    """
    import time
    global _summary_building
    now = time.time()
    with _summary_lock:
        if now - _summary_cache["ts"] < SUMMARY_TTL and _summary_cache["data"]:
            return _summary_cache["data"], False
        if _summary_building:
            return _summary_cache["data"], True
        _summary_building = True

    # фоновая пересборка
    def worker():
        global _summary_building
        try:
            data = _build_summary()
            with _summary_lock:
                _summary_cache["ts"] = time.time()
                _summary_cache["data"] = data
        finally:
            with _summary_lock:
                _summary_building = False

    threading.Thread(target=worker, daemon=True).start()
    return _summary_cache["data"], True


@app.route("/api/summary")
def api_summary():
    data, building = _get_summary()
    if not data:
        return jsonify({"status": "building", "keys": [], "all_repos": []}), 202
    return jsonify({"status": "ready", "building": building, **data})


HTML = """<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Отчёты по Docker-реестрам</title>
<style>
  :root { --bg:#0f172a; --card:#1e293b; --text:#e2e8f0; --muted:#94a3b8; --accent:#38bdf8; }
  * { box-sizing:border-box; }
  body { margin:0; font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
         background:var(--bg); color:var(--text); }
  header { padding:1.5rem 2rem; border-bottom:1px solid #334155; }
  header h1 { margin:0; font-size:1.4rem; color:var(--accent); }
  main { padding:1.5rem 2rem; max-width:1200px; margin:0 auto; }
  .card { background:var(--card); border-radius:10px; padding:1.5rem; margin-bottom:1.5rem;
          border:1px solid #334155; }
  .card h2 { margin-top:0; font-size:1.1rem; color:var(--accent); }
  label { display:block; margin:0.75rem 0 0.25rem; color:var(--muted); font-size:0.85rem; }
  select, input { width:100%; padding:0.5rem 0.75rem; border-radius:6px; border:1px solid #475569;
        background:#0f172a; color:var(--text); font-size:0.95rem; }
  button { padding:0.6rem 1.2rem; border:none; border-radius:6px; background:var(--accent);
        color:#0f172a; font-weight:600; cursor:pointer; }
  button:disabled { opacity:0.5; cursor:not-allowed; }
  .row { display:flex; gap:1rem; flex-wrap:wrap; }
  .row > div { flex:1; min-width:240px; }
  #status { margin-top:0.75rem; color:var(--muted); font-size:0.9rem; }
  table { width:100%; border-collapse:collapse; }
  th, td { text-align:left; padding:0.5rem 0.75rem;
           border-bottom:1px solid #334155; font-size:0.9rem; }
  th { color:var(--muted); font-weight:600; }
  td a { color:var(--accent); text-decoration:none; }
  td a:hover { text-decoration:underline; }
  .chart-wrap { height:auto; margin-top:1rem; }
  .chart-wrap .bar-row { display:flex; align-items:center; margin:0.35rem 0; font-size:0.85rem; }
  .chart-wrap .bar-label { width:240px; flex-shrink:0; color:var(--text); white-space:nowrap;
       overflow:hidden; text-overflow:ellipsis; text-align:right; padding-right:0.6rem; }
  .chart-wrap .bar-track { flex:1; background:#334155; border-radius:4px;
       height:18px; position:relative; }
  .chart-wrap .bar-fill { height:18px; border-radius:4px; display:block; }
  .chart-wrap .bar-val { width:110px; flex-shrink:0; color:var(--muted); padding-left:0.6rem; }
  .bar-size { background:#38bdf8; }
  .bar-tag { background:#f472b6; }
</style>
</head>
<body>
<header><h1>Отчёты по Docker-реестрам</h1></header>
<main>
  <div class="card">
    <h2>Генерация отчёта</h2>
    <div class="row">
      <div>
        <label>Реестр</label>
        <select id="registry" onchange="loadRepos()"></select>
      </div>
      <div>
        <label>Репозиторий (фильтр, необязательно)</label>
        <input id="repo" list="repo-list" placeholder="напр. http-data-diod">
        <datalist id="repo-list"></datalist>
      </div>
    </div>
    <div style="margin-top:1rem">
      <button id="gen-btn" onclick="generate()" disabled>Сгенерировать отчёт</button>
    </div>
    <div id="status"></div>
  </div>

  <div class="card">
    <h2>Сохранённые отчёты</h2>
    <div id="reports"></div>
  </div>

  <div class="card">
    <h2>Распределение по размерам</h2>
    <label style="font-size:0.85rem;color:var(--muted)">
      <input type="checkbox" id="show-all" onchange="renderSummary()">
      Показать все (включая не из конфига)
    </label>
    <label style="font-size:0.85rem;color:var(--muted);margin-left:1rem">
      <input type="checkbox" id="show-candidates" onchange="renderSummary()">
      Только кандидаты на удаление (освобождаемый объём)
    </label>
    <div class="chart-wrap" id="sizeChart"></div>
  </div>
  <div class="card">
    <h2>Число тегов</h2>
    <div class="chart-wrap" id="tagChart"></div>
  </div>
</main>

<script>
let regs = [];

async function api(path, opts) {
  const r = await fetch(path, opts);
  return r.json();
}

function fmtMB(bytes) {
  return (bytes/1024/1024).toFixed(0) + ' MB';
}

function fmtSize(bytes) {
  // Человекочитаемый размер файла отчёта: B / KB / MB (не байты в MB — HTML
  // отчёта весит килобайты, поэтому в MB всегда выходит 0).
  if (bytes >= 1024*1024) return (bytes/1024/1024).toFixed(1) + ' MB';
  if (bytes >= 1024) return (bytes/1024).toFixed(0) + ' KB';
  return bytes + ' B';
}

function fmtDate(iso) {
  const d = new Date(iso);
  return d.toLocaleString('ru-RU');
}

async function loadRegistries() {
  regs = await api('api/registries');
  const sel = document.getElementById('registry');
  sel.innerHTML = regs.map(r => `<option value="${r.name}">${r.name} (${r.url})</option>`).join('');
  if (regs.length) loadRepos();
  document.getElementById('gen-btn').disabled = false;
}

async function loadRepos() {
  const name = document.getElementById('registry').value;
  const dl = document.getElementById('repo-list');
  const repos = await api('api/repos?registry=' + encodeURIComponent(name));
  dl.innerHTML = repos.map(r => `<option value="${r}">`).join('');
}

async function generate() {
  const name = document.getElementById('registry').value;
  const repo = document.getElementById('repo').value.trim();
  const btn = document.getElementById('gen-btn');
  const status = document.getElementById('status');
  btn.disabled = true;
  status.textContent = 'Генерация отчёта...';
  const res = await api('api/generate', {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify({registry:name, repo})
  });
  if (res.status !== 'running') {
    status.textContent = 'Ошибка: ' + (res.error||'unknown');
    btn.disabled = false;
    return;
  }
  // ждём завершения
  pollStatus(res, btn, status);
}

function pollStatus(res, btn, status) {
  const wait = setInterval(async () => {
    status.textContent = 'Генерация идёт...';
    // нет прямого статуса — проверяем появление файла
    const reports = await api('api/reports');
    if (reports.length) {
      clearInterval(wait);
      btn.disabled = false;
      status.textContent = 'Отчёт готов: ' + reports[0].file;
      renderReports(reports);
    }
  }, 1500);
  setTimeout(()=>{
    clearInterval(wait);
    btn.disabled = false;
    status.textContent = 'Таймаут генерации.';
  }, 120000);
}

async function loadReports() {
  const reports = await api('api/reports');
  renderReports(reports);
  renderSummary();
}

function renderReports(reports) {
  const el = document.getElementById('reports');
  if (!reports.length) {
    el.innerHTML = '<p style="color:var(--muted)">Отчётов пока нет</p>';
    return;
  }
  let html = '<table><tr><th>Файл</th><th>Дата</th><th>Размер</th><th></th></tr>';
  for (const r of reports) {
    html += `<tr><td>${r.file}</td><td>${fmtDate(r.date)}</td><td>${fmtSize(r.size)}</td>` +
            `<td><a href="reports/${r.file}" target="_blank">открыть</a></td></tr>`;
  }
  html += '</table>';
  el.innerHTML = html;
}

let summaryTimer = null;

async function renderSummary() {
  // Диаграмма «Распределение по размерам» — по всем repo-ключам из конфига.
  // Галочка «Показать все» добавляет репозитории, которых нет в конфиге.
  const el = document.getElementById('sizeChart');
  const showAll = document.getElementById('show-all').checked;
  const showCand = document.getElementById('show-candidates').checked;
  el.innerHTML = '<p style="color:var(--muted)">Собираю сводку...</p>';
  const data = await api('api/summary');
  // Сводка собирается в фоне и может быть дорогой (минуты): пока она не готова —
  // опрашиваем повторно, а не оставляем «Нет данных» после первой попытки.
  if (!data.keys || !data.keys.length) {
    if (summaryTimer) clearTimeout(summaryTimer);
    summaryTimer = setTimeout(renderSummary, 5000);
    el.innerHTML = '<p style="color:var(--muted)">Сводка считается, обновлюсь через 5 сек...</p>';
    return;
  }
  let items;
  if (showCand) {
    // Только репозитории с кандидатами на удаление; val = освобождаемый объём
    const src = showAll ? data.all_repos : data.keys;
    items = src
      .filter(r => (r.cand_size || 0) > 0)
      .map(r => ({label:r.name || r.key, val:Math.round((r.cand_size||0)/1024/1024)}));
  } else {
    items = showAll
      ? data.all_repos.map(r => ({label:r.name, val:Math.round(r.size/1024/1024)}))
      : data.keys.map(k => ({label:k.key, val:Math.round(k.size/1024/1024)}));
  }
  renderBars('sizeChart', items, showCand ? 'Освобождаемый объём, MB' : 'Размер, MB', 'bar-size');
}

function renderBars(elId, items, unit, cls) {
  const el = document.getElementById(elId);
  if (!el) return;
  if (!items.length) { el.innerHTML = '<p style="color:var(--muted)">Нет данных</p>'; return; }
  const max = Math.max(...items.map(i => i.val), 1);
  let html = '<div style="color:var(--muted);font-size:0.8rem;' +
             'margin-bottom:0.5rem">' + unit + '</div>';
  for (const it of items) {
    const pct = Math.max(it.val/max*100, 2);
    html += `<div class="bar-row">
        <div class="bar-label" title="${it.label}">${it.label}</div>
        <div class="bar-track"><div class="bar-fill ${cls}" style="width:${pct}%"></div></div>
        <div class="bar-val">${it.val}</div>
      </div>`;
  }
  el.innerHTML = html;
}

loadRegistries();
loadReports();
</script>
</body>
</html>
"""


def main():
    parser = argparse.ArgumentParser(description="Registry report server")
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--report-dir", default=REPORT_DIR)
    args = parser.parse_args()
    os.makedirs(args.report_dir, exist_ok=True)
    print(f"registry-report-server listening on :{args.port}, reports in {args.report_dir}")
    app.run(host="0.0.0.0", port=args.port)


if __name__ == "__main__":
    main()
