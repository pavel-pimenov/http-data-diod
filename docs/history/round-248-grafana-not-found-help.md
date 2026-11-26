# Round 248 — Справка в лог при недоступной Grafana (с подсветкой YAML)

## Задача
Когда sidecar-контейнер `grafana-dashboards-sync` не может подключиться к Grafana
(не найдена по GRAFANA_URL), скрипт генерации дашбордов должен выводить в лог
справочное сообщение: что добавить в docker-compose.yml, чтобы контейнер отработал,
с подсветкой синтаксиса YAML.

## Реализация
### `scripts/generate-grafana-dashboards.py`
- В `main()` при неудачном `api.test_connection()`:
  - `logger.error("Cannot connect to Grafana at %s. Exiting.", config['url'])`
  - вызов `_print_yaml_help()` — справочное сообщение.
- Новая функция `_print_yaml_help()`: печатает пример docker-compose-блока для
  `grafana-dashboards-sync` с ANSI-подсветкой YAML:
  - ключи — голубой (`\033[36m`),
  - значения — зелёный (`\033[32m`),
  - комментарии — жёлтый (`\033[33m`),
  - заголовок — жирный красный.
- Показывает: `image`, `restart`, `environment` (GRAFANA_URL/GRAFANA_USER/
  GRAFANA_PASSWORD), `networks`, и `curl` для проверки доступности.
- Длинная строка отмечена `# noqa: E501`.

## Ключевое
- Справка выводится **только** когда подключение к Grafana реально не удалось
  (не для `--dry-run`/`--check` — те не идут в Grafana).
- ANSI-подсветка не зависит от логирования (logger) — используется `print`,
  чтобы не портить формат логов.

## Проверка
- `GRAFANA_URL=http://127.0.0.1:3999 python3 scripts/generate-grafana-dashboards.py`
  → `Cannot connect to Grafana at ...` + справочное сообщение с подсветкой YAML.
- Синтаксис Python OK (нет backslash в f-string, Python 3.7).
- rebuild-and-run.sh зелёный (Perses 10/10).
- message_counter: no message loss.
- style-ошибки LONG100 в `heat-ai-server.py` — предсуществующие (round-247), не от этого round.
