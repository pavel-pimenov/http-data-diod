# Round 243 — Расширенное логирование подключения в lib nats.c (dialing IP:port)

## Задача
Добавить логирование в lib nats.c, чтобы было видно, к какому реальному адресу
подключается NATS-клиент (DNS round-robin / несколько A-записей). Логирование —
в stderr (консоль), видно в `docker logs`.

## Реализация
### `src/nats/src/comsock.c`
В `_dial` перед `connect()` (строка ~308) добавлен блок: преобразует
`p->ai_addr` (сокет-адрес) в IP через `inet_ntop` и выводит в stderr:
```
[nats] dialing <IP>:<port> (IPv4|IPv6)
```
- Обрабатывает AF_INET и AF_INET6 (порт через `ntohs`).
- При ошибке `inet_ntop` — `[nats] dialing (inet_ntop failed)`.
- Использует `fprintf(stderr, ...)` — попадает в stdout/stderr контейнера →
  видно в `docker logs`.

## Проверка
- rebuild-and-run.sh зелёный (NATS C client пересобран), все сервисы healthy.
- Логи l2-proxy: `[nats] dialing 172.24.0.10:4222 (IPv4)`.
- Логи l2-worker: `[nats] dialing 172.24.0.10:4222 (IPv4)`.
- message_counter exit 0 (no message loss).

## Ключевое
Теперь в `docker logs` видно, к какому реальному IP:порту подключается
NATS-клиент (после DNS-резолвинга), а не только hostname.
