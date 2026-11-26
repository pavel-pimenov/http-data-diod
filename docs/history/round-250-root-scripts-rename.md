# Round 250 — Переименование остальных корневых скриптов с `_` (единый стиль)

## Задача
Переименовать оставшиеся исполняемые корневые скрипты с `_` в `-` для единого стиля
(round-247/249).

## Реализация
Переименованы (все — исполняемые, не импортируются как модули):
- `load_test_memory.py` → `load-test-memory.py`
- `fault_tolerance_test.py` → `fault-tolerance-test.py`
- `rate_limit_test.py` → `rate-limit-test.py`

`test-crash-handler.py` — уже с дефисом, не менялся.

Обновлены docstring в переименованных файлах (строки использования).

## Обновление внешних ссылок
- `README.md`: `rate_limit_test.py` → `rate-limit-test.py`,
  `fault_tolerance_test.py` → `fault-tolerance-test.py`.
- `TODO.md`: `fault_tolerance_test.py` → `fault-tolerance-test.py`.
- `run-load-test.sh`: `load_test_memory.py` → `load-test-memory.py` (реальный вызов).

`load_test_memory.log` (имя файла лога, не скрипта) — не менялся.

## Проверка
- Остаточных ссылок на старые имена — 0 (кроме лог-файла).
- Синтаксис Python и bash (`run-load-test.sh`) OK.
- rebuild-and-run.sh зелёный (Perses 10/10).
- message_counter: no message loss.
