#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Веб-сервер прогресса AI-генерации отчёта по плавкам (l2-heat-ai).

По аналогии с l2-spider-ai. Поднимает HTTP-сервис (порт из env HEAT_AI_PORT,
default 19097), который:
  GET  /            — HTML-страница прогресса (автообновляется),
  POST /generate    — запускает фоновую генерацию отчёта (через модель),
  GET  /progress    — JSON-статус прогресса,
  GET  /report.md   — текущий сгенерированный отчёт,
  GET  /report.pdf  — PDF-версия отчёта (если собрана).

Генерация в фоновом потоке: читает heat_*.json из <input_dir>, строит дамп,
шлёт запрос к внешней модели и пишет отчёт в <output_dir>.

Запуск:
  python3 scripts/heat_ai_server.py [--input-dir PATH] [--output-dir PATH]
  env: HEAT_AI_PORT, AI_API_URL, AI_API_KEY, AI_MODEL, HEAT_AI_MAX_HEATS
"""

import argparse
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import heat_report as hr  # noqa: E402
import spider_report as sr  # noqa: E402  (для _load_ai_config fallback)


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.status = "idle"            # idle | running | done | error
        self.stage = ""
        self.stage_detail = ""
        self.log = []
        self.started_at = 0.0
        self.finished_at = 0.0
        self.report_path = ""
        self.pdf_path = ""
        self.error = ""

    def append_log(self, msg):
        with self.lock:
            self.log.append((time.strftime("%H:%M:%S"), msg))

    def set(self, **kw):
        with self.lock:
            for k, v in kw.items():
                setattr(self, k, v)

    def snapshot(self):
        with self.lock:
            return {
                "status": self.status,
                "stage": self.stage,
                "stage_detail": self.stage_detail,
                "log": list(self.log),
                "started_at": self.started_at,
                "finished_at": self.finished_at,
                "report": os.path.basename(self.report_path) if self.report_path else "",
                "pdf": os.path.basename(self.pdf_path) if self.pdf_path else "",
                "error": self.error,
            }


state = State()


def generate_worker(input_dir, output_dir, max_heats, cfg):
    """Фоновая генерация отчёта по плавкам."""
    try:
        state.set(status="running", stage="init",
                  stage_detail="Чтение heat-файлов из {}".format(input_dir),
                  started_at=time.time(), error="")
        state.append_log("Читаю heat-файлы из {}".format(input_dir))

        # 1. сводка (для справки в итоговом md)
        state.set(stage="tables", stage_detail="Сборка сводки по плавкам")
        state.append_log("Сборка сводки по плавкам...")
        tables = hr.build_heat_report(input_dir, max_heats)

        # 2. дамп данных для модели
        state.set(stage="dump",
                  stage_detail="Сборка дампа данных (до {} плавок)".format(max_heats))
        state.append_log("Сборка дампа данных (до {} плавок)...".format(max_heats))
        ai_input = hr.build_ai_input(input_dir, max_heats)

        # 3. отправка в модель
        state.set(stage="ai", stage_detail="Запрос к модели {} (контекст ~{}K токенов)".format(
            cfg.get("model"), len(ai_input) // 2000))
        state.append_log("Отправляю запрос к модели {}...".format(cfg.get("model")))
        state.append_log("Данных {} КБ, это может занять несколько минут.".format(len(ai_input) // 1024))  # noqa: E501
        report, used_ai = hr.generate_ai_report(ai_input)
        if not used_ai:
            raise RuntimeError("AI-модель не ответила (проверь AI_API_URL/AI_API_KEY)")
        state.append_log("Модель ответила, отчёт получен ({} КБ)".format(len(report) // 1024))

        # 4. итоговый md
        state.set(stage="md", stage_detail="Сборка итогового markdown-отчёта")
        out_md = ("# Отчёт по плавкам (l2-heat-ai)\n\n"
                  "> Сгенерирован AI-моделью на основе данных GetHeatData (500 плавок).\n\n" +
                  report + "\n\n---\n\n## Сводка данных\n\n" + tables)
        os.makedirs(output_dir, exist_ok=True)
        md_path = os.path.join(output_dir, "report.md")
        with open(md_path, "w", encoding="utf-8") as fh:
            fh.write(out_md + "\n")
        state.append_log("Отчёт сохранён: {}".format(md_path))

        # 5. PDF
        state.set(stage="pdf", stage_detail="Генерация PDF (альбомный, дерево слева)")
        try:
            import spider_report_pdf as srp
            pdf_path = os.path.join(output_dir, "report.pdf")
            srp.build_pdf(md_path, pdf_path, srp._register_font())
            state.append_log("PDF сохранён: {}".format(pdf_path))
            state.set(pdf_path=pdf_path)
        except Exception as e:  # noqa: BLE001
            state.append_log("PDF не собран: {}".format(e))
            state.set(pdf_path="")

        state.set(status="done", stage="done",
                  stage_detail="Готово", report_path=md_path,
                  finished_at=time.time())
        state.append_log("Готово за {:.0f} с".format(time.time() - state.started_at))
    except Exception as e:  # noqa: BLE001
        state.set(status="error", stage="error", stage_detail=str(e),
                  error=str(e), finished_at=time.time())
        state.append_log("Ошибка: {}".format(e))


_HTML = """<!DOCTYPE html>
<html lang="ru"><head><meta charset="utf-8">
<title>l2-heat-ai — генерация отчёта по плавкам</title>
<style>
  body{font-family:system-ui,Segoe UI,Roboto,sans-serif;background:#0f172a;color:#e2e8f0;
       margin:0;padding:24px;}
  h1{color:#38bdf8;} .card{background:#1e293b;border-radius:10px;padding:20px;margin-bottom:16px;}
  .stage{font-size:1.1em;color:#fbbf24;margin-bottom:8px;}
  .bar{height:8px;background:#334155;border-radius:4px;overflow:hidden;}
  .bar-fill{height:100%;width:0;background:#38bdf8;transition:width .5s;}
  pre{background:#0f172a;padding:12px;border-radius:6px;max-height:400px;overflow:auto;
      font-size:12px;white-space:pre-wrap;}
  .btn{background:#38bdf8;color:#0f172a;border:none;padding:10px 20px;border-radius:6px;
       font-size:1em;cursor:pointer;} .btn:disabled{opacity:.5;cursor:default;}
  a{color:#38bdf8;} .status-done{color:#4ade80;} .status-error{color:#f87171;}
</style></head><body>
<h1>l2-heat-ai — генерация отчёта по плавкам</h1>
<div class="card">
  <div>Статус: <span id="status" class="status-done">загрузка…</span></div>
  <div style="margin-top:8px">Этап: <span id="stage">—</span></div>
  <div id="detail" style="color:#94a3b8;margin-top:4px"></div>
  <div class="bar" style="margin-top:12px"><div class="bar-fill" id="bar"></div></div>
  <div style="margin-top:16px">
    <button class="btn" id="gen" onclick="startGen()">Запустить генерацию</button>
  </div>
  <div style="margin-top:12px">
    <a id="repLink" href="/report.md" target="_blank" style="display:none">Скачать report.md</a>
    &nbsp; <a id="pdfLink" href="/report.pdf" target="_blank" style="display:none">Скачать report.pdf</a>  # noqa: E501
  </div>
</div>
<div class="card"><b>Лог</b><pre id="log">—</pre></div>
<script>
let gen = false;
function setStatus(s){const el=document.getElementById('status');el.className='status-'+s;el.textContent=s;}  # noqa: E501
async function refresh(){
  try{
    const r=await fetch('/progress'); const j=await r.json();
    setStatus(j.status);
    document.getElementById('stage').textContent=j.stage_detail||j.stage||'—';
    document.getElementById('log').textContent=(j.log||[]).map(x=>x[0]+'  '+x[1]).join('\\n')||'—';
    const rep=document.getElementById('repLink');
    const pdf=document.getElementById('pdfLink');
    rep.style.display=j.report?'inline':'none'; rep.href='/report.md?'+Date.now();
    pdf.style.display=j.pdf?'inline':'none'; pdf.href='/report.pdf?'+Date.now();
    const bar=document.getElementById('bar');
    const pct=j.status==='done'?100:j.status==='running'?50:j.status==='error'?100:0;
    bar.style.width=pct+'%';
    document.getElementById('gen').disabled = (j.status==='running');
  }catch(e){setStatus('error');}
}
async function startGen(){
  document.getElementById('gen').disabled=true;
  const r=await fetch('/generate',{method:'POST'}); const j=await r.json();
  if(j.started){refresh(); setInterval(refresh,2000);}
}
refresh(); setInterval(refresh,2000);
</script></body></html>
"""


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/":
            self._send(200, "text/html; charset=utf-8", _HTML.encode("utf-8"))
        elif path == "/progress":
            self._send(200, "application/json; charset=utf-8",
                       json.dumps(state.snapshot(), ensure_ascii=False).encode("utf-8"))
        elif path in ("/report.md", "/report"):
            if state.report_path and os.path.exists(state.report_path):
                with open(state.report_path, "rb") as fh:
                    self._send(200, "text/markdown; charset=utf-8", fh.read())
            else:
                self._send(404, "text/plain", b"not ready yet")
        elif path == "/report.pdf":
            if state.pdf_path and os.path.exists(state.pdf_path):
                with open(state.pdf_path, "rb") as fh:
                    self._send(200, "application/pdf", fh.read())
            else:
                self._send(404, "text/plain", b"not ready yet")
        else:
            self._send(404, "text/plain", b"not found")

    def do_POST(self):
        path = urlparse(self.path).path
        if path == "/generate":
            if state.status == "running":
                self._send(200, "application/json", b'{"started":false,"msg":"already running"}')
                return
            t = threading.Thread(target=generate_worker,
                                 args=(self.server.input_dir, self.server.output_dir,
                                       self.server.max_heats, self.server.ai_cfg),
                                 daemon=True)
            t.start()
            self._send(200, "application/json", b'{"started":true}')
        else:
            self._send(404, "application/json", b'{"error":"not found"}')

    def log_message(self, *a, **k):
        pass


def main():
    ap = argparse.ArgumentParser(description="Веб-сервер прогресса AI-генерации по плавкам")
    ap.add_argument("--input-dir", default="./heat-output")
    ap.add_argument("--output-dir", default="./heat-output/ai")
    ap.add_argument("--port", type=int, default=int(os.environ.get("HEAT_AI_PORT", "19097")))
    ap.add_argument("--max-heats", type=int, default=int(os.environ.get("HEAT_AI_MAX_HEATS", "500")))  # noqa: E501
    args = ap.parse_args()

    cfg = sr._load_ai_config()
    if not cfg:
        cfg = {"url": os.environ.get("AI_API_URL", ""),
               "key": os.environ.get("AI_API_KEY", ""),
               "model": os.environ.get("AI_MODEL", ""),
               "provider": os.environ.get("AI_PROVIDER", "openai"),
               "timeout": int(os.environ.get("AI_TIMEOUT", "600")),
               "temperature": float(os.environ.get("AI_TEMPERATURE", "0.3"))}

    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    server.input_dir = args.input_dir
    server.output_dir = args.output_dir
    server.max_heats = args.max_heats
    server.ai_cfg = cfg
    state.append_log("Сервер запущен на порту {}".format(args.port))
    state.append_log("Вход: {} → выход: {}".format(args.input_dir, args.output_dir))
    print("l2-heat-ai слушает :{}".format(args.port), file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.shutdown()


if __name__ == "__main__":
    main()
