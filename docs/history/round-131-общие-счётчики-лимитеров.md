# round 131 — общие счётчики исходов запроса у rate limiter'ов

## Что сделано

`RateLimiter` (глобальный) и `PerIPRateLimiter` держали одну и ту же тройку
счётчиков исходов запроса и один и тот же идиом инкремента
(`fetch_add(1, std::memory_order_relaxed)`) в каждом месте:

- `RateLimiter::Stats` (private) — `m_total_requests` / `m_allowed_requests` /
  `m_rejected_requests` + 3 места `fetch_add` в `acquire()`;
- `PerIPRateLimiter::Counters` (private) — те же три поля (плюс свои
  `m_unique_ips`/`m_evictions`) + 4 места `fetch_add` в `acquire()`.

Тройка полей и идиом вынесены в общую структуру в `rate_limiter.hpp`:

```cpp
// Relaxed order is deliberate: these counters feed Prometheus gauges only and
// synchronise nothing, so the increment idiom lives here.
struct RateLimiterCounters {
  std::atomic<uint64_t> m_total_requests{0};
  std::atomic<uint64_t> m_allowed_requests{0};
  std::atomic<uint64_t> m_rejected_requests{0};

  void count_request()  { m_total_requests.fetch_add(1, std::memory_order_relaxed); }
  void count_allowed()  { m_allowed_requests.fetch_add(1, std::memory_order_relaxed); }
  void count_rejected() { m_rejected_requests.fetch_add(1, std::memory_order_relaxed); }
};
```

`RateLimiter` избавился от private-`Stats`: поле теперь `RateLimiterCounters
m_stats`, а три места `fetch_add` стали `m_stats.count_request()` /
`count_allowed()` / `count_rejected()`. `PerIPRateLimiter::Counters` теперь
наследует общую структуру и оставляет только своё:

```cpp
struct Counters : RateLimiterCounters {
  std::atomic<uint64_t> m_unique_ips{0};
  std::atomic<uint64_t> m_evictions{0};
};
```

Публичный `PerIPRateLimiter::get_stats()` и его снапшот-`Stats` не менялись —
доступ к унаследованным полям (`m_counters.m_allowed_requests.load()` и т.д.)
сохранён. Порядок памяти (`relaxed`) и семантика не менялись.

## Попутно (предсуществующие предупреждения clang-tidy)

`rate_limiter_per_ip.hpp` впервые попал в набор изменённых для clang-tidy —
всплыли include-gaps: добавлены прямые `<algorithm>` (`std::min`),
`<iterator>` (`std::prev`), `<stop_token>` (`std::stop_token`,
`std::jthread`-колбэк), `<utility>` (`std::move`/`std::pair`), `<vector>`;
в `rate_limiter.hpp` — `<cstdint>` (`uint64_t`).

## Проверка

- `./rebuild-and-run.sh` — rc=0, **All tests passed (2816 assertions in 589
  test cases)**.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck — чисто.
- clang-tidy — чисто.
- coverage (`./scripts/run-coverage.sh`) — rc=0, `OK: регрессий покрытия нет`.
  `rate_limiter.hpp`: 43/44 → 52/53 (методы `count_*` полностью покрыты обоими
  наборами тестов лимитеров), `rate_limiter_per_ip.hpp`: 141/147 (набор
  непокрытых строк не изменился). Общий production 6528/7142 = 91.40%
  (было 6519/7133), baseline переанкор не потребовался.
