# Round 218 — l2-spider-ai: контейнер AI-генерации отчёта с веб-страницей прогресса

## Задача
AI-модель (DeepSeek-V4-Flash, endpoint 10.112.3.51:3004) очень медленная: полный
дамп данных l2-db-spider (~230 строк/файл, влезает в контекст 262144) она
обрабатывала ~10 минут и упиралась в отсутствие лимита вывода. Нужно было:
- изолировать генерацию в отдельный контейнер, чтобы освободить модель от
  текущей сессии opencode;
- создать веб-страницу, отражающую прогресс работы модели, на отдельном порту.

## Решение

### 1. Оптимизация AI-запроса в `scripts/spider_report.py`
- `build_ai_input(input_dir, max_rows)`: полный дамп JSON-данных для модели.
  RAW-колонки (OPTIMIZER_ENV, ADDRESS, BIND_DATA и т.п.) исключаются,
  строковые поля обрезаются до 120 символов, но ВСЕ строки передаются
  (ограничивается только `max_rows` на файл — топ по значимой метрике:
  ELAPSED_TIME для shared_pool, BYTES для table_sizes).
- `_openai_chat` добавляет `max_tokens` (env AI_MAX_TOKENS, default 6000):
  без него модель генерировала до бесконечности → таймаут. Это главный фикс.
- CLI `--ai-rows` (default 40): лимит строк на файл. 40 строк ≈ ~40K токенов,
  генерация ~4 минуты; 230 ≈ полный контекст, но ~10 минут.
- `AI_TIMEOUT` default 600с.

### 2. Веб-сервер прогресса `scripts/spider_ai_server.py`
Чистый Python (`http.server`), без сторонних зависимостей:
- `GET /`            — HTML-страница прогресса (автообновление каждые 2 с),
- `POST /generate`   — запуск фоновой генерации (в отдельном потоке),
- `GET /progress`    — JSON-статус: status/stage/log/report/pdf/error,
- `GET /report.md`   — последний сгенерированный отчёт,
- `GET /report.pdf`  — PDF-версия (альбомный, дерево слева).
Этапы генерации логируются: чтение JSON → сборка таблиц → дамп данных →
запрос к модели → сборка md → генерация PDF.

### 3. Docker-образ `spider-ai` (src/Dockerfile)
- Новый stage `spider-ai` наследует `pdf-report` (Ubuntu 26.04 + reportlab +
  DejaVuSans для кириллицы), добавляет `spider_report.py` + `spider_ai_server.py`.
- `EXPOSE 19096`, ENTRYPOINT запускает веб-сервер.

### 4. Сервис `l2-spider-ai` (docker-compose.yml)
- Контекст сборки `.` (корень) + dockerfile `src/Dockerfile` — чтобы COPY
  нашёл корневые scripts (раньше контекст ./src не видел их).
- Порт 19096, том `./spider-output:/work/spider-output`, env AI_*.
- healthcheck на `/progress`.

### 5. Конфигурация
- `.env` (git-ignored): AI_API_URL=http://10.112.3.51:3004/v1, AI_API_KEY=48,
  AI_MODEL=DeepSeek-V4-Flash-0731-UD-IQ3_XXS, AI_TIMEOUT=600, AI_AI_ROWS=40,
  AI_MAX_TOKENS=6000.
- `.env.example`: задокументированы AI_API_URL/AI_API_KEY/AI_MODEL/AI_AI_ROWS/
  AI_MAX_TOKENS.
- `.gitignore`: добавлен `*.pdf` (PDF-отчёты не коммитим).

## Проверка
- Сервис `l2-spider-ai` Up/healthy, `GET /` отдаёт страницу, `/progress` — JSON.
- `POST /generate` запускает фоновую генерацию; прогресс виден через
  `/progress` (статус running → done за 240 с при 40 строках/файл).
- Итог: `spider-output/ai/report.md` (222 КБ) — связный аналитический отчёт
  с рекомендациями по каждой базе (ccm2/ccm4/ccm5), конкретные SQL_ID,
  модули, схемы, сегменты, приоритизация (критично/важно/желательно);
  `spider-output/ai/report.pdf` (298 КБ) — альбомный A4 с деревом разделов слева
  (bookmarks/outline).
- Модель ответила за ~4 минуты (раньше 10+ мин / таймаут из-за отсутствия
  max_tokens).
