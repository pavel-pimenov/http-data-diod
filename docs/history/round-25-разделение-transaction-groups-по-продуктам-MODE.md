# round 25: разделение transaction groups по продуктам (MODE)

## Date: 2026-09-17

### Что сделано
- Раньше `m_sentry_service` (режим `MODE`: proxy/worker/l2-server) хранился,
  но в транзакции не попадал: transaction groups во всех продуктах
  сливались в одни группы (ключ группы = transaction+op+method, единый
  проект GlitchTip).
- `build_sentry_transaction_json`/10-аргументный
  `build_sentry_transaction_envelope` получили параметр `product` (`""` =
  без изменений): имя транзакции получает префикс режима
  (`worker: HTTP NATS_consume /nats`, `proxy: HTTP INCOMING /`,
  `l2-server: GET /`), `tags.mode` = режим (комбинация даёт чистые группы
  по продуктам; `service`-тег/контексты остаются пер-спановыми).
- `deliver_sentry_transactions` прокидывает `m_sentry_service` как product.
- Тесты: `build_sentry_transaction_json` с product (префикс + `tags.mode`) и
  без (без изменений); E2E-доставка теперь ждёт `test-service: HTTP POST
  /v1/report` и `"mode":"test-service"` в реальном envelope.
- `scripts/glitchtip-performance-e2e.py`: ожидания переведены на префиксы
  (`proxy: HTTP INCOMING /`, `proxy: HTTP POST /`,
  `worker: HTTP NATS_consume/push/poll /nats`).
- Проверено на живом стенде: groups разделены по продуктам, метрики
  sent/failed по-прежнему 0 failed.

