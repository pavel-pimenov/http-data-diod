# round 170 — umbrella-счётчик отказов прокси: `l2_proxy_rejections_total{reason}` (P1 #6 из TODO)

## Что было не так

Причины, по которым прокси отказывает, были разбросаны по разным семействам
метрик:

| причина отказа | где сейчас считается |
|---|---|
| глобальный rate-limit | `l2_rate_limiter_rejected_total` |
| rate-limit на IP | `l2_per_ip_rate_limiter_rejected_total` |
| дубликат POST-тела (409) | `l2_proxy_duplicate_posts_detected_total` (считает *обнаружения*, а не отказы!) |
| невалидный JSON (400) | `l2_proxy_client_request_errors_total{type="CLIENT_ERROR"}` (смешано с 5xx) |

Собрать «почему мы отказываем» в один график было нельзя, а счётчик дубликатов
считал обнаружения, а не отказы (при `DUPLICATE_REJECT_ENABLED=false` дубликат
проходит дальше, но метрика растёт ✓ — то есть она вообще не про отказы).

## Что сделано

- Новый закрытый набор причин в `src/error_categorizer.hpp` (рядом с остальными
  классификаторами проекта):

  ```cpp
  enum class RejectionReason : std::uint8_t {
    RATE_LIMIT_GLOBAL, RATE_LIMIT_PER_IP, DUPLICATE, INVALID_JSON,
  };
  inline constexpr std::array<const char *, 4> g_rejection_reason_names = {
      "rate_limit_global", "rate_limit_per_ip", "duplicate", "invalid_json"};
  ```

  Список причин — единственный источник истины: `AppContext` по нему
  предсоздаёт серию для каждой причины (prometheus-cpp отдаёт лейбл-семейство
  только после первого `Add()`, а здоровый прокси ничего не отказывает ✓).

- Новое семейство `l2_proxy_rejections_total{reason}` в реестре прокси.

- Инкременты — в тех же четырёх местах, где прокси и так отказывает:

  | причина | место |
  |---|---|
  | `invalid_json` | `handle_post()` — тело не разобралось как JSON (400) |
  | `rate_limit_global` | `check_rate_limits()` — исчерпан глобальный лимит (429) |
  | `rate_limit_per_ip` | `check_rate_limits()` — исчерпан лимит на IP (429) |
  | `duplicate` | `record_and_maybe_reject_duplicate()` — отказ 409 |

  Инкремент живёт в одном методе `RequestHandler::count_rejection()` ✓ — метки
  берутся из enum, а не из строковых литералов в местах вызова ✓.

- Дашборд L2 Прокси: в панель rate-limiter'а добавлена линия
  `sum by (reason) (rate(l2_proxy_rejections_total[1m]))` — теперь все отказы
  видны рядом с частными счётчиками ✓.

- Каталог README (строка + абзац с расшифровкой причин и объяснением, зачем
  umbrella рядом с частными счётчиками), `CATALOG` golden-check — итого **94**
  метрики в четырёх источниках.

## Чего метрика не делает

Она не заменяет частные счётчики: те считают *события* (обнаруженных дубликатов,
отклонённых по лимиту), umbrella — *отказы*. Расходиться они могут там, где
событие не приводит к отказу (например, дубликат при `DUPLICATE_REJECT_ENABLED=false`
считается в `duplicate_posts_detected_total`, но в `rejections_total` не попадает ✓ —
это осознанно: umbrella — про отказы).

## Тесты

- `AppContext: proxy pre-creates the rejection reason series` — все четыре причины
  есть в реестре прокси как нули ✓.
- Тест «global rate limiter rejects with 429» проверяет, что отказ посчитан с
  причиной `rate_limit_global`, а `rate_limit_per_ip` остался нулём ✓.
- Тест «POST without a backend answers 504» — невалидный JSON даёт
  `invalid_json == 1` ✓.
- Тест «duplicate POST is rejected with 409» — `duplicate == 1`, а `invalid_json`
  остался нулём ✓ (причины не смешиваются).

## Проверка

- Сборка: `All tests passed (2877 assertions in 599 test cases)` (на один кейс
  больше — тест предсоздания) и `945 assertions in 125 test cases`.
- `./health-check.sh all` — все проверки пройдены; smoke — все GET-запросы успешны.
- `./scripts/ci-gate.sh runtime` с профилем `databases` (rc=0): DB Gateway e2e 7/7,
  golden `90/90 families … PROXY=30, WORKER=30` (было 29/29 — оба новых семейства
  из раундов 169 и 170 скрейпятся), consistency `registered=94 dashboards=94
  perses=94 readme=94 golden=94`, parity Grafana↔Perses — `OK`.
- `./scripts/run-cppcheck.sh` — rc=0; `./scripts/run-clang-tidy.sh --all` — 47
  файлов, «no errors or warnings in project files» (первый прогон указал на
  забытый `error_categorizer.hpp` в `request_handler.cpp` — добавлен).
- Гейт покрытия проверяет CI на этом коммите.
