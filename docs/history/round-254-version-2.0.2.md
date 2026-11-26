# Round 254 — Версия приложения поднята до 2.0.2 + логирование connect() в nats.c

## Задача
Поднять версию приложения с `2.0.1` на `2.0.2` и добавить диагностику
кода возврата `connect()` в lib nats.c (чтобы понять, почему подключение
к NATS не устанавливается).

## Реализация
- `src/generate_version.sh`: `VERSION "2.0.1-$SHA"` → `VERSION "2.0.2-$SHA"`.
- `src/l2-proxy-version.h` — build-generated (не трекается), пересоздаётся
  `generate_version.sh` при сборке, поэтому отдельно не менялся.
- `src/nats/src/comsock.c`: после `res = connect(...)` добавлен вывод в stderr
  `[nats] connect result: <res> errno: <errno> (<strerror>)` — логируется код
  возврата и текст ошибки для диагностики неудачного подключения.

## Проверка
- `./rebuild-and-run.sh` — сборка зелёная, все сервисы healthy.
- Метрика `l2_common_build_info{mode="proxy",version="2.0.2-..."}` —
  версия `2.0.2` попала в бинарник.
- `message_counter.py --iterations 1 --concurrent 1` — exit 0.

## Тэг
- `git tag -f 2.0.2 HEAD && git push origin 2.0.2` — тэг для CI-сборки.
