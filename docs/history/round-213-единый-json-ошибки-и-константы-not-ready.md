# round 213 — единый ответ JSON-ошибки, константы причин not-ready

## Что было не так

**Один и тот же JSON-ответ собирался вручную.** В `request_handler_admin.cpp` два
ответа-отказа (`/crash-test` выключен, дубликаты недоступны в этом режиме) были
записаны сырыми string-литералами `R"({"error": ...})"` рядом с уже существующим
хелпером `set_json_error_response()` (`json_response_utils.hpp`). Сырые литералы
легко разъезжаются по форме с остальными ошибками (пробел после двоеточия, экранирование)
и не получают `request_id`.

**Двойная установка статуса.** В `request_handler_db_gateway.cpp` два места делали
`res.status = ...; send_json_response(res, res.status, ...)`, хотя
`send_json_response()` сам выставляет статус — лишняя запись, которую легко
рассинхронизировать с телом.

**Причины not-ready дублировались строками.** `app_context.hpp` объявляет список
`g_not_ready_reasons` как единственный источник истины (по нему предсоздаются серии
`l2_proxy_not_ready_total{reason}`), но `handle_health_ready()` и воркер в `main.cpp`
писали те же три литерала (`nats_unavailable`, `ping_failed`, `health_check_failed`)
руками. Переименование в одном месте молча рассинхронизировало бы метку с телом ответа.

**Непокрытый 0%-файл.** `DuplicateDetector::eviction_reason_name()` вызывается только
из проводки прокси (`proxy_init.cpp`), поэтому юнит-тесты не исполняли его вовсе —
`duplicate_detector.hpp` в отчёте показывал 0/5 строк.

## Что сделано

- Оба ручных ответа в `request_handler_admin.cpp` переведены на
  `set_json_error_response()`; тело теперь собирается nlohmann (форма `{"error":...}`
  без пробела после двоеточия — для клиента эквивалентно, тесты форму не проверяют).
- `request_handler_db_gateway.cpp`: убраны двойные `res.status = ...` перед
  `send_json_response()` (2 места).
- `app_context.hpp`: рядом со списком добавлены именованные константы
  `kNotReadyNatsUnavailable` / `kNotReadyPingFailed` / `kNotReadyHealthCheckFailed`;
  `g_not_ready_reasons` собирается из них. `request_handler_admin.cpp` использует
  эти же константы, `main.cpp` (воркер) — `kNotReadyNatsUnavailable`. Теперь метка
  причины и значение в теле не могут разойтись.
- Тест `DuplicateDetector: eviction reason names match the metric labels` — все четыре
  причины плюс выход за границы enum (`unknown`), чтобы таблица имён не читалась за
  пределами массива.

Поведение не изменилось: те же статусы, те же тела-ошибки, те же значения меток.

## Проверка

- `./rebuild-and-run.sh` — сборка и запуск стека зелёные, `test_components`
  632 кейса / 3088 assertions, `test_proxy_core` 126 / 952.
- `./scripts/run-coverage.sh` — гейты 90% строк / 40% ветвей пройдены;
  production-файлы **7131/7761 = 91.88%**; пер-файловый гейт показывает только
  `SHIFT` по `request_handler_admin.cpp` (перенос кода без роста непокрытых строк),
  регрессий нет; baseline обновлён.
- `./health-check.sh all` и `python3 message_counter.py --iterations 1 --concurrent 1`
  — rc=0.
- `./scripts/ci-gate.sh all` (unit + offline + runtime) — rc=0.
