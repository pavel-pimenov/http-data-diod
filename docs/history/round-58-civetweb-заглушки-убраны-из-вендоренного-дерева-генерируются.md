# round 58: civetweb-заглушки убраны из вендоренного дерева (генерируются CMake), поправка round 56

## Date: 2026-10-05

### Что было не так
- Round 56 записал в `PATCHED`, `src/VENDORED-LIBS.md` и сюда утверждение, что `external_log_access.inl` и `external_mg_cry_internal_impl.inl` «есть только в релизном tarball v1.16». Это неверно: скачанный `v1.16.tar.gz` их тоже не содержит (как и git-тег). Файлы — **наши собственные no-op заглушки**, написанные проектом для сборки metrics-only экспозеров.
- Проверено, что при `NO_FILESYSTEMS` они обязательны: civetweb не имеет своих реализаций и без заглушек падает — `#error Must either enable filesystems or provide a custom mg_cry_internal_impl implementation` плюс `call to undeclared function 'mg_cry_internal_impl'`. Проверено и обратное — с `-DMG_EXTERNAL_FUNCTION_log_access -DMG_EXTERNAL_FUNCTION_mg_cry_internal_impl` (как в `src/CMakeLists.txt`) компиляция чистая.

### Что сделано
- `git rm` заглушек из `src/prometheus-cpp/3rdparty/civetweb/src/` — вендоренное дерево теперь ровно апстрим (слепок tarball v1.16 минус одна строка `#include "handle_form.inl"`), никаких проектных файлов внутри.
- `src/CMakeLists.txt`: перед `add_library(proj_civetweb ...)` добавлен `file(WRITE)` обеих заглушек в `${CMAKE_CURRENT_BINARY_DIR}/civetweb-stubs` + этот каталог в `target_include_directories(proj_civetweb PRIVATE ...)`. Смысл не изменилась (no-op лог доступа и no-op запись ошибок), но проектные файлы больше не лежат среди вендоренных и не могут быть приняты за апстримные при проверке вендоринга.
- `scripts/update-vendored-libs.py`: обе записи удалены из `PATCHED` (иначе `--prune` ждал бы их в дереве), комментарий над `PATCHED` уточнён — там остаётся только про локальные патчи и `core_export.h`, который генерирует апстримный CMake.
- `src/VENDORED-LIBS.md`: обещание civetweb описано через `#error`-требование `NO_FILESYSTEMS` + генерацию заглушек в build-каталог.
- Поправлена неверная формулировка в записи round 56.

### Проверка
- `./rebuild-and-run.sh`: rc=0 (обе заглушки генерируются, `proj_civetweb` собирается, юнит-тесты зелёные), все сервисы `healthy`.
- `./health-check.sh all`: rc=0. `python3 message_counter.py --iterations 1 --concurrent 1`: rc=0 (проверен и `/metrics`-путь, он отдаётся тем же civetweb).

