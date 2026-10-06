# round 46: группировка членов Config (Nats/Ssl)

## Date: 2026-09-22

### Что сделано
- `src/config.hpp`: открытые члены Config, относящиеся к NATS-бэкенду и SSL/TLS, сгруппированы во вложенные struct-группы:
  - `Nats` (m_host/m_port/m_subject/m_queue_group/m_username/m_password/m_token/m_credentials_file/m_tls_*_file/m_enable_tls/m_timeout_ms) с экземпляром m_nats;
  - `Ssl` (m_enable_server_certificate_verification/m_enable_server_hostname_verification/m_ca_cert_path/m_server_cert_file/m_server_key_file) с экземпляром m_ssl.
  Члены групп сохраняют префикс m_, доступ — `m_nats.m_*` / `m_ssl.m_*`. Дефолтные значения перенесены в определения struct-групп.
- Обновлены ссылки в config.cpp (load_from_env/validate/create_nats_config), l2_worker.cpp, l2_worker_nats.cpp, main.cpp, nats_poll_service.cpp, proxy_init.cpp, test_components.cpp.

### Проверка
- ./rebuild-and-run.sh: сборка в контейнере зелёная, юнит-тесты прошли, все сервисы healthy.
- ./health-check.sh all + python3 message_counter.py --iterations 1 --concurrent 1.

