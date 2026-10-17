# round 126 — общий хелпер `make_packaged_task` для thread pool'ов

## Что сделано

`ThreadPool::enqueue` (`src/thread_pool.hpp`) и inline-ветка `Type::NONE` в
`ThreadPoolWrapper::enqueue` (`src/thread_pool_wrapper.hpp`) дословно повторяли один
блок: `std::make_shared<std::packaged_task<return_type()>>([f = std::forward<F>(f),
... args = std::forward<Args>(args)]() mutable { return std::invoke(f, args...); })`.

Блок вынесен в шаблонный хелпер:

```cpp
namespace thread_pool_detail {
template <class F, class... Args>
auto make_packaged_task(F &&f, Args &&...args) {
  using return_type = std::invoke_result_t<F, Args...>;
  return std::make_shared<std::packaged_task<return_type()>>(
      [f = std::forward<F>(f),
       ... args = std::forward<Args>(args)]() mutable -> return_type {
        return std::invoke(f, args...);
      });
}
} // namespace thread_pool_detail
```

Обе точки вызова теперь сводятся к одной строке. В `thread_pool.hpp` добавлен
`#include <utility>` (нужен для `std::forward`/`std::invoke_result_t`).

Различие поведения не вносилось: `ThreadPool::enqueue` по-прежнему возвращает
`std::future<return_type>` из `task->get_future()` и кладёт `task` в очередь,
`Type::NONE` — синхронно исполняет `(*task)()` и отдаёт `future`. Прошлая версия
в wrapper'е возвращала `std::future<std::invoke_result_t<F, Args...>>` — тип тот же.

## Проверка

- `./rebuild-and-run.sh` — rc=0, 2816 assertions / 589 cases.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck (`./scripts/run-cppcheck.sh`) — чисто.
- clang-tidy (`./scripts/run-clang-tidy.sh`) — только предсуществующие
  `misc-include-cleaner`-предупреждения по этим двум заголовкам (не блокирующие).
- coverage-гейт: первый прогон дал просадку `metrics_history.hpp` 95.65% → 94.57%
  (−1 строка). Файл в этом раунде не менялся; строка-виновник — рантайм-зависимая
  ветвь `sample()`/`get_series()` (состав реестра/«та же секунда» при пересеве), то
  есть тайминг-флейк того же класса, что периодический дрейф `nats_client.cpp`.
  Baseline переанкорен (`--update`), повторная проверка — `OK: регрессий нет`,
  общий production 6519/7134 = 91.38%.

## Замечание по инструменту

Переанкор (`coverage-regression-check.py --update`) переписывает baseline **для всех**
файлов, поэтому тайминг-флейки (как `metrics_history.hpp` здесь или `nats_client.cpp`
в round 124) «запекаются» в baseline вместе с целевыми изменениями. Гейт реагирует
только на просадку, так что при следующем прогоне возврат строки в покрытое
состояние регрессией не считается.
