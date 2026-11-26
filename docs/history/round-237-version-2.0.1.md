# Round 237 — Версия приложения поднята до 2.0.1

## Задача
Поднять версию приложения с `2.0.0` на `2.0.1` и сделать тэг для сравнения
времени сборки в CI (после round-236: убран ccache из kaniko-сборки).

## Реализация
- `src/generate_version.sh`: `VERSION "2.0.0-$SHA"` → `VERSION "2.0.1-$SHA"`.
- `src/l2-proxy-version.h` — build-generated (не трекается), пересоздаётся
  `generate_version.sh` при сборке, поэтому отдельно не менялся.

## Проверка
- `./rebuild-and-run.sh` — сборка зелёная, все сервисы healthy.
- Метрика `l2_common_build_info{mode="proxy",version="2.0.1-97d3c41"}` —
  версия `2.0.1` попала в бинарник.
- `message_counter.py --iterations 1 --concurrent 1` — exit 0.

## Тэг
- `git tag -f 2.0.1 HEAD && git push origin 2.0.1` — тэг для CI-сборки и
  сравнения времени сборки (kaniko, --target runtime).
