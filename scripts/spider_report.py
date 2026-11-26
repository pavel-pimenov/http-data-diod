#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
l2-db-spider report builder.

Анализирует JSON-файлы, сгенерированные l2-db-spider в <output_dir> (по
умолчанию ./spider-output), собирает структурированные таблицы, затем — при
наличии настроенной AI-модели — передаёт их в модель и получает связный
человекочитаемый отчёт с рекомендациями. Если AI недоступен, выводит только
таблицы (фоллбек).

Формат файлов (потоковая запись DbSpider::stream_output):
  {"generated_at":..., "db":..., "query":..., "status":"ok",
   "result":{"rows":[[...],...], "columns":[{"name":..,"type":..},...],
             "row_count":N}}
или на ошибке:
  {"status":"error","result":{"error":{"code":..,"message":..}}}

Запросы:
  shared_pool  — v$sqlarea (SQL-статистика: executions, cpu_time, elapsed_time,
                 buffer_gets, disk_reads, sorts, version_count, rows_processed, ...)
  table_sizes  — размеры сегментов (SEGMENT_NAME, SEGMENT_TYPE, BYTES)
  tablespaces  — табличные пространства (имя, размер, свободно)

AI-генерация отчёта (OpenAI-совместимый или Anthropic chat completions):
  AI_API_URL     — base URL, напр. https://api.openai.com/v1 (обязателен)
  AI_API_KEY     — API-ключ (обязателен)
  AI_PROVIDER    — "openai" | "anthropic" (default: openai)
  AI_MODEL       — имя модели (default: "" → gpt-4o / claude-sonnet-4)
  AI_TIMEOUT     — таймаут запроса, сек (default: 180)
  AI_TEMPERATURE — креативность 0..1 (default: 0.3)
  (значения берутся ТОЛЬКО из окружения — никаких адресов/ключей в коде)

Использование:
  python3 scripts/spider_report.py [--input-dir PATH] [--output-md PATH] [--top N] [--no-ai]
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from datetime import datetime


# --- утилиты -------------------------------------------------------------
def _to_int(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return 0


def _to_float(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return 0.0


def _fmt_bytes(b):
    b = _to_float(b)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if b < 1024.0 or unit == "TB":
            return "{:.1f} {}".format(b, unit)
        b /= 1024.0


def _fmt_dt(epoch_us):
    try:
        return datetime.fromtimestamp(_to_float(epoch_us) / 1_000_000.0).strftime(
            "%Y-%m-%d %H:%M:%S"
        )
    except (ValueError, OSError):
        return str(epoch_us)


# --- анализ одного файла --------------------------------------------------
def analyze_file(path):
    """Возвращает (meta, analysis) или (None, None) если файл не парсится."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            data = json.load(fh)
    except (json.JSONDecodeError, OSError) as e:
        return {"file": path, "parse_error": str(e)}, None

    meta = {
        "file": path,
        "db": data.get("db"),
        "query": data.get("query"),
        "status": data.get("status"),
        "generated_at": _fmt_dt(data.get("generated_at")),
    }
    result = data.get("result", {})
    if data.get("status") != "ok":
        err = result.get("error", {})
        meta["error"] = {"code": err.get("code"), "message": err.get("message")}
        return meta, None

    columns = result.get("columns", [])
    rows = result.get("rows", [])
    col_names = [c.get("name", "") for c in columns] if columns else []
    col_types = [c.get("type", "") for c in columns] if columns else []

    analysis = {
        "columns": col_names,
        "col_types": col_types,
        "row_count": result.get("row_count", len(rows)),
        "rows": rows,
    }
    return meta, analysis


# --- специфичный анализ по типам запросов ----------------------------------
def analyze_shared_pool(meta, analysis):
    """SQL-статистика: топ по нагрузке из v$sqlarea + источники вызова."""
    cols = analysis["columns"]
    idx = {name: i for i, name in enumerate(cols)}
    rows = analysis["rows"]

    def num(r, key):
        i = idx.get(key)
        return _to_int(r[i]) if i is not None and i < len(r) else 0

    def txt(r, key, default=""):
        i = idx.get(key)
        return str(r[i]) if i is not None and i < len(r) else default

    def src(r):
        """Источник вызова: MODULE / ACTION / PROGRAM_ID:PROGRAM_LINE# / schema."""
        module = txt(r, "MODULE")
        action = txt(r, "ACTION")
        prog = txt(r, "PROGRAM_ID")
        line = txt(r, "PROGRAM_LINE#")
        schema = txt(r, "PARSING_SCHEMA_NAME")
        parts = []
        if module:
            parts.append("MODULE=" + module)
        if action and action != module:
            parts.append("ACTION=" + action)
        if prog or line:
            parts.append("PROG={}:{}".format(prog, line))
        if schema:
            parts.append("SCHEMA=" + schema)
        return " | ".join(parts) if parts else "-"

    out = []
    out.append("**SQL-статистика (v$sqlarea)**: {} строк".format(len(rows)))

    # --- топ SQL по различным метрикам, с источником вызова ---
    def top_by(metric, n=10):
        i = idx.get(metric)
        if i is None:
            return []
        ranked = []
        for r in rows:
            ranked.append(
                {
                    "sql": txt(r, "SQL_TEXT", "")[:100].replace("\n", " "),
                    "sql_id": txt(r, "SQL_ID"),
                    "val": num(r, metric),
                    "exec": num(r, "EXECUTIONS"),
                    "cpu": num(r, "CPU_TIME"),
                    "source": src(r),
                }
            )
        ranked.sort(key=lambda x: x["val"], reverse=True)
        return ranked[:n]

    def emit_section(metric, title, n=10):
        ranked = top_by(metric, n)
        if not ranked:
            return
        out.append("\n### Топ SQL по {}".format(title))
        out.append("| # | SQL_ID | {} | Executions | CPU (ms) | Источник | SQL (обрезан) |".format(metric))  # noqa: E501
        out.append("|---|--------|------:|-----------:|---------:|----------|----------------|")
        for pos, e in enumerate(ranked, 1):
            out.append("| {} | {} | {} | {} | {} | {} | `{}` |".format(
                pos, e["sql_id"], e["val"], e["exec"], e["cpu"],
                e["source"], e["sql"]))

    emit_section("ELAPSED_TIME", "общее время выполнения (ELAPSED_TIME)", 15)
    emit_section("CPU_TIME", "CPU-времени (CPU_TIME)", 15)
    emit_section("EXECUTIONS", "количеству исполнений (EXECUTIONS)", 15)
    emit_section("BUFFER_GETS", "буферным чтениям (BUFFER_GETS)", 15)
    emit_section("DISK_READS", "дисковым чтениям (DISK_READS)", 15)

    # --- wait-тайминги ---
    emit_section("APPLICATION_WAIT_TIME", "времени ожидания приложения (APPLICATION_WAIT_TIME)", 10)
    emit_section("CONCURRENCY_WAIT_TIME", "времени ожидания конкурентности (CONCURRENCY_WAIT_TIME)", 10)  # noqa: E501
    emit_section("CLUSTER_WAIT_TIME", "времени ожидания кластера (CLUSTER_WAIT_TIME)", 10)
    emit_section("USER_IO_WAIT_TIME", "времени ожидания пользовательского IO (USER_IO_WAIT_TIME)", 10)  # noqa: E501

    # --- IO в байтах ---
    emit_section("PHYSICAL_READ_BYTES", "физическим чтениям в байтах (PHYSICAL_READ_BYTES)", 10)
    emit_section("PHYSICAL_WRITE_BYTES", "физическим записям в байтах (PHYSICAL_WRITE_BYTES)", 10)
    emit_section("IO_INTERCONNECT_BYTES", "байтам IO через interconnect (IO_INTERCONNECT_BYTES)", 10)  # noqa: E501
    emit_section("IO_CELL_OFFLOAD_ELIGIBLE_BYTES",
                 "байтам, подходящим под offload (IO_CELL_OFFLOAD_ELIGIBLE_BYTES)", 10)
    emit_section("PHYSICAL_READ_REQUESTS", "числу физических чтений (PHYSICAL_READ_REQUESTS)", 10)
    emit_section("PHYSICAL_WRITE_REQUESTS", "числу физических записей (PHYSICAL_WRITE_REQUESTS)", 10)  # noqa: E501

    # --- прочая нагрузка ---
    emit_section("ROWS_PROCESSED", "обработанным строкам (ROWS_PROCESSED)", 10)
    emit_section("SORTS", "сортировкам (SORTS)", 10)
    emit_section("PARSE_CALLS", "числу парсингов (PARSE_CALLS)", 10)
    emit_section("SHARABLE_MEM", "занимаемой памяти в пуле (SHARABLE_MEM)", 10)
    emit_section("VERSION_COUNT", "числу версий (VERSION_COUNT)", 10)

    # --- сводка по источникам вызова (MODULE) ---
    module_agg = {}
    for r in rows:
        m = txt(r, "MODULE") or "<none>"
        d = module_agg.setdefault(m, {
            "exec": 0, "cpu": 0, "elapsed": 0, "buf": 0, "disk": 0,
            "sql_count": 0, "parses": 0, "rows_proc": 0,
        })
        d["exec"] += num(r, "EXECUTIONS")
        d["cpu"] += num(r, "CPU_TIME")
        d["elapsed"] += num(r, "ELAPSED_TIME")
        d["buf"] += num(r, "BUFFER_GETS")
        d["disk"] += num(r, "DISK_READS")
        d["sql_count"] += 1
        d["parses"] += num(r, "PARSE_CALLS")
        d["rows_proc"] += num(r, "ROWS_PROCESSED")
    if module_agg:
        out.append("\n### Нагрузка по источнику (MODULE)")
        out.append("| MODULE | SQL | Executions | CPU (ms) | Elapsed (ms) | Buffer | Disk | RowsProc |")  # noqa: E501
        out.append("|--------|----:|-----------:|---------:|-------------:|-------:|-----:|--------:|")  # noqa: E501
        for m, d in sorted(module_agg.items(), key=lambda kv: kv[1]["cpu"], reverse=True)[:20]:
            out.append("| {} | {} | {} | {} | {} | {} | {} | {} |".format(
                m, d["sql_count"], d["exec"], d["cpu"] // 1000, d["elapsed"] // 1000,
                d["buf"], d["disk"], d["rows_proc"]))

    # --- сводка по схеме парсинга ---
    schema_agg = {}
    for r in rows:
        s = txt(r, "PARSING_SCHEMA_NAME") or "<none>"
        d = schema_agg.setdefault(s, {
            "exec": 0, "cpu": 0, "elapsed": 0, "buf": 0, "disk": 0, "sql_count": 0,
        })
        d["exec"] += num(r, "EXECUTIONS")
        d["cpu"] += num(r, "CPU_TIME")
        d["elapsed"] += num(r, "ELAPSED_TIME")
        d["buf"] += num(r, "BUFFER_GETS")
        d["disk"] += num(r, "DISK_READS")
        d["sql_count"] += 1
    if schema_agg:
        out.append("\n### Нагрузка по схеме парсинга (PARSING_SCHEMA_NAME)")
        out.append("| SCHEMA | SQL | Executions | CPU (ms) | Elapsed (ms) | Buffer | Disk |")
        out.append("|--------|----:|-----------:|---------:|-------------:|-------:|-----:|")
        for s, d in sorted(schema_agg.items(), key=lambda kv: kv[1]["cpu"], reverse=True)[:15]:
            out.append("| {} | {} | {} | {} | {} | {} | {} |".format(
                s, d["sql_count"], d["exec"], d["cpu"] // 1000,
                d["elapsed"] // 1000, d["buf"], d["disk"]))

    # --- сводка по COMMAND_TYPE ---
    cmd_agg = {}
    for r in rows:
        c = txt(r, "COMMAND_TYPE") or "<none>"
        d = cmd_agg.setdefault(c, {"exec": 0, "cpu": 0, "sql_count": 0})
        d["exec"] += num(r, "EXECUTIONS")
        d["cpu"] += num(r, "CPU_TIME")
        d["sql_count"] += 1
    if cmd_agg:
        out.append("\n### Нагрузка по типу команды (COMMAND_TYPE)")
        out.append("| COMMAND_TYPE | SQL | Executions | CPU (ms) |")
        out.append("|--------------|----:|-----------:|---------:|")
        for c, d in sorted(cmd_agg.items(), key=lambda kv: kv[1]["cpu"], reverse=True)[:10]:
            out.append("| {} | {} | {} | {} |".format(c, d["sql_count"], d["exec"], d["cpu"] // 1000))  # noqa: E501

    # --- агрегаты ---
    agg = {
        "total_sql": len(rows),
        "total_exec": sum(num(r, "EXECUTIONS") for r in rows),
        "total_cpu_ms": sum(num(r, "CPU_TIME") for r in rows) // 1000,
        "total_elapsed_ms": sum(num(r, "ELAPSED_TIME") for r in rows) // 1000,
        "total_buffer_gets": sum(num(r, "BUFFER_GETS") for r in rows),
        "total_disk_reads": sum(num(r, "DISK_READS") for r in rows),
        "total_sorts": sum(num(r, "SORTS") for r in rows),
        "total_parse_calls": sum(num(r, "PARSE_CALLS") for r in rows),
        "total_rows_processed": sum(num(r, "ROWS_PROCESSED") for r in rows),
        "total_sharable_mem": sum(num(r, "SHARABLE_MEM") for r in rows),
        "high_version_count": [
            {
                "sql": txt(r, "SQL_TEXT", "")[:100].replace("\n", " "),
                "sql_id": txt(r, "SQL_ID"),
                "version_count": num(r, "VERSION_COUNT"),
                "source": src(r),
            }
            for r in rows
            if num(r, "VERSION_COUNT") > 20
        ][:10],
        "high_invalidations": [
            {
                "sql": txt(r, "SQL_TEXT", "")[:100].replace("\n", " "),
                "sql_id": txt(r, "SQL_ID"),
                "invalidations": num(r, "INVALIDATIONS"),
                "source": src(r),
            }
            for r in rows
            if num(r, "INVALIDATIONS") > 0
        ][:10],
    }
    out.append("\n### Агрегаты по пулу")
    out.append("- Всего SQL: **{}**".format(agg["total_sql"]))
    out.append("- Суммарно исполнений: **{}**".format(agg["total_exec"]))
    out.append("- CPU: **{} ms**".format(agg["total_cpu_ms"]))
    out.append("- Время выполнения: **{} ms**".format(agg["total_elapsed_ms"]))
    out.append("- Буферные чтения: **{}**".format(agg["total_buffer_gets"]))
    out.append("- Дисковые чтения: **{}**".format(agg["total_disk_reads"]))
    out.append("- Сортировки: **{}**".format(agg["total_sorts"]))
    out.append("- Парсинги (PARSE_CALLS): **{}**".format(agg["total_parse_calls"]))
    out.append("- Обработанные строки (ROWS_PROCESSED): **{}**".format(agg["total_rows_processed"]))
    out.append("- Занимаемая память пула (SHARABLE_MEM): **{}**".format(_fmt_bytes(agg["total_sharable_mem"])))  # noqa: E501
    if agg["high_version_count"]:
        out.append("\n### SQL с высоким VERSION_COUNT (>20) — кандидаты на проблему версионирования")  # noqa: E501
        out.append("| SQL_ID | VersionCount | Источник | SQL |")
        out.append("|--------|-------------:|----------|-----|")
        for e in agg["high_version_count"]:
            out.append("| {} | {} | {} | `{}` |".format(
                e["sql_id"], e["version_count"], e["source"], e["sql"]))
    if agg["high_invalidations"]:
        out.append("\n### SQL с инвалидациями (INVALIDATIONS>0)")
        out.append("| SQL_ID | Invalidations | Источник | SQL |")
        out.append("|--------|--------------:|----------|-----|")
        for e in agg["high_invalidations"]:
            out.append("| {} | {} | {} | `{}` |".format(
                e["sql_id"], e["invalidations"], e["source"], e["sql"]))
    return out


def analyze_table_sizes(meta, analysis):
    """Размеры сегментов: топ по байтам."""
    cols = analysis["columns"]
    idx = {name: i for i, name in enumerate(cols)}
    rows = analysis["rows"]

    def val(r, key, default=""):
        i = idx.get(key)
        return r[i] if i is not None and i < len(r) else default

    ranked = []
    for r in rows:
        ranked.append(
            {
                "name": str(val(r, "SEGMENT_NAME")),
                "type": str(val(r, "SEGMENT_TYPE")),
                "bytes": _to_int(val(r, "BYTES")),
            }
        )
    ranked.sort(key=lambda x: x["bytes"], reverse=True)

    out = []
    out.append("**Сегменты**: {} шт, суммарно **{}**".format(
        len(ranked), _fmt_bytes(sum(x["bytes"] for x in ranked))))
    out.append("\n### Топ сегментов по размеру")
    out.append("| # | SEGMENT_NAME | SEGMENT_TYPE | BYTES |")
    out.append("|---|--------------|--------------|------:|")
    for pos, e in enumerate(ranked[:20], 1):
        out.append("| {} | {} | {} | {} ({}) |".format(
            pos, e["name"], e["type"], e["bytes"], _fmt_bytes(e["bytes"])))

    # агрегация по типу
    by_type = {}
    for e in ranked:
        by_type[e["type"]] = by_type.get(e["type"], 0) + e["bytes"]
    if by_type:
        out.append("\n### Размер по типу сегмента")
        out.append("| SEGMENT_TYPE | Суммарно |")
        out.append("|--------------|---------:|")
        for t, b in sorted(by_type.items(), key=lambda kv: kv[1], reverse=True):
            out.append("| {} | {} |".format(t, _fmt_bytes(b)))
    return out


def analyze_tablespaces(meta, analysis):
    """Табличные пространства."""
    cols = analysis["columns"]
    idx = {name: i for i, name in enumerate(cols)}
    rows = analysis["rows"]

    def val(r, key, default=""):
        i = idx.get(key)
        return r[i] if i is not None and i < len(r) else default

    out = []
    out.append("**Табличные пространства**: {} шт".format(len(rows)))
    out.append("\n| NAME | TYPE | SIZE | FREE | USED % |")
    out.append("|------|------|-----:|-----:|-------:|")
    for r in rows:
        size = _to_float(val(r, "BYTES"))
        free = _to_float(val(r, "BYTES"))
        pct = (100.0 * (size - free) / size) if size > 0 else 0.0
        out.append("| {} | {} | {} | {} | {:.1f}% |".format(
            val(r, "TABLESPACE_NAME"), val(r, "CONTENTS"),
            _fmt_bytes(size), _fmt_bytes(free), pct))
    return out


_ANALYZERS = {
    "shared_pool": analyze_shared_pool,
    "table_sizes": analyze_table_sizes,
    "tablespaces": analyze_tablespaces,
}


# --- сбор отчёта ----------------------------------------------------------
def build_report(input_dir, top_n):
    files = sorted(
        os.path.join(input_dir, f)
        for f in os.listdir(input_dir)
        if f.endswith(".json") and "_" in f
    )
    lines = []
    lines.append("# Отчёт l2-db-spider")
    lines.append("")
    lines.append("- Каталог: `{}`".format(input_dir))
    lines.append("- Сгенерировано: {}".format(datetime.now().strftime("%Y-%m-%d %H:%M:%S")))
    lines.append("- Файлов JSON: **{}**".format(len(files)))
    lines.append("")
    lines.append("> Этот документ — структурированный вход для AI-агента. "
                 "На его основе агент формирует сводный отчёт с рекомендациями.")
    lines.append("")

    # сводная таблица по файлам
    metas = []
    analyses = {}
    errors = []
    for f in files:
        meta, analysis = analyze_file(f)
        if meta is None:
            continue
        metas.append(meta)
        if analysis is not None:
            analyses[meta["file"]] = (meta, analysis)
        elif meta.get("parse_error"):
            errors.append(meta)

    # группируем по db/query
    lines.append("## Сводка по БД и запросам")
    lines.append("")
    lines.append("| БД | Запрос | Статус | Строк | Время генерации |")
    lines.append("|----|--------|--------|------:|------------------|")
    for m in metas:
        status = m.get("status", "?")
        rows = ""
        if m.get("status") == "ok":
            a = analyses.get(m["file"], (None, None))[1]
            rows = str(a["row_count"]) if a else ""
        lines.append("| {} | {} | {} | {} | {} |".format(
            m.get("db", "?"), m.get("query", "?"), status, rows,
            m.get("generated_at", "")))
    lines.append("")

    if errors:
        lines.append("## Файлы с ошибками парсинга")
        lines.append("")
        for e in errors:
            lines.append("- `{}`: {}".format(e["file"], e["parse_error"]))
        lines.append("")

    # детальный анализ по файлам
    for f in sorted(analyses.keys()):
        meta, analysis = analyses[f]
        lines.append("---")
        lines.append("")
        lines.append("## {} / {}".format(meta.get("db"), meta.get("query")))
        lines.append("")
        lines.append("_Файл_: `{}`, строк: {}".format(meta["file"], analysis["row_count"]))
        lines.append("")
        if analysis["columns"]:
            lines.append("_Колонки_: {}".format(", ".join(
                "{} ({})".format(n, t) for n, t in zip(analysis["columns"], analysis["col_types"]))))  # noqa: E501
            lines.append("")
        analyzer = _ANALYZERS.get(meta.get("query"))
        if analyzer:
            section = analyzer(meta, analysis)
            if section:
                lines.extend(section)
                lines.append("")
        else:
            # неизвестный запрос — показываем сырые данные (первые топ_N строк)
            lines.append("### Сырые данные (первые {} строк)".format(top_n))
            lines.append("")
            for r in analysis["rows"][:top_n]:
                lines.append("- `{}`".format(str(r)[:200]))
            lines.append("")
    return "\n".join(lines)


def _analyze_file_data(path):
    """Читает JSON-файл и возвращает (meta, analysis) без форматирования."""
    return analyze_file(path)


# Бинарные/неинформативные RAW-колонки v$sqlarea, раздувающие дамп.
_RAW_EXCLUDE = {"OPTIMIZER_ENV", "ADDRESS", "LAST_ACTIVE_CHILD_ADDRESS",
                "BIND_DATA", "LITERAL_HASH_VALUE", "OUTLINE_SID",
                "CHILD_LATCH", "EXACT_MATCHING_SIGNATURE",
                "FORCE_MATCHING_SIGNATURE"}
# Предел длины строкового поля в дампе (SQL_TEXT и т.п. бывают гигантскими).
_MAX_FIELD_LEN = 120
# Максимум строк на файл, передаваемых модели (контекст 262144 токенов не
# вмещает 90K+ строк v$sqlarea целиком). Берём топ по значимой метрике.
# ~230 строк × ~9 файлов ≈ 435K символов ≈ ~245K токенов (коэфф ~1.8 симв/токен
# для русско-английского) — влезает в 262144 с небольшим запасом.
_MAX_AI_ROWS_PER_FILE = 230


def build_ai_input(input_dir, max_rows=_MAX_AI_ROWS_PER_FILE):
    """Собирает дамп всех JSON-данных l2-db-spider для передачи AI-модели.
    RAW-колонки (бинарные) исключаются, строковые поля обрезаются до
    _MAX_FIELD_LEN. `max_rows` ограничивает число строк на файл (берём топ по
    значимой метрике); 0/None = без ограничения (риск превышения контекста).
    Модель сама проводит анализ по переданному набору данных."""
    files = sorted(
        os.path.join(input_dir, f)
        for f in os.listdir(input_dir)
        if f.endswith(".json") and "_" in f
    )
    lines = []
    for f in files:
        meta, analysis = analyze_file(f)
        lines.append("=== ФАЙЛ: {} ===".format(os.path.basename(f)))
        if meta is None:
            continue
        lines.append("db={} query={} status={} generated_at={}".format(
            meta.get("db"), meta.get("query"), meta.get("status"),
            meta.get("generated_at")))
        if meta.get("status") != "ok":
            err = meta.get("error", {})
            lines.append("error: code={} message={}".format(
                err.get("code"), err.get("message")))
            lines.append("")
            continue
        if analysis is None:
            lines.append("")
            continue
        cols = analysis["columns"]
        types = analysis["col_types"]
        # исключаем RAW-колонки
        keep_idx = [i for i, n in enumerate(cols)
                    if n not in _RAW_EXCLUDE and (i >= len(types) or types[i] != "RAW")]
        keep_cols = [cols[i] for i in keep_idx]
        # значимая метрика для сортировки (по ней берём топ строк)
        query = meta.get("query")
        sort_key = "ELAPSED_TIME" if query == "shared_pool" else \
                   "BYTES" if query == "table_sizes" else \
                   cols[0] if cols else ""
        rows = analysis["rows"]
        total = len(rows)
        if sort_key in cols:
            sk = cols.index(sort_key)
            try:
                rows = sorted(rows, key=lambda r: _to_float(r[sk]) if sk < len(r) else 0,
                              reverse=True)
            except (TypeError, ValueError):
                pass
            if max_rows and max_rows > 0:
                rows = rows[:max_rows]
        lines.append("columns: " + ", ".join(keep_cols))
        shown = len(rows)
        lines.append("row_count={} (показано {} из {} строк, топ по '{}')".format(
            total, shown, total, sort_key))
        lines.append("rows:")

        def _fmt(v):
            if v is None:
                return "null"
            if isinstance(v, bool):
                return "1" if v else "0"
            if isinstance(v, (int, float)):
                return str(v)
            s = str(v)
            if len(s) > _MAX_FIELD_LEN:
                s = s[:_MAX_FIELD_LEN] + "…"
            return s

        for r in rows:
            vals = [_fmt(r[i]) if i < len(r) else "null" for i in keep_idx]
            lines.append("  [" + ", ".join(vals) + "]")
        lines.append("")
    return "\n".join(lines), True


# --- AI-генерация человекочитаемого отчёта --------------------------------
# Конфигурация из переменных окружения (OpenAI-совместимый chat completions):
#   AI_PROVIDER   — "openai" | "anthropic" (default: openai)
#   AI_API_URL    — base URL, напр. https://api.openai.com/v1 (обязателен)
#   AI_API_KEY    — API-ключ (обязателен)
#   AI_MODEL      — имя модели, напр. gpt-4o / claude-sonnet-4 (default: "")
#   AI_TIMEOUT    — таймаут запроса в секундах (default: 120)
#   AI_TEMPERATURE— креативность 0..1 (default: 0.3)

_AI_SYSTEM_PROMPT = (
    "Ты — эксперт по производительности и диагностике Oracle БД. Тебе передан ПОЛНЫЙ "
    "дамп данных l2-db-spider по нескольким базам (ccm2/ccm4/ccm5 и др.):\n"
    "  - shared_pool: v$sqlarea — статистика каждого SQL (executions, cpu_time, "
    "elapsed_time, buffer_gets, disk_reads, sorts, version_count, invalidations, "
    "parse_calls, rows_processed, wait-тайминги, physical io, модуль/схема-источник "
    "MODULE/ACTION/PARSING_SCHEMA_NAME/COMMAND_TYPE);\n"
    "  - table_sizes: размеры сегментов (SEGMENT_NAME/SEGMENT_TYPE/BYTES);\n"
    "  - tablespaces: табличные пространства.\n"
    "\n"
    "Составь СВЯЗНЫЙ, человекочитаемый, аналитический отчёт в Markdown с РЕКОМЕНДАЦИЯМИ. "
    "Требования:\n"
    "1. Пиши связным текстом с заголовками, а НЕ просто таблицами. Таблицы можно "
    "использовать только как краткие сводки внутри текста.\n"
    "2. Структура отчёта:\n"
    "   ## Общая сводка\n"
    "   ## Анализ по каждой базе\n"
    "      ### <Имя базы> — состояние и нагрузка\n"
    "      ### Проблемные SQL и их источники (модуль/приложение/схема)\n"
    "      ### Аномалии и риски (VERSION_COUNT, инвалидации, wait, память пула)\n"
    "   ## Анализ источников нагрузки (кто грузит БД)\n"
    "   ## Сегменты и табличные пространства\n"
    "   ## Рекомендации (по приоритету: критично/важно/желательно)\n"
    "3. В рекомендациях указывай конкретные SQL_ID, модули, схемы, таблицы из данных.\n"
    "4. Хеш запроса (SQL_ID) в рекомендации оформляй как ССЫЛКУ на сам запрос: "
    "Markdown-ссылка вида [SQL <SQL_ID>](#sql-<SQL_ID>), указывающая на раздел с полным "
    "текстом этого запроса ниже. Обязательно добавь в конец отчёта раздел "
    "«## Полные тексты запросов», где для каждого упомянутого SQL_ID приведи сам запрос "
    "в блоке кода (если текст запроса есть в данных; если нет — блок с пометкой "
    "«текст недоступен в дампе»).\n"
    "5. Если клик по ссылке невозможен (например, при конвертации в PDF): оформляй "
    "рекомендацию в виде таблицы со столбцами | SQL_ID | Сам запрос (обрезан до 200 "
    "символов) | Рекомендация |, а не просто текстом со ссылкой.\n"
    "6. Приоритизируй рекомендации по влиянию на производительность.\n"
    "7. Не выдумывай фактов, которых нет в данных. Если данных мало — честно это отметь.\n"
    "8. Язык отчёта — русский."
)


def _load_ai_config():
    """Читает конфигурацию AI из переменных окружения. Возвращает dict или None,
    если AI_API_URL/AI_API_KEY не заданы."""
    provider = os.environ.get("AI_PROVIDER", "openai").strip().lower()
    url = os.environ.get("AI_API_URL", "").strip().rstrip("/")
    key = os.environ.get("AI_API_KEY", "").strip()
    model = os.environ.get("AI_MODEL", "").strip()
    if not url or not key:
        return None
    cfg = {
        "url": url,
        "key": key,
        "model": model,
        "provider": provider,
        "timeout": int(os.environ.get("AI_TIMEOUT", "600")),
        "temperature": float(os.environ.get("AI_TEMPERATURE", "0.3")),
    }
    return cfg


def _openai_chat(cfg, messages):
    """Отправляет chat-запрос в OpenAI-совместимый endpoint."""
    url = cfg["url"] + "/chat/completions"
    payload = {
        "model": cfg["model"] or "gpt-4o",
        "messages": messages,
        "temperature": cfg["temperature"],
        # Ограничение на выход: модель на этой железке очень медленная,
        # без max_tokens она генерирует до бесконечности (таймаут).
        "max_tokens": int(os.environ.get("AI_MAX_TOKENS", "6000")),
    }
    req = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": "Bearer {}".format(cfg["key"]),
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=cfg["timeout"]) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    return data["choices"][0]["message"]["content"].strip()


def _anthropic_chat(cfg, messages):
    """Отправляет запрос в Anthropic Messages API (маппинг на system/user)."""
    system = "\n\n".join(m["content"] for m in messages if m["role"] == "system")
    user = "\n\n".join(m["content"] for m in messages if m["role"] == "user")
    url = cfg["url"].replace("/v1", "") + "/v1/messages"
    payload = {
        "model": cfg["model"] or "claude-sonnet-4",
        "system": system,
        "messages": [{"role": "user", "content": user}],
        "max_tokens": 8192,
        "temperature": cfg["temperature"],
    }
    req = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "x-api-key": cfg["key"],
            "anthropic-version": "2023-06-01",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=cfg["timeout"]) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    return "".join(block.get("text", "") for block in data["content"]).strip()


def generate_ai_report(data_text):
    """Передаёт полный дамп данных AI-модели, возвращает человекочитаемый отчёт.
    Возвращает (report, used_ai) — used_ai=True если модель ответила."""
    cfg = _load_ai_config()
    if not cfg:
        return data_text, False
    messages = [
        {"role": "system", "content": _AI_SYSTEM_PROMPT},
        {"role": "user",
         "content": ("Ниже — ПОЛНЫЕ данные l2-db-spider по всем базам. Проанализируй их "
                     "и составь связный отчёт с рекомендациями по производительности "
                     "Oracle БД, следуя системному промту:\n\n" + data_text)},
    ]
    try:
        if cfg["provider"] == "anthropic":
            return _anthropic_chat(cfg, messages), True
        return _openai_chat(cfg, messages), True
    except (urllib.error.HTTPError, urllib.error.URLError, ValueError,
            KeyError, json.JSONDecodeError, OSError) as e:
        print("Предупреждение: AI-модель недоступна ({}), отчёт без генерации.".format(e),
              file=sys.stderr)
        return data_text, False


def main():
    parser = argparse.ArgumentParser(description="Собирает md-отчёт из spider-output")
    parser.add_argument("--input-dir", default="./spider-output",
                        help="Каталог с JSON-файлами spider (default: ./spider-output)")
    parser.add_argument("--output-md", default="",
                        help="Путь для сохранения отчёта (default: печать в stdout)")
    parser.add_argument("--top", type=int, default=10,
                        help="Размер топ-списков (default: 10)")
    parser.add_argument("--no-ai", action="store_true",
                        help="Не вызывать AI-модель, только собрать таблицы")
    parser.add_argument("--ai-rows", type=int, default=40,
                        help="Лимит строк на файл для AI (0=без лимита; "
                             "больше строк = медленнее генерация; ~40 быстро, "
                             "~230 влезает в контекст 262144, но ~10 мин)")
    args = parser.parse_args()

    if not os.path.isdir(args.input_dir):
        print("Ошибка: каталог не найден: {}".format(args.input_dir), file=sys.stderr)
        sys.exit(1)

    tables = build_report(args.input_dir, args.top)

    report, used_ai = (tables, False)
    if not args.no_ai:
        # Дамп данных для модели (контекст ~262144 токенов). Лимит строк на
        # файл настраивается --ai-rows (default 80 — быстрее; 230 — полный).
        ai_input, _ = build_ai_input(args.input_dir, args.ai_rows)
        report, used_ai = generate_ai_report(ai_input)

    header = "# Отчёт l2-db-spider\n\n"
    if used_ai:
        header += "> Сгенерирован AI-моделью на основе данных l2-db-spider.\n\n"
        out = header + report
    else:
        out = report
    out += "\n\n---\n\n## Сырые таблицы (для справки)\n\n" + tables

    if args.output_md:
        with open(args.output_md, "w", encoding="utf-8") as fh:
            fh.write(out + "\n")
        print("Сохранено: {}".format(args.output_md))
        if used_ai:
            print("Отчёт сгенерирован AI ({}).".format(
                os.environ.get("AI_PROVIDER", "openai")))
        else:
            print("AI не использовался (настрой AI_API_URL/AI_API_KEY) — только таблицы.")
    else:
        print(out)


if __name__ == "__main__":
    main()
