# Round 249 — Переименование dedup_test.py → dedup-test.py (единый стиль имён)

## Задача
Переименовать `dedup_test.py` в `dedup-test.py` для единого стиля имён скриптов
(`_` → `-`), как в round-247.

## Реализация
- `git mv dedup_test.py dedup-test.py`.
- Обновлён docstring: `python3 dedup_test.py` → `python3 dedup-test.py`.

## Поиск мёртвых файлов
Проведён анализ корня и `scripts/` на файлы с 0 внешних ссылок:
- 10 кандидатов в корне (`ods-test-query-datahub.py`, `ods-test-query-datahub-diag.py`,
  `ods-test-stress-datahub.py`, `dedup_test.py`, `cleanup.sh`, `run-load-test.sh`,
  `archive-src.sh`, `dos2unix-recursive.py`, `rebuild-and-run-mac.sh`,
  `test-memory-leaks.sh`).
- В `scripts/` ложные срабатывания: модули, импортируемые через `import`
  (`metrics_common.py`, `e2e_utils.py`, `heat_report.py`, `spider_report.py`,
  `spider_report_pdf.py`) — их не переименовываем (Python не импортирует `-`).
- `compare-grafana-perses.py`, `comprehensive-performance-test.py`,
  `glitchtip-performance-e2e.py`, `sentry-mock-receiver.py` — описаны в README/TODO,
  не мёртвые.
- `update-vendored-libs.py` — ручной инструмент (не автоматизирован).

**По решению пользователя:** ничего не удалять (кандидаты остаются), только
переименован `dedup_test.py` → `dedup-test.py`.

## Проверка
- `dedup_test` ссылок не осталось (0).
- Синтаксис Python OK.
- rebuild-and-run.sh зелёный (Perses 10/10).
- message_counter: no message loss.
