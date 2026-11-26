# Round 223 — Сервис l2-heat-ai: AI-генерация отчёта по 500 плавкам

## Задача
Создать сервис, который на HTML-странице (новый порт) отдаёт отчёт, сгенерированный
AI после анализа 500 собранных плавок (GetHeatData). По аналогии с l2-spider-ai
(генерация AI-отчёта по shared_pool баз Oracle).

## Реализация
### `scripts/heat_report.py`
Анализ heat-файлов (`heat_*.json` от heat_scraper):
- `analyze_heat_file` — разбор файла (heat_no/http_status/response), поддержка
  двух форматов данных (старый: `attributes` вложенные; новый: атрибуты прямо в
  handling, ключ `type`);
- `build_heat_report` — человекочитаемая сводка (для справки в md);
- **`build_ai_input` — АГРЕГИРОВАННЫЙ дамп** для AI: 500 плавок в полном виде
  = 7.6 МБ (не вмещаются в контекст модели), поэтому считает компактную
  статистику: распределение по агрегатам, типы операций с длительностями,
  диапазоны температур/весов, химсостав, аномалии (>120 мин) + детальные примеры.
  Итог: **41 КБ (~21К токенов)** — влезает в контекст 262К.
- `_AI_HEAT_PROMPT` — системный промт: типовой сценарий, анализ по агрегатам,
  химсостав, аномалии, рекомендации с heat_no-ссылками (или таблицей в PDF);
- `generate_ai_report` — через `sr._load_ai_config()` / `sr._openai_chat()`.

### `scripts/heat_ai_server.py`
Веб-сервер прогресса по аналогии со spider_ai_server:
- `GET /` — HTML-страница (автообновление, кнопка «Запустить генерацию»);
- `POST /generate` — фоновый запуск;
- `GET /progress`, `/report.md`, `/report.pdf`.

### `src/Dockerfile` — stage `heat-ai`
На базе pdf-report (python3 + reportlab + шрифты); копирует heat_report.py,
heat_ai_server.py, spider_report.py (для конфига AI/чата), spider_report_pdf.py.
EXPOSE 19097.

### `docker-compose.yml` — сервис `l2-heat-ai`
- Порт **19097**, volume `./heat-output:/work/heat-output`, restart unless-stopped;
- env: HEAT_AI_PORT, HEAT_AI_MAX_HEATS (500), AI_* (конфиг модели);
- healthcheck на /progress.

### `www/index.html` — плашка l2-heat-ai на хабе (порт 19097).

### `.env.example`
- Добавлены HEAT_AI_PORT, HEAT_AI_MAX_HEATS.

## Проверка
- `docker compose build l2-heat-ai` + `up -d` — образ собран, контейнер healthy.
- `POST /generate` → running → done (без ошибок):
  - агрегированный дамп 41 КБ (~21К токенов) — модель ответила за ~4 мин;
  - отчёт `heat-output/ai/report.md` (14 КБ) — связный аналитический: общая сводка
    по 500 плавкам (72730..73229), типовой сценарий, анализ по агрегатам (миксеры,
    УДЧ, конвертеры, доводка, УНРС), химсостав, аномалии, приоритетные рекомендации;
  - `heat-output/ai/report.pdf` (45 КБ);
- HTML-страница :19097 отдаёт report.md (200, 14 КБ) и report.pdf (200, 45 КБ).

## Ключевое решение
500 плавок в полном виде = 7.6 МБ > контекста модели → **агрегированный дамп**
(статистика + примеры) вместо построчного перебора каждой плавки.
