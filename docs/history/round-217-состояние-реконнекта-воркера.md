# round 217 — состояние реконнекта воркера вынесено в тестируемый хелпер

## Что было не так

`L2Worker::run_with_nats()` держал всё состояние цикла reconnect в локальных
`bool`-флагах (`was_connected`, `subscription_active`, `db_subscription_active`)
и инкременте счётчика `++m_state.m_db_init_retry_count` прямо в условии. Решения
«сбросить подписки после восстановления связи», «записать failure/success в
backoff» и «пора ли догнать опоздавшую БД» были неотделимы от живых вызовов
`NatsClient::is_connected()`/`connect()` и публикации метрик, поэтому почти не
покрывались тестами: `l2_worker_nats.cpp` — 326/367 строк (88.8%) и 431
непокрытая ветка, самый «тёмный» first-party файл, не попадающий под исключения
правил (Oracle, `trace_logger`, `sentry_client`, `stats_page`).

## Что сделано

- Новый `src/worker_reconnect_state.hpp`: `WorkerReconnectState` хранит
  `m_was_connected`/`m_subscription_active`/`m_db_subscription_active` и
  методы принятия решений: `observe_connection()` возвращает «был край восстановления
  связи» и сбрасывает оба флага подписки, `set_subscription_active()`,
  `set_db_subscription_active()`, `clear_on_disconnect()`,
  `backoff_failed()` (failure, если нет клиента/связи/активной подписки);
  отдельная `db_init_retry_due()` инкрементирует счётчик только когда хендлер
  есть и не все БД готовы, и возвращает true раз в `every_passes` проходов.
- `run_with_nats()` использует хелпер вместо локальных флагов и инкремента;
  логирование, `unsubscribe()`, `connect()` и вызовы подписок остались на
  прежних местах и в прежнем порядке, то есть поведение не изменилось.
- Тесты в `test_l2_worker.cpp`: край потери/восстановления связи и сброс
  флагов, `clear_on_disconnect()` на подключённом клиенте, все комбинации
  `backoff_failed()`, каденция DB-retry (счётчик не двигается без хендлера и
  при `all_configured`, срабатывает на 50-м проходе и на другом интервале).

## Проверка

- `./rebuild-and-run.sh` — rc=0, сборка и запуск стека зелёные, `test_components`
  639 кейсов / 3214 assertions (+3 / +82), `test_proxy_core` 132 / 996.
- `./scripts/run-coverage.sh` + `coverage-regression-check.py` — гейты пройдены;
  production-файлы **7203/7830 = 91.99%** (94 файла), новый
  `worker_reconnect_state.hpp` 22/22 строк и 22/22 ветвей,
  `l2_worker_nats.cpp` 326/366 = 89.1%; baseline обновлён, повторный чек — `OK`.
- `./health-check.sh all`, `python3 message_counter.py --iterations 1 --concurrent 1`,
  `./scripts/ci-gate.sh all` — rc=0.
- `./scripts/run-cppcheck.sh` — rc=0, замечаний в проектных файлах нет;
  `./scripts/run-clang-tidy.sh` — rc=0, только унаследованные предупреждения
  `readability-use-std-min-max` в `db_query_utils.hpp:51` (файл раунда не менялся).
