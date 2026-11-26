#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Анализ данных плавок (heat-output/*.json) для AI-генерации человекочитаемого
отчёта. Формат heat-файла (от heat-scraper):
    {"heat_no": <номер>, "http_status": 200, "scraped_at": "...",
     "response": {"jsonrpc":"2.0","result":{"data":{
        "processes": [ {unit, id2, naliv_no, proc_begin, proc_end, ladles_cil,
                        attributes, handlings: [ {type_description, type_id,
                        hdl_begin, hdl_end, sections, attributes:[{name,value}]} ]} ],
        "sample request": [], "chemical analyses": [ {sample_point, id2,
        sample_no, result_time, material_type, analisys_type, laboratory,
        chemistry:[{name,value}]} ] }}, "id":"1"}}

Использует конфиг AI из spider_report (_load_ai_config / _openai_chat), поэтому
нужны те же env: AI_API_URL, AI_API_KEY, AI_MODEL, AI_AI_ROWS, AI_MAX_TOKENS.
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spider_report as sr  # noqa: E402  (переиспользуем _load_ai_config/_openai_chat)


def _fmt(v, default=""):
    """Форматирует значение в строку."""
    if v is None:
        return default
    return str(v)


def analyze_heat_file(path):
    """Возвращает (meta, analysis) для heat-файла."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            data = json.load(fh)
    except (json.JSONDecodeError, OSError) as e:
        return {"file": path, "parse_error": str(e)}, None

    meta = {
        "file": path,
        "heat_no": data.get("heat_no"),
        "http_status": data.get("http_status"),
        "scraped_at": data.get("scraped_at"),
    }
    response = data.get("response", {})
    if "error" in response or data.get("http_status", 0) >= 400:
        err = response.get("error", {})
        meta["error"] = {"code": err.get("code"), "message": err.get("message")}
        return meta, None

    result = response.get("result", {})
    data_obj = result.get("data", {}) if isinstance(result, dict) else {}
    processes = data_obj.get("processes", []) if isinstance(data_obj, dict) else []
    chemistry = data_obj.get("chemical analyses", []) if isinstance(data_obj, dict) else []

    analysis = {
        "processes": processes,
        "chemical": chemistry,
        "n_processes": len(processes) if isinstance(processes, list) else 0,
        "n_chemical": len(chemistry) if isinstance(chemistry, list) else 0,
    }
    return meta, analysis


_HANDLING_KEYS = ("sections", "hdl_begin", "hdl_end", "type", "type_id",
                  "type_description")


def _attrs_from_obj(o):
    """Извлекает пары name=value из объекта handling/процесса.

    Поддерживает два формата:
      1) вложенный attributes: [{name, value}, ...]
      2) атрибуты прямо в объекте (SCALES_NO, W_MODE, ...).
    Исключает служебные ключи (sections/hdl_begin/hdl_end/type и т.п.).
    """
    pairs = []
    attrs = o.get("attributes")
    if isinstance(attrs, list):
        for a in attrs:
            if isinstance(a, dict):
                pairs.append("{}={}".format(a.get("name", ""), a.get("value", "")))
    for k, v in o.items():
        if k in _HANDLING_KEYS or k == "attributes" or k == "id2":
            continue
        if isinstance(v, (str, int, float)):
            pairs.append("{}={}".format(k, v))
    return pairs


def summarize_process(p, max_handlings=30):
    """Компактная сводка одного процесса плавки для дампа AI."""
    lines = []
    unit = p.get("unit", "")
    naliv = p.get("naliv_no", "")
    begin = p.get("proc_begin", "")
    end = p.get("proc_end", "")
    lines.append("unit={} naliv_no={} begin={} end={}".format(unit, naliv, begin, end))

    pattr = _attrs_from_obj(p)
    if pattr:
        lines.append("  attributes: " + "; ".join(pattr))

    handlings = p.get("handlings", [])
    for h in handlings[:max_handlings]:
        td = (h.get("type_description") or h.get("type") or
              "type_id={}".format(h.get("type_id", "")))
        sec = h.get("sections", "")
        hb = h.get("hdl_begin", "")
        he = h.get("hdl_end", "")
        ha = _attrs_from_obj(h)
        lines.append("  - [{}] {} {} {} :: {}".format(td, sec, hb, he, "; ".join(ha)))
    return "\n".join(lines)


def summarize_chemical(c, max_chem=20):
    """Компактная сводка хим. анализа одной пробы."""
    lines = []
    sp = c.get("sample_point", "")
    sno = c.get("sample_no", "")
    rt = c.get("result_time", "")
    lab = c.get("laboratory", "")
    mat = c.get("material_type", "")
    lines.append("sample_point={} sample_no={} time={} material={} lab={}".format(
        sp, sno, rt, mat, lab))
    chem = c.get("chemistry", [])
    kv = ["{}={}".format(ch.get("name", ""), ch.get("value", "")) for ch in chem[:max_chem]]
    lines.append("  chemistry: " + ", ".join(kv))
    return "\n".join(lines)


def build_heat_report(input_dir, max_heats=100):
    """Строит человекочитаемую сводку по heat-файлам (для справки в md)."""
    files = sorted(
        os.path.join(input_dir, f)
        for f in os.listdir(input_dir)
        if f.startswith("heat_") and f.endswith(".json")
    )
    files = files[:max_heats]
    lines = []
    lines.append("**Данные плавок**: {} файлов".format(len(files)))
    ok = err = 0
    for f in files:
        meta, analysis = analyze_heat_file(f)
        if analysis is None:
            err += 1
            continue
        ok += 1
    lines.append("С плавками (OK): {}, пустых/ошибок: {}".format(ok, err))
    return "\n".join(lines)


def _parse_dt(ts):
    """Разбирает ISO-временную метку 'YYYY-MM-DDTHH:MM:SSZ' в минуты от начала
    суток (для расчёта длительности операций). Возвращает None при ошибке."""
    if not ts or not isinstance(ts, str):
        return None
    m = re.match(r"(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})", ts)
    if not m:
        return None
    try:
        hh, mm, ss = int(m.group(4)), int(m.group(5)), int(m.group(6))
        return hh * 60 + mm + ss / 60.0
    except (ValueError, TypeError):
        return None


def build_ai_input(input_dir, max_heats=100, n_examples=4):
    """Строит АГРЕГИРОВАННЫЙ дамп данных плавок для AI-модели.

    500 плавок в полном виде = ~7.6 МБ (> контекста модели), поэтому считаем
    компактную статистику: распределение по агрегатам, типы операций с
    длительностями, диапазоны температур/весов, химсостав, аномалии; плюс
    n_examples детальных примеров плавок для конкретики."""
    files = sorted(
        os.path.join(input_dir, f)
        for f in os.listdir(input_dir)
        if f.startswith("heat_") and f.endswith(".json")
    )
    files = files[:max_heats]
    if not files:
        return "Данных плавок нет.\n"

    heats = []          # heat_no
    unit_count = {}     # unit -> число процессов
    op_stats = {}       # op_type -> [count, dur_sum]
    temps = []          # все температуры
    weights = []        # все NET_WEIGHT
    chem_vals = {}      # элемент -> [значения]
    anoms = []          # аномалии (строки)
    examples = []       # детальные примеры

    ok = err = 0
    for f in files:
        meta, analysis = analyze_heat_file(f)
        heat = meta.get("heat_no")
        if analysis is None:
            err += 1
            continue
        ok += 1
        heats.append(heat)
        procs = analysis["processes"]
        for p in procs:
            unit = p.get("unit", "?")
            unit_count[unit] = unit_count.get(unit, 0) + 1
            # операции
            for h in p.get("handlings", []):
                op = (h.get("type_description") or h.get("type") or
                      "type_id={}".format(h.get("type_id", "")))
                b = _parse_dt(h.get("hdl_begin"))
                e = _parse_dt(h.get("hdl_end"))
                dur = (e - b) if (b and e and e >= b) else 0
                s = op_stats.setdefault(op, [0, 0.0])
                s[0] += 1
                s[1] += dur
                # атрибуты операции (температура/вес)
                for a in h.get("attributes", []):
                    if isinstance(a, dict):
                        name, val = a.get("name", ""), a.get("value")
                        if name == "TEMPERATURE":
                            _add_num(temps, val)
                        elif name == "NET_WEIGHT":
                            _add_num(weights, val)
                # атрибуты прямо в объекте (новый формат)
                for k in ("TEMPERATURE", "NET_WEIGHT"):
                    if k in h:
                        _add_num(temps if k == "TEMPERATURE" else weights, h[k])
        # химсостав
        for c in analysis["chemical"]:
            for ch in c.get("chemistry", []):
                name = ch.get("name", "")
                _add_num(chem_vals.setdefault(name, []), ch.get("value"))
        # аномалия: длинный процесс или много операций
        for p in procs:
            b = _parse_dt(p.get("proc_begin"))
            e = _parse_dt(p.get("proc_end"))
            if b and e and e - b > 120:
                anoms.append("heat={} unit={}: процесс > 120 мин ({:.0f})".format(
                    heat, p.get("unit", "?"), e - b))
        # детальные примеры (первые n_examples)
        if len(examples) < n_examples:
            ex = ["heat_no={} http_status={}".format(heat, meta.get("http_status"))]
            for p in procs:
                ex.append(summarize_process(p, max_handlings=8))
            examples.append("\n".join(ex))

    heats = sorted([h for h in heats if h is not None])

    out = []
    out.append("=== АГРЕГИРОВАННАЯ СТАТИСТИКА ПО ПЛАВКАМ ===")
    out.append("Плавок: {} (OK={}, error={}), диапазон heat_no: {}..{}".format(
        ok + err, ok, err, heats[0] if heats else "-", heats[-1] if heats else "-"))

    out.append("\n**Распределение по агрегатам (unit):**")
    for u, n in sorted(unit_count.items(), key=lambda x: -x[1]):
        out.append("  {}: {} процессов".format(u, n))

    out.append("\n**Операции (handlings) — частота и средняя длительность:**")
    for op, (cnt, dur) in sorted(op_stats.items(), key=lambda x: -x[1][0]):
        avg = dur / cnt if cnt else 0
        out.append("  {}: {} шт, сред. длительность {:.1f} мин".format(op, cnt, avg))

    out.append("\n**Температуры (°C):** " + _fmt_range(temps))
    out.append("**Веса NET_WEIGHT (т):** " + _fmt_range(weights))

    if chem_vals:
        out.append("\n**Химический состав (диапазоны по элементам):**")
        for el, vals in sorted(chem_vals.items()):
            out.append("  {}: {}".format(el, _fmt_range(vals)))

    if anoms:
        out.append("\n**Аномалии (длительные процессы > 120 мин):**")
        for a in anoms[:30]:
            out.append("  - " + a)

    if examples:
        out.append("\n=== ДЕТАЛЬНЫЕ ПРИМЕРЫ ПЛАВОК ===")
        out.append("\n\n".join(examples))

    return "\n".join(out)


def _add_num(lst, val):
    """Добавляет число в список (если val — число)."""
    if isinstance(val, bool):
        return
    if isinstance(val, (int, float)):
        lst.append(float(val))


def _fmt_range(vals):
    """Форматирует список чисел как 'min..max (N, среднее X)'."""
    if not vals:
        return "нет данных"
    vals = sorted(vals)
    avg = sum(vals) / len(vals)
    return "{:.2f}..{:.2f} (N={}, среднее {:.2f})".format(
        vals[0], vals[-1], len(vals), avg)


# --- AI-промт для анализа плавок -----------------------------------------
_AI_HEAT_PROMPT = (
    "Ты — эксперт по технологическому процессу производства стали и анализа данных "
    "АСУ ТП (DataHub). Тебе передан набор данных по ПЛАВКАМ (heat) с агрегата "
    "дозирования чугуна/УДЧ/ковшевой обработки: для каждой плавки (heat_no) есть:\n"
    "  - процессы (unit: МК3/УДЧ2 и др.) с временными интервалами (proc_begin/proc_end);\n"
    "  - attributes — параметры (ladle_no, heat_no, вес, режим);\n"
    "  - handlings — операции (Провешивание, Фиксация тары, Калькуляция, Обработка, "
    "Инжекция, Скачивание шлака, Отдув шлака, Измерение температуры) с временными "
    "интервалами и атрибутами (вес, температура, расход FeSi, дозировка и т.д.);\n"
    "  - chemical analyses — химический состав проб (C, Si, P, S, Ti, V, Cr, Mn, Ni, Cu).\n"
    "\n"
    "Составь СВЯЗНЫЙ, человекочитаемый, аналитический отчёт в Markdown. Требования:\n"
    "1. Пиши связным текстом с заголовками; таблицы — только как краткие сводки.\n"
    "2. Структура:\n"
    "   ## Общая сводка по диапазону плавок\n"
    "   ## Типовой сценарий плавки (операции, их длительность, порядок)\n"
    "   ## Анализ по агрегатам (МК3, УДЧ2 и др.)\n"
    "   ## Химический состав: диапазоны и отклонения (C, Si, P, S, Mn и др.)\n"
    "   ## Аномалии и риски (нестандартные операции, длинные простои, отклонения веса)\n"
    "   ## Рекомендации (по приоритету)\n"
    "3. Указывай конкретные heat_no, агрегаты (unit), операции (handlings) из данных.\n"
    "4. Хеш плавки (heat_no) в рекомендации оформляй как ссылку "
    "[Плавка <heat_no>](#heat-<heat_no>) на раздел с её сводкой в конце отчёта "
    "(## Сводки плавок). Если клик невозможен (PDF) — оформляй рекомендацию таблицей "
    "| heat_no | Ключевые операции | Рекомендация |.\n"
    "5. Приоритизируй рекомендации по влиянию на качество/процесс.\n"
    "6. Не выдумывай фактов, которых нет в данных.\n"
    "7. Язык отчёта — русский."
)


def generate_ai_report(data_text):
    """Передаёт дамп плавок AI-модели, возвращает человекочитаемый отчёт."""
    cfg = sr._load_ai_config()
    if not cfg:
        return data_text, False
    messages = [
        {"role": "system", "content": _AI_HEAT_PROMPT},
        {"role": "user",
         "content": ("Ниже — данные по плавкам (GetHeatData) от l2-heat-scraper. "
                     "Проанализируй и составь связный отчёт, следуя системному "
                     "промту:\n\n" + data_text)},
    ]
    try:
        if cfg["provider"] == "anthropic":
            return sr._anthropic_chat(cfg, messages), True
        return sr._openai_chat(cfg, messages), True
    except Exception as e:  # noqa: BLE001
        print("Предупреждение: AI-модель недоступна ({}), отчёт без генерации.".format(e),
              file=sys.stderr)
        return data_text, False


def main():
    import argparse
    ap = argparse.ArgumentParser(description="Собирает md-отчёт из heat-output")
    ap.add_argument("--input-dir", default="./heat-output")
    ap.add_argument("--output-md", default="")
    ap.add_argument("--max-heats", type=int, default=100)
    ap.add_argument("--ai", action="store_true", help="генерировать AI-отчёт")
    args = ap.parse_args()

    report = build_heat_report(args.input_dir, args.max_heats)
    if args.ai:
        ai_input = build_ai_input(args.input_dir, args.max_heats)
        ai_report, used = generate_ai_report(ai_input)
        if used:
            report = "# Отчёт по плавкам (AI)\n\n" + ai_report + "\n\n---\n\n## Сводка\n\n" + report
    if args.output_md:
        with open(args.output_md, "w", encoding="utf-8") as fh:
            fh.write(report + "\n")
    else:
        print(report)
    return 0


if __name__ == "__main__":
    main()
