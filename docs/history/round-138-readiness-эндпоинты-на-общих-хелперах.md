# round 138 — общий код readiness-эндпоинтов NATS-сервисов

## Что сделано

Эндпоинт `/health/ready` есть у worker'а (`src/main.cpp`) и у proxy
(`RequestHandler::handle_readiness`), и оба повторяли один и тот же каркас
собственными руками: пара gauge'ов выставлялась тернарником, а тело ответа
«не готов» собиралось каждый раз своим способом — worker вписывал сырую
строку-литерал, proxy строил `nlohmann::json` и отдавал через
`send_json_response(res, res.status, ...)` (то есть выставлял `status = 503`
дважды). Форма ответа при этом одна и та же.

Вынесено в два общих помощника.

`src/metrics_manager.hpp` (рядом с другими такими же inline-хелперами вроде
`record_db_request_metrics`):

```cpp
// Mirrors NATS connectivity into the two gauges the /health/ready endpoint
// updates: the connection state read by the availability panels and the
// readiness flag itself. Both NATS-backed handlers (worker, proxy) set them
// with the same ternary pair.
inline void set_nats_readiness_gauges(prometheus::Gauge &nats_connected,
                                      prometheus::Gauge &health_ready,
                                      bool ready) {
  const double value = ready ? 1.0 : 0.0;
  nats_connected.Set(value);
  health_ready.Set(value);
}
```

`src/json_response_utils.hpp` (рядом с `set_health_ready_nats`, форма тела та же,
только `"messaging"` меняется на `"error"`):

```cpp
// Readiness failure body for the NATS-backed services: 503 plus the same shape
// as set_health_ready_nats with "error" instead of "messaging". The worker used
// a raw string literal and the proxy an nlohmann object for the very same body.
inline void set_health_not_ready_nats(httplib::Response &res,
                                      std::string_view service,
                                      std::string_view error) {
  res.status = 503;
  res.set_content(
      std::format(
          R"({{"status": "not_ready", "service": "{}", "error": "{}"}})",
          service, error),
      "application/json");
}
```

Тела ответов у worker'а остались байт-в-байт прежними (списанный литерал имел
ровно такой же вид), у proxy — тот же JSON в компактном виде вместо `dump()`
(тесты разбирают тело через `json::parse` и проверяют поля `status`/`error`,
побайтовых проверок нет). `main.cpp` из-за нового хелпера получил
`#include "metrics_manager.hpp"`.

## Проверка

- `./rebuild-and-run.sh` — rc=0, все юнит-тесты прошли.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck — чисто.
- clang-tidy — чисто.
- coverage (`./scripts/run-coverage.sh`) — production 6531/7142 = 91.44%
  (+8 покрытых строк: оба хелпера полностью покрыты, включая новое тело
  `set_health_not_ready_nats` — его гоняет тест readiness-503 в
  `test_proxy_handlers.cpp`). Гейт по строкам сначала пометил
  `request_handler.cpp: 95.29% -> 95.23%`: удалено 6 **покрытых** строк
  (`446/425 -> 440/419`, число непокрытых не изменилось — те же 21), то есть
  это тот же случай «удаление покрытых строк без добавления непокрытых», что
  и в раундах 132/134. Baseline переанкорен
  (`coverage-regression-check.py --update`) — туда же попал известный
  флейк `metrics_history.hpp` (87 -> 90); повторный прогон покрытия после
  переанкора зелёный.
