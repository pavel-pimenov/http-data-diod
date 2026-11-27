# Раунд 275. Один `set_content()` для бинарного и текстового ответа

Продолжение focused-рефакторинга после round 274. В `response_builder.cpp`
две ветви (`is_binary` / `else`) вызывали один и тот же
`res.set_content(response_body, content_type)`; различались только комментарии
и debug-лог, который уже был вынесен выше. Ветвление убрано: остался один
вызов с тем же телом и тем же Content-Type, а условный debug-лог сохранён.

## Что сделано

- `src/response_builder.cpp`: схлопнуты дублирующие ветки `set_content()`;
  комментарий про сохранение upstream Content-Type перенесён к единственному
  вызову, binary-лог по-прежнему пишется только для `is_binary=true`.
- Поведение не меняется: бинарный путь использует декодированные байты,
  текстовый — исходное тело, `Content-Type` берётся из конверта с дефолтом
  `application/json`; метрики, логи и NATS-контракт без изменений.

## Тесты

- Существующие кейсы `test_components.cpp` и `test_proxy_handlers.cpp`
  проверяют бинарный base64-конверт, текстовый HTML-конверт и размер гистограммы
  `m_response_size_bytes`; после правки они остаются точной проверкой
  эквивалентности.

## Проверки

- `./scripts/run-coverage.sh` — 659 кейсов/3331 assertion и 135/999,
  production 97 файлов, 7695/8727 = 88.17%, регрессий нет; ожидаемый сдвиг
  `response_builder.cpp` 93.94% → 93.75% вызван удалением дублирующих строк,
  непокрытых строк не прибавилось.
- `./rebuild-and-run.sh`, `./health-check.sh all`,
  `python3 message_counter.py --iterations 1 --concurrent 1` — зелёные.
- `./scripts/ci-gate.sh offline` / `runtime` — зелёные; локальное предупреждение
  `postgres is down — DB Gateway metric checks skipped` ожидаемо.
- `./scripts/run-cppcheck.sh` и `./scripts/run-clang-tidy.sh` — зелёные,
  новых предупреждений нет.
