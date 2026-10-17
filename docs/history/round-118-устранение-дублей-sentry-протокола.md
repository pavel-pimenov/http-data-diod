# Round 118: устранение дублей Sentry-протокола (SentryClient ↔ JaegerLogger)

## Что сделано

Sentry-протокол (URL/auth/шапка/сборка envelope, строковая упаковка тегов) был
реализован дважды: в `sentry_client.cpp` (отправка ошибок, `SentryClient`) и в
`JaegerLogger` (`trace_logger.cpp`, отправка performance-транзакций). Дословно
совпадали:

* URL envelope — `path_prefix + "/api/" + project_id + "/envelope/"`;
* `X-Sentry-Auth` — `"Sentry sentry_version=7, sentry_key=" + key(+"/"+secret)`;
* шапка envelope `{event_id, sent_at, sdk{name,version}}`;
* сборка `header.dump() + "\n" + item_header.dump() + "\n" + payload`;
* цикл «строковой» упаковки тегов (`is_string() ? value : value.dump()`);
* `contexts.trace`.

## Изменения

* **`sentry_client.hpp/.cpp`** — в `namespace sentry` добавлены общие хелперы
  envelope-протокола (все — чистые, без сети/реестра):
  * `envelope_path(const DsnData&)` — путь эндпоинта;
  * `auth_header(const DsnData&)` — значение `X-Sentry-Auth`;
  * `envelope_header(const std::string &event_id)` — шапка (json);
  * `envelope_item(const json &payload, std::string_view type, bool include_length=false)`
    — строка item-заголовка + payload.
  `build_envelope` (событие) и `SentryClient::send_envelope` переведены на них.
* **`json_utils.hpp`** — новый нейтральный хелпер
  `merge_stringified_tags(json &dest, const json &source)`: копирует пары в
  `dest` со строковой упаковкой (`dump()` для не-строк); не-объект игнорируется.
  Заменил **три** одинаковых цикла: `sentry_client` (`build_event_json`),
  `trace_logger.cpp` (`build_sentry_transaction_json`) и inline
  `JaegerLogger::build_span_json` в `trace_logger.hpp`.
* **`trace_logger.cpp`** — `sentry_envelope_url`, `sentry_auth_header`,
  `build_sentry_transaction_envelope`, `build_sentry_envelope` теперь тонкие
  обёртки над `sentry::*` (публичные имена сохранены — их прямо покрывает
  `test_trace_logger.cpp`).

Поведение сохранено байт-в-байт (формат envelope/порядок ключей, `length` item'а
равен длине `dump()`); публичный API не менялся.

## Проверка

* `./rebuild-and-run.sh`: rc=0, юнит-тесты `All tests passed`.
* `./health-check.sh all`: rc=0.
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0.
* Существующие тесты покрывают новые общие хелперы (envelope-пути — через
  `test_trace_logger.cpp`; сборка события — через `test_sentry_client.cpp`),
  падение покрытия не ожидается.

## Замечание по методике

Дубли искались временным token-based детектором (упрощённый CPD, окна ≥40
токенов, без вендора/тестов/build). Прочие найденные «совпадения» —
ложные (общие `#include`/`using json`, пары `.hpp↔.cpp` объявление/определение)
или нецелесообразные к правке (boilerplate prometheus-билдеров разных типов;
row-conversion `dpi` vs `libpq` — разные C-API, Oracle вне покрытия).
