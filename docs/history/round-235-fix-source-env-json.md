# Round 235 — Фикс `source .env` с JSON-массивом (rebuild-and-run.sh)

## Задача
`./rebuild-and-run.sh` падал на строке 21 `.env`:
```
.env: line 21: https://odskc2.nlmk/DataHubRobots/DataHubSrv,: No such file or directory
```
Причина — `source .env` выполнял строку `L2_SERVER_URLS=["http://...", "https://..."]`
как bash-код: `[` раскрывается как глоббинг/команда, URL-адрес выполняется как
команда → ошибка.

## Причина
`rebuild-and-run.sh` использовал `set -a; source .env; set +a`. Строки с
JSON-массивами (и вообще со сложными значениями) не являются валидным bash-кодом,
и `source` их интерпретирует как команды.

## Реализация
В `rebuild-and-run.sh` замена `source .env` на безопасный построчный парсер:
```bash
if [ -f .env ]; then
    set -a
    while IFS='=' read -r _key _value; do
        case "$_key" in
            '' | \#* | *[[:space:]]*) continue ;;
        esac
        case "$_value" in
            *'['* | *[[:space:]]*) continue ;;
        esac
        export "$_key=$_value"
    done < .env
    set +a
fi
```
Загружаются только простые `VAR=value` строки (без ведущих пробелов/комментариев);
значения с `[` (JSON-массивы) или пробелами пропускаются. `docker compose` по-прежнему
сам читает `.env` для подстановки `${VAR}` (включая JSON-массивы), поэтому
`L2_SERVER_URLS` продолжает работать.

## Проверка
- `./rebuild-and-run.sh` — проходит без ошибки `.env: line 21`, сборка зелёная.
- Все сервисы healthy (nats-server, l2-proxy, l2-worker).
- `message_counter.py --iterations 1 --concurrent 1` — exit 0 (no message loss).
- Загружаются нужные build-переменные (L2_WORKER_DOCKER_TARGET=runtime-db,
  REGISTRY), JSON-массив L2_SERVER_URLS корректно пропущен.

## Ключевое решение
Заменить `source .env` (выполняет весь файл как bash, ломается на JSON-массивах)
на построчный безопасный парсер, который экспортирует только простые `VAR=value`.
