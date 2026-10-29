# round 207 — внешний сторож (cron) и лимиты логов Docker

## Что было не так

Round 205 научил `health-check.sh` видеть наблюдаемость и диск, но оставил
две дыры, обе всплыли в инциденте 27.10.2026 (`l2-victoria-metrics` умер с
`no space left on device` и лежал ~43 часа):

1. **Проверку никто не запускал.** `health-check.sh` — ручная команда.
   `vmalert` о падении VictoriaMetrics сообщить не может (он спрашивает тот
   же VictoriaMetrics), значит внешний сторож обязателен. Cron на хосте был
   пуст (`no crontab for dc`), systemd-юнит поставить нельзя — у оператора
   нет sudo (`sudo: interactive authentication is required`).
2. **Логи Docker росли без ограничения.** Лимит `json-file` `20m × 5` был
   только у `l2-proxy`, `l2-worker` и `l2-server`. У остальных 15 сервисов
   (`nats-server`, `nginx`, `jaeger`, `victoria-metrics`, `vmagent`,
   `vmalert`, `perses`, `postgres` и т. д.) `LogConfig` был безлимитным —
   прямой путь к тому самому «диск кончился».

## Что сделано

### `scripts/install-health-cron.sh`

Идемпотентный установщик пользовательского cron-задания:

```bash
./scripts/install-health-cron.sh            # поставить/обновить
./scripts/install-health-cron.sh --print    # показать строку
./scripts/install-health-cron.sh --status   # стоит ли + хвост лога
./scripts/install-health-cron.sh --remove   # снять
```

- Строка crontab помечена комментарием
  `# http-data-diod-health-check (managed by ...)`; повторная установка
  заменяет её, а не добавляет вторую (проверено: после двух `--install` в
  crontab ровно одна строка).
- `--remove` вырезает только управляемую строку и маркер, остальные записи
  пользователя не трогает.
- `flock -n /tmp/http-data-diod-health.lock` — медленный прогон (рестарт +
  `sleep 10` + повторные попытки) не наслаивается на следующий тик.
- `PATH=/usr/local/bin:/usr/bin:/bin` прописан явно: у cron минимальное
  окружение, `docker` лежит в `/usr/bin`.
- Лог `/tmp/http-data-diod-health.log` усекается на месте, когда превышает
  1 МиБ — отдельный logrotate не нужен.
- Параметры: `HEALTH_CRON_INTERVAL` (15 мин), `HEALTH_CRON_LOG`,
  `HEALTH_CRON_RETRIES` (2), `HEALTH_CRON_MAX_LOG`, `HEALTH_CRON_LOCK`.
  Интервал валидируется (1..1440), при отсутствии `crontab`/`flock` —
  понятная ошибка.

### Лимиты логов в `docker-compose.yml`

Добавлен якорь верхнего уровня:

```yaml
x-logging: &default-logging
  driver: json-file
  options:
    max-size: "20m"
    max-file: "5"
```

и `logging: *default-logging` у всех долгоживущих сервисов (18 штук):
`nats-server`, `nats-exporter`, `nats-gui`, `jaeger`, `victoria-metrics`,
`vmagent`, `vmalert`, `l2-server`, `oracle`, `glitchtip-db`, `glitchtip`,
`postgres`, `swagger-ui`, `l2-proxy`, `nginx`, `nginx-exporter`, `l2-worker`,
`perses`. Три прежних инлайн-блока у `l2-*` заменены ссылкой на якорь, чтобы
значение было одно. `l2-test` пропущен осознанно — это одноразовый
`curl`-контейнер, ему лимит не нужен.

Потолок — ~100 МБ на контейнер вместо неограниченного роста.

## Проверка

- `docker compose config -q` и `COMPOSE_FILE=docker-compose.yml:docker-compose.live.yml
  docker compose config -q` — валидно; в развёрнутом конфиге 14 записей
  `max-size: 20m` (18 минус 4 профильных сервиса, не входящих в дефолтный
  профиль: `oracle`, `postgres`, `glitchtip`, `glitchtip-db`).
- `sh -n` для сгенерированной cron-строки — синтаксис ок; прогон строки в
  минимальном окружении (`env -i SHELL=/bin/sh HOME=... PATH=/usr/bin:/bin`)
  — rc=0, лог заполнился, «All health checks passed».
- Усечение лога: при `HEALTH_CRON_MAX_LOG=1000` и логе в 5000 байт после
  прогона файл обнулён.
- `--install` дважды → одна строка; `--remove` → `no crontab for dc`;
  `--status` показывает строку и хвост лога.
- `./scripts/run-shellcheck.sh --all` — rc=0, у нового скрипта замечаний нет.
- После пересборки стека `docker inspect` подтверждает `LogConfig` у
  ранее безлимитных сервисов.

## Ограничения (честно)

- Пользовательский cron живёт, пока живёт хост и запущен cron-демон; это не
  systemd-юнит с автоперезапуском. Проверка `systemctl is-active cron` —
  `active`.
- Cron не спасает от сценария «диск кончился настолько, что не пишется даже
  лог»: тогда остаётся только внешний мониторинг хоста.
- `--remove` на пустом crontab удаляет crontab целиком (`crontab -r`) —
  ожидаемо, других записей там нет.
- Лимит логов не уменьшает уже накопленные файлы; старые логи нужно чистить
  вручную (`docker system prune`), что и делалось в round 205.
