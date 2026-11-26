# Отчёт по Docker registry `ia-ursu-docker-prod.dp.nlmk.com`

Дата: 2026-11-10
Реестр: `ia-ursu-docker-prod.dp.nlmk.com`
Учётка: `datp-replication-ro` (read-only)
Скрипт: `scripts/registry-report.py`

## Сводка

| Репозиторий | Тегов | Размер (MB) |
|---|---|---|
| http-data-diod | 6 | 848 |
| http-data-diod/cache | 10 | 352 |
| http-data-diod-dashboards-sync | 2 | 199 |
| ar2 | 1 | 0 |
| crane-scales | 3 | 0 |
| crane-scales-moxa | 2 | 0 |
| iba-kafka-to-db | 4 | 0 |
| json-validator-be | 4 | 0 |
| json-validator-fe | 7 | 0 |
| opcua-eml-backend | 5 | 0 |
| opcua-eml-frontend | 5 | 0 |

**ИТОГО: ~1400 MB (1.4 GB)**

## Детализация по-тегово (репозитории > 10 MB)

### http-data-diod (848 MB)

| Тег | Размер (MB) |
|---|---|
| 1.0.0 | 0 |
| 1.0.2 | 88 |
| 1.0.3 | 88 |
| 2.0.0 | 223 |
| 2.0.1 | 224 |
| current | 224 |

### http-data-diod/cache (352 MB) — kaniko-кэш

| Слой | Размер (MB) |
|---|---|
| 07fbce208a2e... | 0 |
| 21082a8f8670... | 4 |
| 23b7e5ea7f8e... | 0 |
| 34aa617f8567... | 0 |
| 364abeaf44f2... | 28 |
| 50629466cbc3... | 0 |
| 826c1394c98d... | 161 |
| 905c9e9ec6dc... | 0 |
| c6aba6fb592c... | 0 |
| ea2152bc94c6... | 157 |

### http-data-diod-dashboards-sync (199 MB)

| Тег | Размер (MB) |
|---|---|
| (2 тега: latest, current) | ~199 суммарно |

## Наблюдения

- **Основное место (~1.4 GB)** занимают три репозитория http-data-diod-семейства.
- Остальные 8 проектов (ar2, crane-scales, iba-kafka-to-db, json-validator-be/fe,
  opcua-eml-*) имеют теги, но **манифесты недоступны** (MANIFEST_UNKNOWN/404) —
  они не занимают место в этом реестре (тег есть, образ недоступен / реплицирован).
- **http-data-diod/cache** — kaniko-кэш (TTL 168h). Два больших слоя (161 MB, 157 MB)
  — это apt-зависимости и компиляция. Кэш накапливается.
- Старые теги http-data-diod (`1.0.0`, `1.0.2`, `1.0.3`, `2.0.0`) — больше не актуальны
  (актуальны `2.0.1` и `current`).

## Рекомендации

1. **Удалить старые теги http-data-diod** — `1.0.0`, `1.0.2`, `1.0.3`, `2.0.0`
   (≈ 399 MB). Оставить `2.0.1` и `current`.
   ```
   curl -u user:pass -X DELETE https://ia-ursu-docker-prod.dp.nlmk.com/v2/http-data-diod/manifests/<digest>
   ```
   (нужен digest тега, не просто тег).

2. **Очистить/сократить kaniko-кэш** `http-data-diod/cache` (≈ 352 MB):
   - уменьшить `--cache-ttl` в `.gitlab-ci.yml` с `168h` до `24h`, либо
   - удалить кэш-репозиторий (сбросить кэш kaniko).

3. **Сократить dashboards-sync** — оставить только актуальный тег, удалить лишний
   (≈ 199 MB).

4. **Увеличить квоту репозитория** в Artifactory (админская задача), если 1.4 GB
   близко к лимиту.

## Повторный анализ

```bash
python3 scripts/registry-report.py
```

Скрипт использует креды из env `IA_URSU_DOCKER_USER_RO`/`IA_URSU_DOCKER_PASS_RO`
или дефолтные (datp-replication-ro).
