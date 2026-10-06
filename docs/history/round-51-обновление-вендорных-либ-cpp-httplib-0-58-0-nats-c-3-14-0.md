# round 51: обновление вендорных либ (cpp-httplib 0.58.0, nats.c 3.14.0)

## Date: 2026-09-23

### Что сделано
- `scripts/update-vendored-libs.py` + `src/VENDORED-LIBS.md`: подняты пины `cpp-httplib` v0.56.0 → v0.58.0 (@ 4f3f9ef) и `nats.c` 3.14.0-beta (коммит 9cae3733) → v3.14.0 (@ 6cb096a7).
- Ресинк деревьев: `httplib/httplib.h`+`httplib.cc` (split.py тега v0.58.0) и 7 файлов `nats/` (README.md, src/js.h, jsm.c, kv.c, nats.h, object.c, version.h). Локальные правки сохранены (json/nlohmann/*: NOLINT+GCC modules, nats/CMakeLists.txt, civetweb.c — не перезаписаны без --force).
- Прочие вендорные либы уже актуальны: nlohmann/json v3.12.0 (latest), odpi v26.0.0 (latest), civetweb v1.16 (latest), prometheus-cpp и base64 — master HEAD не сдвинулся.

### Проверка
- ./rebuild-and-run.sh: сборка в контейнере зелёная, юнит-тесты прошли, все сервисы healthy.
- ./health-check.sh all + python3 message_counter.py --iterations 1 --concurrent 1.

