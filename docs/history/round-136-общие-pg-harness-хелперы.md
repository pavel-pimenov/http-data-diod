# round 136 — общие pg-harness хелперы в `test_pg_diag.hpp`

## Что сделано

Продолжение round 135 (общие хелперы live-тестов): второй пласт дублирования —
каркас запуска эфемерного postgres. `test_db_live.cpp` и компактный
`namespace pgtest` из `test_nats_live.cpp` держали по своей копии одних и тех же
четырёх сущностей (различались именами и стилем):

- выбор unix-пользователя, под которым должен идти форк сервера —
  `TargetUser`/`resolve_target_user()` ↔ `PgTarget`/`pg_target()`;
- сброс привилегий перед `exec` — `drop_privileges()` ↔ `pg_droppriv()`;
- перенаправление stdout/stderr сервера в лог — `redirect_to()` ↔ `pg_redir()`;
- проверка готовности через libpq — `sql_probe()` ↔ `pg_sql_probe()`.

Заголовок `test_pg_diag.hpp` уже заведён именно под это («Shared diagnostics for
the two ephemeral-postgres test harnesses (test_db_live.cpp and the pgtest block
of test_nats_live.cpp)») — туда и переехали все четыре, в `namespace
pg_test_diag`:

```cpp
struct ServerUser {
  bool m_ok = false;
  bool m_need_switch = false;
  uid_t m_uid = 0;
  gid_t m_gid = 0;
  std::string m_name;
};
[[nodiscard]] inline ServerUser resolve_server_user();
[[nodiscard]] inline bool drop_privileges(const ServerUser &user);
[[nodiscard]] inline bool redirect_to(const char *path);
[[nodiscard]] inline bool postgres_sql_probe(int port);
```

Взяты варианты из `test_db_live.cpp` (с `Logger::warn` при отсутствии
пользователя `postgres` и нормализацией пробелов); `pgtest` уже сам вызывает
`pg_test_diag::fail(...)` в этом случае, так что диагностика сохранилась.
Комментарий-обоснование libpq-пробы («A bare TCP accept only proves…») переехал в
заголовок к `postgres_sql_probe`, а его локальная копия в `pgtest` убрана как
дублирующая.

В TU определения удалены, вместо них точечные
`using pg_test_diag::…;`, а места вызова переименованы:
`TargetUser`/`PgTarget` → `ServerUser`, `resolve_target_user`/`pg_target` →
`resolve_server_user`, `pg_droppriv` → `drop_privileges`, `pg_redir` →
`redirect_to`, `sql_probe`/`pg_sql_probe` → `postgres_sql_probe`,
`t.m_need` → `t.m_need_switch`. Попутно из `test_db_live.cpp` убраны ставшие
ненужными `<fcntl.h>`, `<grp.h>`, `<functional>`, `<pwd.h>`, а из блока
`pgtest` в `test_nats_live.cpp` — `<pwd.h>` и `<grp.h>`; прямые `<fcntl.h>`,
`<grp.h>`, `<libpq-fe.h>`, `<pwd.h>`, `<sys/types.h>`, `<unistd.h>` добавлены в
`test_pg_diag.hpp`.

Диффстат: −49 строк (−146/+97).

## Проверка

- `./rebuild-and-run.sh` — rc=0, все юнит-тесты прошли.
- `./health-check.sh all` — rc=0.
- `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0.
- cppcheck — чисто.
- clang-tidy — чисто (первый прогон поймал пропущенный `t.m_need` —
  переименование поля — компиляционной ошибкой `clang-diagnostic-error`;
  исправлено до сборки).
- coverage (`./scripts/run-coverage.sh`) — rc=0, `OK: регрессий покрытия нет`;
  изменения только в `test_`-файлах, production 6523/7137 = 91.40% без
  изменений, baseline не трогался.
