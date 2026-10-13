# Round 99: прогон ASan / LSan / UBSan

## Что сделано

Полноценный санитайзерный прогон всего стека (`rebuild-and-run.sh --asan`):
Debug-сборка `l2-proxy`/`l2-worker`/`l2-server` с
`-fsanitize=address,undefined`, `detect_leaks=1` (LSan). Проверены и
unit-тесты в фазе сборки, и рантайм трёх сервисов под нагрузкой.

## Методика

1. `./rebuild-and-run.sh --asan` — сборка + unit-тесты (`test_components`,
   `test_proxy_core`) под ASan/UBSan, поднятие сервисов на `runtime-asan`.
2. `./health-check.sh all` — rc=0.
3. `python3 message_counter.py --iterations 1 --concurrent 1` — success.
4. e2e-трафик: `scripts/e2e-graceful-shutdown-test.py` (флуд и graceful
   shutdown по трём сервисам: `l2-proxy`/`l2-worker`/`l2-server`, 16382
   in-flight запроса, все завершены, ExitCode 0 — LSan получает шанс
   отработать на выходе), `scripts/db-gateway-e2e-test.py` (7/7).
5. Стресс: `message_counter.py --iterations 30 --concurrent 8` (528 rps).
6. Повторный проход со строгими опциями:
   `ASAN_OPTIONS=...:detect_stack_use_after_return=1:check_initialization_order=1:strict_string_checks=1`.

## Результат: находок нет

* В логе сборки (unit-тесты под ASan/UBSan) — 0 `runtime error`, 0
  `SUMMARY: AddressSanitizer`.
* В рантайме — 0 сообщений ASan/UBSan/LSan (`docker compose logs`).
* `./docker-memory-analysis/` пуст: ни ASan-репортов, ни LSan-дампов
  (`detect_leaks=1`, `log_path=/memory-logs/<service>-asan`).

### Sanity-check механизма LSan

Чтобы убедиться, что «пустой каталог» — это действительно «нет утечек», а не
«LSan молчит», в builder-контейнере собрана намеренно утекающая программа
(`new int(42); return 0;`) и запущена с тем же `log_path`. LSan создал дамп
`leakasan.<pid>` с `SUMMARY: AddressSanitizer: 4 byte(s) leaked` — механизм
пишет в смонтированный каталог и отрабатывает на выходе. Значит чистота
сервисов достоверна.

## Найденный дефект конфига

В `docker-compose.yml` у трёх сервисов в значение `ASAN_OPTIONS` был вписан
хвост `:ubsan_options=print_stacktrace=1`. Это неизвестный для ASan ключ —
он молча игнорируется, к UBSan отношения не имеет. Корректный
`UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=0}` рядом уже
есть, так что хвост был мёртвым. Удалён из `l2-server`, `l2-proxy`,
`l2-worker`.

## Попутно (не санитайзер)

`scripts/e2e-worker-reconnect-test.py` упал на проверке готовности
`nats-server` после рестарта (таймаут 60 с) — под asan-сборкой хост нагружен и
nats поднимался дольше. `nats-server` затем `healthy`, санитайзер-вывода нет —
это тайминговый флейк харнесса, не дефект кода.

## Проверка

* `./rebuild-and-run.sh --asan`: rc=0, unit-тесты зелёные, сервисы healthy.
* e2e graceful shutdown: rc=0, три сервиса ExitCode 0.
* db-gateway e2e: 7/7.
* `./rebuild-and-run.sh` (возврат к обычной сборке после правки compose):
  rc=0, `./health-check.sh all` passed, `message_counter.py
  --iterations 1 --concurrent 1` success.
