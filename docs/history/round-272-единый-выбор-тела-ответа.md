# Раунд 272. Единый выбор тела ответа в `response_builder.cpp`

Продолжение focused-рефакторинга после round 271: в `set_response_content()`
три независимых выбора `is_binary ? decoded_binary_response : l2_response`
заменены одной локальной `const std::string &response_body`. Поведение HTTP
ответа, заголовков, логов и метрик не меняется.

## Что сделано

- После base64-декодирования введён `const std::string &response_body`:
  ссылка живёт до конца функции и указывает либо на локальный
  `decoded_binary_response`, либо на `l2_response` (ссылку в
  `parsed_response_data`, которая переживает вызов). Копирования строк нет.
- Через эту ссылку теперь пишутся размер в
  `m_response_size_bytes.Observe()`, размер в debug-логе бинарного ответа и
  `res.set_content()` в обеих ветках. Условие `is_binary`, `content_type`,
  комментарии о сжатии/Content-Type и `set_traceparent_response_header()`
  остались на месте.

## Тесты

- `test_components.cpp`, бинарный envelope: добавлена проверка, что гистограмма
  `m_response_size_bytes` получила размер **декодированного** тела (5 байт),
  а не размер base64-строки (8 байт).
- `test_components.cpp`, текстовый envelope: добавлена проверка, что та же
  гистограмма получила размер обычного тела без копирования.

## Проверки

- `./scripts/run-coverage.sh` — 656 кейсов/3324 assertions (`test_components`)
  и 135/999 (`test_proxy_core`); production 7691/8722 строк = 88.18% (97
  файлов), пер-файловых регрессий нет; `docs/coverage-baseline.json` обновлён.
- `python3 scripts/coverage-regression-check.py` — регрессий покрытия нет.
- `./rebuild-and-run.sh`, `./health-check.sh all`,
  `python3 message_counter.py --iterations 1 --concurrent 1` — зелёные.
- `./scripts/ci-gate.sh offline` / `runtime` — зелёные.
- `./scripts/run-cppcheck.sh` — без замечаний; clang-tidy — только известное
  предупреждение `db_query_utils.hpp` из включаемого заголовка.

Метрики, PromQL, README-каталог, golden-check, env и NATS-контракт не
изменялись: единственная метрика уже существовала и лишь перестала дублировать
тернарный оператор в трёх местах.
