# Round 106: нормализация line-endings вендоренного `match.inl` (снят «вечно грязный» файл)

## Проблема

`git status` постоянно показывал один изменённый файл —
`src/prometheus-cpp/3rdparty/civetweb/src/match.inl` — на **262 строки**
(262 insertions / 262 deletions), хотя содержимое не менялось.
`git checkout -- <file>` откатывал рабочую копию к тому же состоянию,
и файл оставался dirty.

## Причина

Несогласованность line-endings между blob'ом в git и правилом `.gitattributes`:

* при вендоринге файл попал в репозиторий с **CRLF** в blob'е (upstream
  CivetWeb хранит исходники с CRLF) — `git ls-files --eol` показывал
  `i/crlf  w/crlf`;
* в round 65 в `.gitattributes` добавлено `*.inl text eol=lf`
  (репозиторий LF-only; `dos2unix-recursive.py` тоже держит `.inl` в
  `SOURCE_EXTS`);
* из-за атрибута git нормализует рабочую копию в LF при diff/commit, но
  **blob в индексе остался CRLF** → сравнение «LF-рабочая копия ↔ CRLF-blob»
  даёт различия по всем строкам. Файл стал вечно грязным, но без реальной
  правки содержимого; до round 65 (пока атрибута не было) он был чистым.

`match.inl` — **единственный** файл во всём репозитории с CRLF-blob'ом
(проверено `git ls-files --eol | awk '$1=="i/crlf"'`).

## Фикс

Renormalize по политике репозитория (LF-only):

1. рабочая копия переведена в LF (`b'\r\n'` → `b'\n'`, идентично поведению
   `dos2unix-recursive.py`);
2. `git add` — blob в индексе нормализован в LF.

Проверка: `git ls-files --eol` для файла теперь `i/lf  w/lf
attr/text eol=lf`, незастейдженных отличий нет. Содержимое лишь изменило
окончания строк, байт-в-байт после удаления CR идентично прежнему
(`diff <(tr -d '\r' ...)` — identical).

## Проверка

* `./rebuild-and-run.sh` — первая попытка упала на шаге юнит-тестов в
  builder'е: 2 кейса `test_db_live.cpp` (`[db-live]`, напр. `DbQueryHandler:
  re-init keeps already ready executors`, стр. 732) — `handler.init(...)` →
  `false`, «failed to connect to 127.0.0.1:1» для БД `other`. Это известный
  флейк живого Postgres-кластера в builder'е (ср. round 91: 12 пропущенных
  live-Postgres-тестов; round 95: 4 флейка, авто-ретрай прошёл): диск не
  переполнен (6.1 G свободно), компиляция прошла. Повторный запуск — rc=0,
  сборка/тесты зелёные; правка line-endings на тесты влиять не может.
* `./health-check.sh all` — rc=0, все проверки прошли.
* `python3 message_counter.py --iterations 1 --concurrent 1` — rc=0, success.
* `SKIP_CLANG_TIDY=1 ./scripts/pre-commit.sh "..."` — rc=0: Python-юниты,
  offline-гейты (metric-name/env-var/build-context), runtime-гейты
  (message counter, DB Gateway e2e, golden metrics 80/80, metric-consistency
  84/84, Grafana↔Perses parity), cppcheck — пройдены; shellcheck/cppcheck
  пропущены как «нет изменённых файлов» (`.inl` вендоренного дерева в
  набор не входит). clang-tidy пропущен (`SKIP_CLANG_TIDY=1`) — проектный
  C++ не затронут.
* Правка затрагивает только line-endings вендоренного файла: скомпилированный
  код не отличается, покрытие/baseline не меняются.
