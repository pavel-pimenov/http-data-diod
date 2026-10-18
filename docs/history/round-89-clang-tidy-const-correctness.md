# Round 89: clang-tidy — misc-const-correctness sweep

## Мотивация

Продолжение hardening'а clang-tidy (round 88). Включили `misc-const-correctness`
и добавили `const` там, где переменная/лок не мутируется.

## Конфиг

* В `src/.clang-tidy` добавлен `misc-const-correctness`.
* Добавлена опция `misc-const-correctness.AnalyzePointers: false`: анализ
  «pointee можно объявить const» давал ложные срабатывания на C-API
  (`PGconn *conn` возвращается как `PGconn*`; `dpiData *data` передаётся
  как out-параметр `dpiData**`) — такие указатели нельзя сделать
  `const <T>*` без ломающих кастов. Проверка значения/ссылок оставлена.

## Правки (37 `const` в 12 файлах)

* **`logger.hpp` (4):** `level_str` (×2), `msg_str`, `use_json_format`.
* **`db_query_handler.cpp` (8):** 7 `std::scoped_lock lock(m_state.m_mutex)`
  + `json exec_body`.
* **`db_query_executor_postgres.cpp` (7):** все `std::scoped_lock lock(m_mutex)`.
* **`duplicate_detector.cpp` (4):** все `std::scoped_lock lock(m_state.m_mutex)`.
* **`http_client_pool.cpp` (3):** `std::scoped_lock lock(m_state.m_pool_mutex)`.
* **`http_client.cpp` (3):** `PreparedRequest req` (`execute_request` берёт
  `const PreparedRequest &`).
* **`db_query_handler.hpp` (2):** `std::scoped_lock` в `is_enabled`/`all_configured`.
* **`scoped_profiler.hpp` (2):** `std::chrono::duration<double> duration`.
* **`db_query_utils.hpp` (1):** `std::string word` (`next_word_lower`).
* **`db_query_executor_oracle.cpp` (1):** `StmtGuard stmt_guard{stmt_raw}`.
* **`dynamic_labeled_family.hpp` (1):** `std::lock_guard lock` в `get()`.
* **`header_utils.hpp` (1):** `std::span<const BrowserPattern> pat_view`.

`const std::scoped_lock`/`std::lock_guard` — стандартный идиом RAII-гарда:
объект не переприсваивается, деструктор разблокирует.

## Проверка

* `./scripts/run-clang-tidy.sh --all`: `✓ clang-tidy: no errors or warnings
  in project files` (два подряд чистых прогона, без ошибок конфига).
* `./rebuild-and-run.sh`: rc=0, `All tests passed (883 assertions in 120 test
  cases)` + `(2585 assertions in 556 test cases)`, `All health checks passed!`,
  `Done! Services are running`.
* `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0,
  `Success: No message loss or crossed responses detected`.
* `scripts/run-coverage.sh`: rc=0, production 6379/7120 = **89.59%** —
  регрессий нет, baseline не менялся (добавление `const` не меняет набор строк).
