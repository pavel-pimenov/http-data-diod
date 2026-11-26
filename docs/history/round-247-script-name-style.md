# Round 247 — Единый стиль имён исполняемых скриптов (_ → -)

## Задача
Обобщить стиль имён исполняемых Python-скриптов: заменить `_` на `-`
(`heat_ai_server` → `heat-ai-server` и т.п.).

## Объём (по решению пользователя)
Переименованы **только исполняемые скрипты** (не импортируемые модули):
- `scripts/heat_ai_server.py` → `scripts/heat-ai-server.py`
- `scripts/heat_scraper.py` → `scripts/heat-scraper.py`
- `scripts/spider_ai_server.py` → `scripts/spider-ai-server.py`

Импортируемые модули (`metrics_common.py`, `e2e_utils.py`, `heat_report.py`,
`spider_report.py`, `spider_report_pdf.py`) **оставлены с `_`** — Python не может
импортировать файл с `-` в имени, переименование сломало бы `import` (потребовался
бы importlib).

## Изменения
### `src/Dockerfile`
- `COPY scripts/heat_ai_server.py` → `COPY scripts/heat-ai-server.py`, ENTRYPOINT
  `/usr/local/bin/heat-ai-server.py`.
- `COPY scripts/heat_scraper.py` → `scripts/heat-scraper.py`, ENTRYPOINT
  `heat-scraper.py`.
- `COPY scripts/spider_ai_server.py` → `scripts/spider-ai-server.py`, ENTRYPOINT
  `spider-ai-server.py`.
- Комментарий про heat-ai-server обновлён.
- Имена Dockerfile-стадий (`heat-scraper` и др.) уже были с дефисом — не менялись.

### `scripts/heat_report.py`
- Docstring: `heat_scraper` → `heat-scraper`.

## Ключевое
- Переименованные файлы не импортируются другими как модули — их внутренние
  импорты (`import heat_report`, `import spider_report`, `import spider_report_pdf`)
  не затронуты, т.к. эти модули остались с `_`.
- `docker-context-check.py` (гейт) не ссылается на переименованные — проверено.

## Проверка
- rebuild-and-run.sh зелёный (Perses 10/10).
- Сервисы `l2-heat-ai`, `l2-heat-scraper`, `l2-spider-ai` — healthy (переименованные ENTRYPOINT).
- `docker-context-check.py` OK.
- message_counter: no message loss.
