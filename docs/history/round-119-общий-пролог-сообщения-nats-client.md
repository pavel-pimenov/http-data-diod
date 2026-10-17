# Round 119: устранение дублей пролога сообщения в `NatsClient`

## Что сделано

`request_impl()` и `publish_with_headers()` в `nats_client.cpp` открывались
одним и тем же блоком (≈15 строк):

* `ensure_connected()` → `acquire_connection(operation)`;
* `natsMsg_Create(&msg, subject, nullptr, data, size)`;
* обработка ошибки создания с соответствующим текстом;
* `set_msg_headers(msg, headers, operation)`.

Различались только имена операции в логах потери связи (`"request"` /
`"publish_with_headers"`) и формулировки ошибок (`NATS request message` /
`NATS message`).

## Изменения

* **`nats_client.hpp`** — приватный агрегат `PreparedMessage`
  (`m_conn` + `m_msg`) и метод
  `prepare_message_with_headers(subject, data, headers, operation, message_kind)`
  (возвращает `std::optional<PreparedMessage>`).
* **`nats_client.cpp`** — реализация хелпера (общий пролог) и перевод на него:
  * `request_impl` → `prepare_message_with_headers(..., "request", "request message")`;
  * `publish_with_headers` → `prepare_message_with_headers(..., "publish_with_headers", "message")`.

Формулировки ошибок воспроизводятся параметром `message_kind`
(`"Failed to create NATS " + kind + " for subject '…'"`, `"NATS " + kind`),
поэтому поведение и тексты сохранены байт-в-байт. `publish()` (без заголовков)
не тронут — он использует `natsConnection_PublishString` без создания
сообщения и общего блока с двумя другими путями не имеет.

## Проверка

* `./rebuild-and-run.sh`: rc=0, юнит-тесты `All tests passed (2771 assertions in 582 test cases)`.
* `./health-check.sh all`: rc=0.
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0.

Существующие live-тесты `[nats-live]` покрывают обе ветви-потребители (request
и publish-with-headers), а также error-пути пустого subject/header (round 108),
поэтому покрытие не должно просесть.
