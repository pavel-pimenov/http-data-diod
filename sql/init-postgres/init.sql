-- Demo data for the HTTP DB gateway (POST /v1/sql/postgres/query).
-- postgres image runs *.sql scripts as POSTGRES_USER in POSTGRES_DB on first
-- boot, so no schema prefix is needed.
CREATE TABLE IF NOT EXISTS demo_messages (
  id         BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  message    VARCHAR(512) NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

INSERT INTO demo_messages (message) VALUES ('Hello from PostgreSQL DB gateway');
INSERT INTO demo_messages (message) VALUES ('DB gateway works over NATS');

-- Совместимость: нагрузочный тест исторически шлёт SELECT * FROM test_table (6h прогон).
-- Чтобы не сыпать 422 SQL_ERROR в логи, держим таблицу рядом с demo_messages.
CREATE TABLE IF NOT EXISTS test_table (
  id   SERIAL PRIMARY KEY,
  data TEXT NOT NULL
);
INSERT INTO test_table (data) VALUES ('hello') ON CONFLICT DO NOTHING;
