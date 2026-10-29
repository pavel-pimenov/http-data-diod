# round 204 — Grafana удалена из стека: Perses остаётся единственным UI

## Что было не так

В compose-стеке жили два UI для одних и тех же метрик: **Grafana** (`:3000`,
образ `grafana/grafana:12.4`, ~850 МБ) и **Perses** (`:8089`, ~90 МБ).
Дашборды при этом строились из одного источника
(`scripts/generate-grafana-dashboards.py`), то есть Grafana не давала ничего,
чего не даёт Perses, — но тянула за собой три лишних сущности:

- **второй синк дашбордов**: `rebuild-and-run.sh` ждал готовности Grafana до
  30 итераций, заливал дашборды и datasource через API и печатал подсказки про
  `admin/admin`;
- **гейт паритета**: `scripts/compare-grafana-perses.py` поднимал оба UI и
  сверял наборы дашбордов, панели, PromQL и результаты запросов через два
  разных API (`/api/ds/query` против `/proxy/globaldatasources/...`), а
  `scripts/test-grafana-generator.sh` отдельно поднимал временную Grafana ради
  проверки генератора;
- **третий том и контейнер**: `grafana-storage` + healthcheck с `curl`, которого
  нет в distroless-стиле Perses.

История вопроса тоже показательна: в round 66 Grafana чуть не была удалена в
пользу Perses, но осталась; в CI её адрес подменяли на мёртвый порт, из-за чего
обновление молча пропускалось и гейт паритета падал каждый прогон («Grafana=0
Perses=8»). Поддержка второго UI стоила внимания, а не давала возможностей.

## Что сделано

### Compose

- Удалён сервис `grafana` (образ, порт `3000`, env `GF_*`, healthcheck,
  `depends_on`) и volume `grafana-storage`.
- Комментарий у `perses` переписан: Perses — **единственный** UI, определения
  панелей берутся из того же генератора; healthcheck по-прежнему внешний
  (базовый образ без `curl`/`sh`).

### Скрипты

- `rebuild-and-run.sh`: удалён блок «Update Grafana dashboards» (ожидание
  `/api/health`, выбор креденшелов `GRAFANA_API_KEY`/`GRAFANA_USER`, вызов
  генератора с `--correct-dashboards`); остался только синк Perses. Из очистки
  конфликтующих контейнеров убран `docker rm -f grafana`.
- `scripts/ci-gate.sh`: из runtime-гейта убран вызов
  `compare-grafana-perses.py --allow-down`, шапка обновлена.
- `scripts/compare-grafana-perses.py` и `scripts/test-grafana-generator.sh`
  **оставлены в репозитории**, но отвязаны от стека и гейтов: вызывать их
  больше нечему (Grafana из compose удалена), а удалять из git не стали —
  скрипты понадобятся, если Grafana вернётся как второй UI.
- `scripts/generate-grafana-dashboards.py` **оставлен** и остаётся источником
  определений панелей/PromQL: `scripts/generate-perses-dashboards.py` строит из
  него нативные Perses-дашборды. Имя историческое — в README это оговорено.

### Документация и CI

- README: раздел «Grafana-дашборды» переписан в «Дашборды (Perses, генерация
  скриптом)», раздел про Perses больше не описывает живое сравнение с Grafana,
  убраны упоминания `:3000` и `admin/admin`; статус-страница `/stats` описана
  как «без веб-GUI»; таблица инфраструктуры и описание «шестого места, где
  встречается имя метрики» переведены на «генератор/Perses»; упоминания
  «в Grafana» в разделах про per-client метрики и «Хот-клиенты» заменены на
  Perses.
- `.github/workflows/ci.yml`: комментарий про `GRAFANA_URL` и гейт паритета
  заменён на «дашборды провижинятся только в Perses».
- Хост очищен руками: контейнер `grafana` остановлен и удалён, образ
  `grafana/grafana:12.4` снят.

## Проверка

- `docker compose config` — валиден; сервисы после удаления: `jaeger`,
  `l2-proxy`, `l2-server`, `l2-worker`, `nats-exporter`, `nats-server`, `nginx`,
  `nginx-exporter`, `perses`, `swagger-ui`, `victoria-metrics`, `vmagent`,
  `vmalert` (13 вместо 14).
- Полный `./rebuild-and-run.sh` на дефолтном стеке — зелёный: сборка,
  юнит-тесты, health-check, golden metrics; синк дашбордов
  `9/9 successful` в Perses (`http://localhost:8089`).
- `./scripts/ci-gate.sh unit` (95 тестов) и `./scripts/ci-gate.sh offline` —
  rc=0; `metrics-consistency-check.py --offline` по-прежнему сверяет PromQL
  генератора с C++ и Perses.
- Освобождено место: снятый образ ~850 МБ + `grafana-storage` том + build cache;
  `docker system df` до/после — сборка и запуск больше не упираются в диск
  (было 97 % занято, ~0.9 ГБ свободно).

## Ограничения (честно)

- `generate-grafana-dashboards.py` сохраняет режим отправки в **внешнюю**
  Grafana (`GRAFANA_URL`/`--correct-dashboards`): это опциональный экспорт в
  чужой инстанс, в compose Grafana больше не входит. Поэтому имя файла и
  упоминания Grafana внутри Perses-конвертера остались — это не «забытый»
  сервис, а совместимость формата.
- `scripts/compare-grafana-perses.py` и `scripts/test-grafana-generator.sh`
  не удалены из git, хотя сейчас никем не вызываются: они проверяют генератор
  и паритет с Grafana, и понадобятся при возврате второго UI.
- Исторические раунды в `docs/history/` описывают Grafana как часть стека —
  их не переписываем: это летопись, а не инструкция.
