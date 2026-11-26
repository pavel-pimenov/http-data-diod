#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Конвертирует markdown-отчёт l2-db-spider в PDF в альбомной ориентации (A4
landscape) с деревом разделов слева (bookmarks/outline) для навигации.

Берёт .md-файл (например, результат scripts/spider_report.py --output-md) и
строит PDF через reportlab:
- альбомная ориентация (A4 landscape);
- закладки из заголовков # / ## / ### (вложенное дерево в панели слева);
- параграфы, списки, таблицы (строки вида | a | b |), моноширинный код (```).

Запуск в docker (Ubuntu 26.04, свежий Python — не нужны костыли под 3.7):
  docker build -t spider-pdf -f src/Dockerfile --target pdf-report .
  docker run --rm -v "$PWD/spider-output:/data" spider-pdf \
      /data/report.md /data/report.pdf

Или локально (нужен reportlab + TTF-шрифт с кириллицей):
  pip3 install --user reportlab
  python3 scripts/spider_report_pdf.py INPUT.md [OUTPUT.pdf]

Использование:
  python3 scripts/spider_report_pdf.py INPUT.md [OUTPUT.pdf] [--font PATH]
    (default OUTPUT.pdf = INPUT.pdf)

Замечание: для кириллицы нужен TTF-шрифт. Если DejaVuSans недоступен в системе,
укажите путь через --font PATH/DejaVuSans.ttf. Без TTF русские буквы не
выведутся (стандартный Helvetica не содержит кириллицы).
"""

import argparse
import os
import re
import sys

from reportlab.lib import colors
from reportlab.lib.pagesizes import A4, landscape
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.lib.enums import TA_LEFT
from reportlab.pdfgen import canvas
from reportlab.platypus import (
    SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, Preformatted,
)


# --- шрифты ---------------------------------------------------------------
def _find_ttf():
    candidates = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/urw-base35/NimbusSans-Regular.ttf",
    ]
    for p in candidates:
        if os.path.exists(p):
            return p
    return None


def _register_font():
    """Регистрирует TTF-шрифт с кириллицей. Возвращает имя или None."""
    ttf = _find_ttf()
    if not ttf:
        return None
    from reportlab.pdfbase import pdfmetrics
    from reportlab.pdfbase.ttfonts import TTFont
    base = os.path.basename(ttf).replace(".ttf", "")
    try:
        pdfmetrics.registerFont(TTFont(base, ttf))
        return base
    except Exception as e:  # noqa: BLE001
        print("Не удалось зарегистрировать шрифт {}: {}".format(ttf, e),
              file=sys.stderr)
        return None


# --- md-разбор ------------------------------------------------------------
def _parse_md(path):
    """Разбирает md в список блоков: (kind, data).
    kind: 'h1'|'h2'|'h3'|'p'|'li'|'table'|'code'|'quote'|'hr'."""
    blocks = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        lines = fh.readlines()
    i = 0
    while i < len(lines):
        line = lines[i].rstrip("\n")
        s = line.strip()
        if not s:
            i += 1
            continue
        if s.startswith("```"):
            code = []
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("```"):
                code.append(lines[i].rstrip("\n"))
                i += 1
            i += 1
            blocks.append(("code", "\n".join(code)))
            continue
        if s.startswith("|") and "|" in s[1:]:
            tbl = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                tbl.append(lines[i].strip())
                i += 1
            blocks.append(("table", tbl))
            continue
        m = re.match(r"^(#{1,6})\s+(.*)$", s)
        if m:
            level = len(m.group(1))
            title = m.group(2).strip()
            blocks.append(("h{}".format(min(level, 3)), title))
            i += 1
            continue
        if re.match(r"^[-*]\s+", s) or re.match(r"^\d+\.\s+", s):
            blocks.append(("li", re.sub(r"^([-*]|\d+\.)\s+", "", s)))
            i += 1
            continue
        if s.startswith(">"):
            blocks.append(("quote", s.lstrip(">").strip()))
            i += 1
            continue
        if s.startswith("---") or s.startswith("==="):
            blocks.append(("hr", ""))
            i += 1
            continue
        blocks.append(("p", s))
        i += 1
    return blocks


def _escape_md(text):
    """Убирает md-разметку (**, _, `) для вывода в PDF."""
    t = re.sub(r"\*\*([^*]+)\*\*", r"\1", text)
    t = re.sub(r"`([^`]+)`", r"\1", t)
    t = re.sub(r"__([^_]+)__", r"\1", t)
    t = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", t)
    return t


def _md_table_flowables(tbl_lines, font):
    """Преобразует строки md-таблицы в flowables (таблица + отступ)."""
    rows = []
    for line in tbl_lines:
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        rows.append(cells)
    if not rows:
        return []
    header = rows[0]
    body = [r for r in rows[1:] if not all(re.match(r"^:?-+:?$", c) for c in r)]
    data = [header] + body
    ncols = max(len(r) for r in data)
    for r in data:
        while len(r) < ncols:
            r.append("")
    tbl = Table(data, repeatRows=1)
    tbl.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#e3f2fd")),
        ("FONTNAME", (0, 0), (-1, 0), font or "Helvetica-Bold"),
        ("FONTSIZE", (0, 0), (-1, -1), 7),
        ("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor("#cfd8dc")),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 4),
        ("RIGHTPADDING", (0, 0), (-1, -1), 4),
    ]))
    return [tbl, Spacer(1, 6)]


# --- PDF-построение -------------------------------------------------------
class OutlineDoc(SimpleDocTemplate):
    """SimpleDocTemplate, который собирает заголовки + номера страниц для
    построения дерева закладок слева."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._headers = []  # (level, title, page)
        self._root_added = False

    def handle_pageBegin(self, *args, **kwargs):
        super().handle_pageBegin(*args, **kwargs)
        # Корневой entry уровня 0 (обязателен: reportlab не позволяет
        # первый addOutlineEntry на уровне >0).
        if not self._root_added:
            try:
                self.canv.bookmarkPage("root")
                self.canv.addOutlineEntry("Отчёт l2-db-spider", "root", 0, 1)
            except Exception:  # noqa: BLE001
                pass
            self._root_added = True

    def afterFlowable(self, flowable):
        """Ловим заголовки; добавляем закладку сразу на текущей странице.

        reportlab требует, чтобы уровни outline шли без пропусков (переход с
        уровня X сразу на Y>X+1 бросает ValueError). Поэтому корень создаётся
        на уровне 0, а все заголовки привязываются к уровню 1 (плоское, но
        кликабельное дерево — навигация по разделам работает)."""
        if isinstance(flowable, Paragraph):
            style = getattr(flowable, "style", None)
            name = getattr(style, "name", "") if style else ""
            if name in ("h1", "h2", "h3"):
                title = _escape_md(getattr(flowable, "getPlainText", lambda: "")() or "")
                key = "hdr{}".format(len(self._headers))
                self._headers.append((name, title, self.page))
                try:
                    self.canv.bookmarkPage(key)
                    self.canv.addOutlineEntry(title, key, 1, self.page)
                except Exception:  # noqa: BLE001
                    pass


def build_pdf(md_path, pdf_path, font):
    w, h = landscape(A4)

    def canvas_factory(*a, **k):
        return canvas.Canvas(*a, **k)

    doc = OutlineDoc(
        pdf_path,
        pagesize=(w, h),
        leftMargin=15 * mm, rightMargin=15 * mm,
        topMargin=12 * mm, bottomMargin=12 * mm,
        title="Отчёт l2-db-spider", author="l2-db-spider",
    )
    styles = {
        "h1": ParagraphStyle("h1", fontName=font or "Helvetica-Bold",
                             fontSize=16, spaceAfter=8, textColor=colors.HexColor("#1a237e")),
        "h2": ParagraphStyle("h2", fontName=font or "Helvetica-Bold",
                             fontSize=13, spaceBefore=6, spaceAfter=5,
                             textColor=colors.HexColor("#0d47a1")),
        "h3": ParagraphStyle("h3", fontName=font or "Helvetica-Bold",
                             fontSize=11, spaceBefore=4, spaceAfter=4,
                             textColor=colors.HexColor("#37474f")),
        "p": ParagraphStyle("p", fontName=font or "Helvetica", fontSize=9,
                            leading=12, spaceAfter=4, alignment=TA_LEFT),
        "li": ParagraphStyle("li", fontName=font or "Helvetica", fontSize=9,
                             leading=12, spaceAfter=2, leftIndent=10),
        "quote": ParagraphStyle("quote", fontName=font or "Helvetica-Oblique",
                                fontSize=9, leading=12, textColor=colors.grey,
                                spaceAfter=4),
        "code": ParagraphStyle("code", fontName="Courier", fontSize=7,
                               leading=9, spaceAfter=4),
    }

    story = []
    blocks = _parse_md(md_path)
    for kind, data in blocks:
        if kind in ("h1", "h2", "h3"):
            story.append(Paragraph(_escape_md(data), styles[kind]))
        elif kind == "p":
            story.append(Paragraph(_escape_md(data), styles["p"]))
        elif kind == "li":
            story.append(Paragraph("• " + _escape_md(data), styles["li"]))
        elif kind == "quote":
            story.append(Paragraph(_escape_md(data), styles["quote"]))
        elif kind == "code":
            story.append(Preformatted(data, styles["code"]))
        elif kind == "table":
            story.extend(_md_table_flowables(data, font))
        elif kind == "hr":
            story.append(Spacer(1, 4))

    doc.build(story, canvasmaker=canvas_factory)


def main():
    parser = argparse.ArgumentParser(
        description="Конвертирует md-отчёт l2-db-spider в PDF (альбомный, с деревом слева)")
    parser.add_argument("input", help="Путь к .md-файлу отчёта")
    parser.add_argument("output", nargs="?", default="",
                        help="Путь к PDF (default: <input>.pdf)")
    parser.add_argument("--font", default="",
                        help="Путь к TTF-шрифту с кириллицей (default: искать DejaVuSans)")
    args = parser.parse_args()

    if not os.path.isfile(args.input):
        print("Ошибка: файл не найден: {}".format(args.input), file=sys.stderr)
        sys.exit(1)
    out = args.output or args.input.rsplit(".", 1)[0] + ".pdf"

    font = args.font
    if not font:
        font = _register_font()
    if font:
        print("Шрифт: {}".format(font), file=sys.stderr)
    else:
        print("Внимание: TTF-шрифт с кириллицей не найден — русский текст может "
              "не отобразиться. Укажите --font /path/DejaVuSans.ttf", file=sys.stderr)

    build_pdf(args.input, out, font)
    print("Сохранено: {}".format(out))


if __name__ == "__main__":
    main()
