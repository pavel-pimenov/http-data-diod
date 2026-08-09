# Интеграция трейсинга с Oracle клиентом

## Формат Trace ID

Проект использует стандарт **W3C Trace Context**. Формат заголовка `traceparent`:

```
00-{trace-id}-{parent-span-id}-{trace-flags}
```

| Поле | Размер | Описание |
|------|--------|----------|
| Version | 2 hex символа | Всегда `00` |
| Trace ID | 32 hex символа | 128-bit случайное число |
| Parent Span ID | 16 hex символов | 64-bit случайное число |
| Trace Flags | 2 hex символа | `01` = sampled, `00` = not sampled |

**Общий размер**: 55 символов

**Пример**: `00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01`

## Реализация на PL/SQL

### Пакет trace_context

```sql
CREATE OR REPLACE PACKAGE trace_context AS
  -- Генерация случайной hex строки указанной длины
  FUNCTION random_hex(p_len PLS_INTEGER) RETURN VARCHAR2;
  
  -- Генерация traceparent заголовка
  FUNCTION generate_traceparent(
    p_trace_id   VARCHAR2 DEFAULT NULL,
    p_span_id    VARCHAR2 DEFAULT NULL,
    p_sampled    BOOLEAN  DEFAULT TRUE
  ) RETURN VARCHAR2;
  
  -- Парсинг traceparent заголовка
  PROCEDURE parse_traceparent(
    p_traceparent    IN  VARCHAR2,
    p_trace_id       OUT VARCHAR2,
    p_parent_span_id OUT VARCHAR2,
    p_sampled        OUT BOOLEAN
  );
  
  -- Валидация формата traceparent
  FUNCTION validate_traceparent(p_traceparent VARCHAR2) RETURN BOOLEAN;
END trace_context;
/

CREATE OR REPLACE PACKAGE BODY trace_context AS

  FUNCTION random_hex(p_len PLS_INTEGER) RETURN VARCHAR2 IS
    l_result VARCHAR2(256) := '';
    l_rand   PLS_INTEGER;
  BEGIN
    FOR i IN 1..p_len LOOP
      l_rand := MOD(ABS(DBMS_RANDOM.RANDOM), 16);
      l_result := l_result || SUBSTR('0123456789abcdef', l_rand + 1, 1);
    END LOOP;
    RETURN l_result;
  END random_hex;

  FUNCTION generate_traceparent(
    p_trace_id   VARCHAR2 DEFAULT NULL,
    p_span_id    VARCHAR2 DEFAULT NULL,
    p_sampled    BOOLEAN  DEFAULT TRUE
  ) RETURN VARCHAR2 IS
    l_trace_id  VARCHAR2(32) := NVL(p_trace_id, random_hex(32));
    l_span_id   VARCHAR2(16) := NVL(p_span_id, random_hex(16));
    l_flags     VARCHAR2(2)  := CASE WHEN p_sampled THEN '01' ELSE '00' END;
  BEGIN
    RETURN '00-' || l_trace_id || '-' || l_span_id || '-' || l_flags;
  END generate_traceparent;

  PROCEDURE parse_traceparent(
    p_traceparent    IN  VARCHAR2,
    p_trace_id       OUT VARCHAR2,
    p_parent_span_id OUT VARCHAR2,
    p_sampled        OUT BOOLEAN
  ) IS
    l_flags VARCHAR2(2);
  BEGIN
    IF NOT validate_traceparent(p_traceparent) THEN
      RAISE_APPLICATION_ERROR(-20001, 'Invalid traceparent format');
    END IF;
    
    p_trace_id       := SUBSTR(p_traceparent, 4, 32);
    p_parent_span_id := SUBSTR(p_traceparent, 37, 16);
    l_flags          := SUBSTR(p_traceparent, 54, 2);
    p_sampled        := (l_flags = '01');
  END parse_traceparent;

  FUNCTION validate_traceparent(p_traceparent VARCHAR2) RETURN BOOLEAN IS
  BEGIN
    IF LENGTH(p_traceparent) != 55 THEN RETURN FALSE; END IF;
    IF SUBSTR(p_traceparent, 1, 2) != '00' THEN RETURN FALSE; END IF;
    IF SUBSTR(p_traceparent, 3, 1) != '-' THEN RETURN FALSE; END IF;
    IF SUBSTR(p_traceparent, 36, 1) != '-' THEN RETURN FALSE; END IF;
    IF SUBSTR(p_traceparent, 53, 1) != '-' THEN RETURN FALSE; END IF;
    -- Проверка hex валидности
    IF NOT REGEXP_LIKE(SUBSTR(p_traceparent, 4, 32), '^[0-9a-f]{32}$') THEN RETURN FALSE; END IF;
    IF NOT REGEXP_LIKE(SUBSTR(p_traceparent, 37, 16), '^[0-9a-f]{16}$') THEN RETURN FALSE; END IF;
    IF NOT REGEXP_LIKE(SUBSTR(p_traceparent, 54, 2), '^[01]{2}$') THEN RETURN FALSE; END IF;
    RETURN TRUE;
  END validate_traceparent;

END trace_context;
/
```

### Пакет pkg_jaeger_client

```sql
CREATE OR REPLACE PACKAGE pkg_jaeger_client AS
  -- Константы
  c_jaeger_url CONSTANT VARCHAR2(256) := 'http://jaeger:14268/api/traces';
  c_proxy_url  CONSTANT VARCHAR2(256) := 'http://l2-proxy:8080';
  
  -- Типы
  TYPE t_response IS RECORD (
    status_code PLS_INTEGER,
    body        VARCHAR2(32767),
    traceparent VARCHAR2(55)
  );
  
  -- Установка ACL для HTTP доступа
  PROCEDURE setup_acl(
    p_acl_name  VARCHAR2 DEFAULT 'http_access.xml',
    p_principal VARCHAR2 DEFAULT USER,
    p_host      VARCHAR2 DEFAULT '*'
  );
  
  -- Отправка HTTP запроса с traceparent
  FUNCTION send_request(
    p_url        VARCHAR2,
    p_method     VARCHAR2 DEFAULT 'GET',
    p_body       CLOB    DEFAULT NULL,
    p_traceparent VARCHAR2 DEFAULT NULL
  ) RETURN t_response;
  
  -- Отправка span в Jaeger
  FUNCTION send_to_jaeger(
    p_trace_id     VARCHAR2,
    p_span_id      VARCHAR2,
    p_operation    VARCHAR2,
    p_tags         VARCHAR2 DEFAULT NULL,
    p_service_name VARCHAR2 DEFAULT 'oracle-client'
  ) RETURN t_response;
  
  -- Полный цикл: запрос + трейс
  FUNCTION request_with_trace(
    p_url         VARCHAR2,
    p_method      VARCHAR2 DEFAULT 'GET',
    p_body        CLOB    DEFAULT NULL,
    p_operation   VARCHAR2 DEFAULT 'oracle-client-request'
  ) RETURN t_response;
  
  -- Обработка входящего traceparent
  PROCEDURE process_incoming_trace(
    p_traceparent  IN  VARCHAR2,
    p_trace_id     OUT VARCHAR2,
    p_span_id      OUT VARCHAR2,
    p_sampled      OUT BOOLEAN
  );
  
  -- Асинхронная отправка span в Jaeger через DBMS_JOB (fire-and-forget)
  PROCEDURE send_to_jaeger_async(
    p_trace_id     VARCHAR2,
    p_span_id      VARCHAR2,
    p_operation    VARCHAR2,
    p_tags         VARCHAR2 DEFAULT NULL,
    p_service_name VARCHAR2 DEFAULT 'oracle-client'
  );
  
  -- Асинхронный полный цикл: запрос + трейс
  PROCEDURE request_with_trace_async(
    p_url         VARCHAR2,
    p_method      VARCHAR2 DEFAULT 'GET',
    p_body        CLOB    DEFAULT NULL,
    p_operation   VARCHAR2 DEFAULT 'oracle-client-request'
  );
  
  -- Внутренняя процедура для DBMS_JOB (не вызывать напрямую)
  PROCEDURE p_send_to_jaeger_job(
    p_trace_id     VARCHAR2,
    p_span_id      VARCHAR2,
    p_operation    VARCHAR2,
    p_tags         VARCHAR2,
    p_service_name VARCHAR2
  );
END pkg_jaeger_client;
/

CREATE OR REPLACE PACKAGE BODY pkg_jaeger_client AS

  PROCEDURE setup_acl(
    p_acl_name  VARCHAR2 DEFAULT 'http_access.xml',
    p_principal VARCHAR2 DEFAULT USER,
    p_host      VARCHAR2 DEFAULT '*'
  ) IS
  BEGIN
    BEGIN
      DBMS_NETWORK_ACL_ADMIN.DROP_ACL(acl => p_acl_name);
    EXCEPTION
      WHEN OTHERS THEN NULL;
    END;
    
    DBMS_NETWORK_ACL_ADMIN.CREATE_ACL(
      acl         => p_acl_name,
      description => 'HTTP Access for Jaeger',
      principal   => p_principal,
      is_grant    => TRUE,
      privilege   => 'connect',
      start_date  => SYSTIMESTAMP,
      end_date    => NULL
    );
    
    DBMS_NETWORK_ACL_ADMIN.ADD_PRIVILEGE(
      acl       => p_acl_name,
      principal => p_principal,
      is_grant  => TRUE,
      privilege => 'resolve'
    );
    
    DBMS_NETWORK_ACL_ADMIN.ASSIGN_ACL(
      acl  => p_acl_name,
      host => p_host
    );
    
    COMMIT;
  END setup_acl;

  FUNCTION send_request(
    p_url        VARCHAR2,
    p_method     VARCHAR2 DEFAULT 'GET',
    p_body       CLOB    DEFAULT NULL,
    p_traceparent VARCHAR2 DEFAULT NULL
  ) RETURN t_response IS
    l_req         UTL_HTTP.REQ;
    l_resp        UTL_HTTP.RESP;
    l_buffer      VARCHAR2(32767);
    l_traceparent VARCHAR2(55);
    l_result      t_response;
  BEGIN
    l_traceparent := NVL(p_traceparent, trace_context.generate_traceparent());
    
    l_req := UTL_HTTP.BEGIN_REQUEST(p_url, p_method, 'HTTP/1.1');
    
    UTL_HTTP.SET_HEADER(l_req, 'Content-Type', 'application/json');
    UTL_HTTP.SET_HEADER(l_req, 'traceparent', l_traceparent);
    
    IF p_body IS NOT NULL THEN
      UTL_HTTP.SET_HEADER(l_req, 'Content-Length', LENGTH(p_body));
      UTL_HTTP.WRITE_TEXT(l_req, p_body);
    END IF;
    
    l_resp := UTL_HTTP.GET_RESPONSE(l_req);
    
    l_result.status_code := l_resp.status_code;
    
    BEGIN
      UTL_HTTP.READ_TEXT(l_resp, l_buffer);
      l_result.body := l_buffer;
    EXCEPTION
      WHEN OTHERS THEN NULL;
    END;
    
    BEGIN
      UTL_HTTP.GET_HEADER_BY_NAME(l_resp, 'traceparent', l_buffer);
      l_result.traceparent := l_buffer;
    EXCEPTION
      WHEN UTL_HTTP.NO_HEADER_FOUND THEN
        l_result.traceparent := l_traceparent;
    END;
    
    UTL_HTTP.END_RESPONSE(l_resp);
    RETURN l_result;
    
  EXCEPTION
    WHEN OTHERS THEN
      BEGIN
        UTL_HTTP.END_RESPONSE(l_resp);
      EXCEPTION
        WHEN OTHERS THEN NULL;
      END;
      RAISE;
  END send_request;

  FUNCTION send_to_jaeger(
    p_trace_id     VARCHAR2,
    p_span_id      VARCHAR2,
    p_operation    VARCHAR2,
    p_tags         VARCHAR2 DEFAULT NULL,
    p_service_name VARCHAR2 DEFAULT 'oracle-client'
  ) RETURN t_response IS
    l_json    CLOB;
    l_tags    VARCHAR2(32767);
    l_result  t_response;
  BEGIN
    l_tags := NVL(p_tags, '{"span.kind":"client"}');
    
    l_json := '{
      "data": [{
        "traceID": "' || p_trace_id || '",
        "spanID": "' || p_span_id || '",
        "operationName": "' || p_operation || '",
        "startTime": ' || (SYSDATE - TO_DATE('01-01-1970','DD-MM-YYYY')) * 86400000 || ',
        "duration": 100,
        "tags": ' || l_tags || ',
        "process": {
          "serviceName": "' || p_service_name || '"
        }
      }]
    }';
    
    l_result := send_request(c_jaeger_url, 'POST', l_json);
    RETURN l_result;
  END send_to_jaeger;

  FUNCTION request_with_trace(
    p_url         VARCHAR2,
    p_method      VARCHAR2 DEFAULT 'GET',
    p_body        CLOB    DEFAULT NULL,
    p_operation   VARCHAR2 DEFAULT 'oracle-client-request'
  ) RETURN t_response IS
    l_traceparent VARCHAR2(55);
    l_trace_id    VARCHAR2(32);
    l_span_id     VARCHAR2(16);
    l_sampled     BOOLEAN;
    l_response    t_response;
  BEGIN
    l_traceparent := trace_context.generate_traceparent();
    
    trace_context.parse_traceparent(l_traceparent, l_trace_id, l_span_id, l_sampled);
    
    l_response := send_request(p_url, p_method, p_body, l_traceparent);
    
    IF l_sampled THEN
      send_to_jaeger(l_trace_id, l_span_id, p_operation);
    END IF;
    
    RETURN l_response;
  END request_with_trace;

  PROCEDURE process_incoming_trace(
    p_traceparent  IN  VARCHAR2,
    p_trace_id     OUT VARCHAR2,
    p_span_id      OUT VARCHAR2,
    p_sampled      OUT BOOLEAN
  ) IS
  BEGIN
    trace_context.parse_traceparent(p_traceparent, p_trace_id, p_span_id, p_sampled);
  END process_incoming_trace;

  PROCEDURE p_send_to_jaeger_job(
    p_trace_id     VARCHAR2,
    p_span_id      VARCHAR2,
    p_operation    VARCHAR2,
    p_tags         VARCHAR2,
    p_service_name VARCHAR2
  ) IS
    l_response t_response;
  BEGIN
    l_response := send_to_jaeger(p_trace_id, p_span_id, p_operation, p_tags, p_service_name);
  EXCEPTION
    WHEN OTHERS THEN
      syslog.error('pkg_jaeger_client.p_send_to_jaeger_job: trace_id=' || p_trace_id
        || ' span_id=' || p_span_id
        || ' operation=' || p_operation
        || ' error=' || SQLERRM);
  END p_send_to_jaeger_job;

  PROCEDURE send_to_jaeger_async(
    p_trace_id     VARCHAR2,
    p_span_id      VARCHAR2,
    p_operation    VARCHAR2,
    p_tags         VARCHAR2 DEFAULT NULL,
    p_service_name VARCHAR2 DEFAULT 'oracle-client'
  ) IS
    l_job_name VARCHAR2(30);
  BEGIN
    l_job_name := 'JAEGER_' || p_trace_id || '_' || TO_CHAR(SYSTIMESTAMP, 'FF6');
    
    DBMS_JOB.SUBMIT(
      job       => l_job_name,
      what      => 'BEGIN pkg_jaeger_client.p_send_to_jaeger_job(''' || p_trace_id || ''', ''' || p_span_id || ''', ''' || p_operation || ''', ''' || NVL(p_tags, '{"span.kind":"client"}') || ''', ''' || p_service_name || '''); END;',
      next_date => SYSDATE,
      no_parse  => FALSE
    );
    
    COMMIT;
  END send_to_jaeger_async;

  PROCEDURE request_with_trace_async(
    p_url         VARCHAR2,
    p_method      VARCHAR2 DEFAULT 'GET',
    p_body        CLOB    DEFAULT NULL,
    p_operation   VARCHAR2 DEFAULT 'oracle-client-request'
  ) IS
    l_traceparent VARCHAR2(55);
    l_trace_id    VARCHAR2(32);
    l_span_id     VARCHAR2(16);
    l_sampled     BOOLEAN;
    l_response    t_response;
  BEGIN
    l_traceparent := trace_context.generate_traceparent();
    trace_context.parse_traceparent(l_traceparent, l_trace_id, l_span_id, l_sampled);
    
    l_response := send_request(p_url, p_method, p_body, l_traceparent);
    
    IF l_sampled THEN
      send_to_jaeger_async(l_trace_id, l_span_id, p_operation);
    END IF;
  END request_with_trace_async;

END pkg_jaeger_client;
/
```

## Тип запроса в Jaeger

Проект использует **TCP** подключение для отправки трейсов. Jaeger агент работает по протоколу **Thrift over TCP** на порту `6831`.

### Архитектура взаимодействия

```
Oracle Client ──TCP──▶ Jaeger Agent (6831) ──TCP──▶ Jaeger Collector (14268) ──▶ Jaeger Storage
```

- **TCP**: Основной протокол для отправки span данных
- **Thrift**: Формат сериализации (Compact Protocol)
- **Порт 6831**: Jaeger agent (UDP/TCP)
- **Порт 14268**: Jaeger collector (HTTP)

### Формат запроса

Jaeger принимает данные в формате **Thrift IDL**. Каждый span содержит:

```thrift
struct Span {
  1: required i64 traceIdLow,
  2: required i64 traceIdHigh,
  3: required i64 spanId,
  4: required string operationName,
  5: optional list<SpanReference> references,
  6: required i64 startTime,
  7: required i64 duration,
  8: optional map<string, string> tags,
  9: optional list<Log> logs,
  10: optional Process process,
  11: optional i64 flags,
}
```

## Jaeger Endpoint

- **Internal (Docker)**: `http://jaeger:14268/api/traces`
- **UI**: `http://localhost:16686`
- **Agent (TCP/UDP)**: `jaeger:6831`
- **Collector (HTTP)**: `jaeger:14268`

## Примеры использования

### 1. Настройка ACL для HTTP доступа

```sql
BEGIN
  pkg_jaeger_client.setup_acl(
    p_acl_name  => 'http_access.xml',
    p_principal => 'MY_SCHEMA',
    p_host      => '*'
  );
END;
/
```

### 2. Простой HTTP запрос с traceparent

```sql
DECLARE
  l_response pkg_jaeger_client.t_response;
BEGIN
  l_response := pkg_jaeger_client.send_request(
    p_url    => 'http://l2-proxy:8080/api/data',
    p_method => 'GET'
  );
  
  DBMS_OUTPUT.PUT_LINE('Статус: ' || l_response.status_code);
  DBMS_OUTPUT.PUT_LINE('Ответ: ' || l_response.body);
  DBMS_OUTPUT.PUT_LINE('Traceparent: ' || l_response.traceparent);
END;
/
```

### 3. POST запрос с телом и трейсом

```sql
DECLARE
  l_response pkg_jaeger_client.t_response;
  l_body     CLOB := '{"key":"value"}';
BEGIN
  l_response := pkg_jaeger_client.send_request(
    p_url    => 'http://l2-proxy:8080/api/data',
    p_method => 'POST',
    p_body   => l_body
  );
  
  DBMS_OUTPUT.PUT_LINE('Статус: ' || l_response.status_code);
  DBMS_OUTPUT.PUT_LINE('Ответ: ' || l_response.body);
END;
/
```

### 4. Полный цикл: запрос + отправка в Jaeger

```sql
DECLARE
  l_response pkg_jaeger_client.t_response;
BEGIN
  l_response := pkg_jaeger_client.request_with_trace(
    p_url       => 'http://l2-proxy:8080/api/data',
    p_method    => 'POST',
    p_body      => '{"key":"value"}',
    p_operation => 'oracle-db-call'
  );
  
  DBMS_OUTPUT.PUT_LINE('Статус: ' || l_response.status_code);
  DBMS_OUTPUT.PUT_LINE('Ответ: ' || l_response.body);
  DBMS_OUTPUT.PUT_LINE('Traceparent: ' || l_response.traceparent);
END;
/
```

### 5. Отправка span в Jaeger напрямую

```sql
DECLARE
  l_response pkg_jaeger_client.t_response;
BEGIN
  l_response := pkg_jaeger_client.send_to_jaeger(
    p_trace_id     => '4bf92f3577b34da6a3ce929d0e0e4736',
    p_span_id      => '00f067aa0ba902b7',
    p_operation    => 'custom-operation',
    p_tags         => '{"span.kind":"client","db.type":"oracle"}',
    p_service_name => 'oracle-client'
  );
  
  DBMS_OUTPUT.PUT_LINE('Jaeger статус: ' || l_response.status_code);
END;
/
```

### 6. Обработка входящего traceparent

```sql
DECLARE
  l_trace_id    VARCHAR2(32);
  l_span_id     VARCHAR2(16);
  l_sampled     BOOLEAN;
  l_traceparent VARCHAR2(55) := '00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01';
BEGIN
  pkg_jaeger_client.process_incoming_trace(
    p_traceparent => l_traceparent,
    p_trace_id    => l_trace_id,
    p_span_id     => l_span_id,
    p_sampled     => l_sampled
  );
  
  DBMS_OUTPUT.PUT_LINE('Trace ID: ' || l_trace_id);
  DBMS_OUTPUT.PUT_LINE('Span ID: ' || l_span_id);
  DBMS_OUTPUT.PUT_LINE('Sampled: ' || CASE WHEN l_sampled THEN 'ДА' ELSE 'НЕТ' END);
END;
/
```

### 7. Асинхронная отправка span в Jaeger (fire-and-forget)

```sql
BEGIN
  pkg_jaeger_client.send_to_jaeger_async(
    p_trace_id     => '4bf92f3577b34da6a3ce929d0e0e4736',
    p_span_id      => '00f067aa0ba902b7',
    p_operation    => 'async-operation',
    p_tags         => '{"span.kind":"client"}',
    p_service_name => 'oracle-client'
  );
  
  DBMS_OUTPUT.PUT_LINE('Задача отправлена в очередь');
END;
/
```

### 8. Асинхронный полный цикл: запрос + трейс

```sql
BEGIN
  pkg_jaeger_client.request_with_trace_async(
    p_url       => 'http://l2-proxy:8080/api/data',
    p_method    => 'POST',
    p_body      => '{"key":"value"}',
    p_operation => 'async-db-call'
  );
  
  DBMS_OUTPUT.PUT_LINE('Запрос отправлен, трейс будет доставлен асинхронно');
END;
/
```

### 9. Проверка выполнения jobs

```sql
SELECT job, what, next_date, interval, failures
FROM user_jobs
WHERE what LIKE '%JAEGER%';
```
