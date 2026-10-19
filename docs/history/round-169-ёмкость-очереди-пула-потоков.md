# round 169 — ёмкость очереди пула потоков: `l2_worker_queue_capacity` (P1 #9 из TODO)

## Что было не так

`l2_worker_queue_size` (глубина очереди пула потоков воркера) показывает, *сколько*
задач стоит в очереди, но не даёт понять, *много* ли это: без знаменателя очередь
глубиной 50 — это норма (ёмкость 512) или тревога (ёмкость 64). Аналогичный
разрыв уже закрывался для пула PostgreSQL в раунде 157 (`l2_worker_db_pool_capacity`).

## Что сделано

- В `ThreadPool` появились два аксессора:

  ```cpp
  // Effective capacity of the task queue: the explicit limit, or the
  // automatic bound (threads * g_max_queue_per_thread) when the limit is 0.
  [[nodiscard]] static size_t effective_queue_capacity(size_t threads,
                                                       size_t max_queue_size);
  [[nodiscard]] size_t max_queue_capacity() const;
  ```

  Реальная ёмкость пула считается в конструкторе (`max_queue_size == 0 ? threads *
  g_max_queue_per_thread : max_queue_size`) — теперь эта же формула доступна
  наружу (`effective_queue_capacity`) и используется конструктором, то есть
  источник истины один ✓.

- `ThreadPoolWrapper` прокинул аксессор (`max_queue_capacity()`); для inline-режима
  (`Type::NONE`, без очереди) он возвращает 0 ✓.

- Новая метрика `l2_worker_queue_capacity` (gauge, реестр воркера): ёмкость
  очереди пула потоков. Ставится один раз при инициализации пула — значение не
  меняется в рантайме ✓.

  Предел либо задаётся явно (`L2_WORKER_QUEUE_SIZE`), либо выводится автоматически
  (8 задач на поток × `L2_WORKER_THREADS`). Воркер с `THREAD_POOL_TYPE=none`
  принудительно переводится на обычный пул (это сделано ещё в исходном коде:
  inline-режим небезопасен для NATS-воркера), поэтому ёмкость всегда определена
  и «0» в метрике означает «ёмкости нет» только в невозможном для воркера режиме.

- Дашборд «Очередь воркера (глубина)» получил вторую линию — ёмкость, так что
  насыщение видно прямо на графике (глубина упирается в ёмкость ✓).

- Каталог README (строка + пояснение в разделе про бэкпрешен), `CATALOG`
  golden-check — итого **93** метрики в четырёх источниках.

## Почему только воркер, а не прокси

У прокси тоже есть очередь задач (`TimedTaskQueue`, метрики
`l2_proxy_task_queue_enqueued_total`/`_rejected_total`/`_wait_seconds`), но у неё
нет **глубины** в метриках — `ThreadPoolWrapper::queue_size()` никто не
экспортирует. Одна ёмкость без глубины бесполезна (знаменатель без числителя),
поэтому прокси-часть пункта отложена: сначала нужна метрика глубины очереди
прокси (её надо периодически опрашивать, как `l2_worker_queue_size` в воркере),
и только потом — ёмкость. Это отмечено в `docs/TODO-metrika-kandidaty.md`.

## Тесты

- `ThreadPool: automatic queue capacity is threads times the bound` — проверяет
  оба режима (явный предел и автоматический), а также `effective_queue_capacity`
  и инстансный `max_queue_capacity`.
- Существующий тест `ThreadPool: accessors report queue and thread counts`
  дополнен проверкой `max_queue_capacity() == 4`.
- Тест воркера (`L2Worker: outbound call routing …`) проверяет, что после
  создания пула `l2_worker_queue_capacity` больше нуля — то есть gauge действительно
  публикуется, а не остаётся незаполненным.

## Проверка

- Сборка: `All tests passed (2870 assertions in 598 test cases)` (на один кейс
  больше — новый тест на ёмкость) и `945 assertions in 125 test cases`.
- `./health-check.sh all` — все проверки пройдены; smoke — все GET-запросы успешны.
- `./scripts/ci-gate.sh runtime` с профилем `databases` (rc=0): DB Gateway e2e 7/7,
  golden `89/89 families … WORKER=30` (было 29 — новое семейство скрейпится),
  consistency `registered=93 dashboards=93 perses=93 readme=93 golden=93`, parity
  Grafana↔Perses — `OK`.
- `./scripts/run-cppcheck.sh` — rc=0; `./scripts/run-clang-tidy.sh --all` — 47
  файлов, «no errors or warnings in project files» (первый прогон нашёл в новом
  тесте `misc-const-correctness` — локальные переменные пула сделаны `const`).
- Гейт покрытия проверяет CI на этом коммите.
