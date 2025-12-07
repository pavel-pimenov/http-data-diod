# http-redis-proxy

sudo apt install libhiredis-dev libmicrohttpd-dev libjsoncpp-dev libcurl4-openssl-dev python3-pip 
sudo apt-get install apache2-utils

# sudo apt-get install  opentelemetry-cpp-dev (only ubuntu 25.10)

Perfect! The docker-compose.yml has been updated successfully:

- Replaced Redis with Valkey (using `valkey/valkey:alpine` image)

- Added a custom network configuration with a specific subnet (`172.25.0.0/16`) to avoid Docker's address pool exhaustion error

- All containers are now running:

  - Valkey (Redis-compatible database) on port 6379
  - l2-proxy on port 8888
  - l2-server on port 3000
  - l2-worker (running in the background)

The services are properly interconnected and should work as intended for the HTTP-Redis proxy system.

## benchmark.py

Python скрипт с асинхронным тестированием:

- Использует aiohttp для HTTP запросов
- Поддерживает параметры командной строки: URL, количество запросов, одновременность
- Выводит подробную статистику: среднее время, процентили, статус-коды
- По умолчанию тестирует [](http://0.0.0.0:8888/)<http://0.0.0.0:8888/> с 1000 запросами и 50 одновременными

## benchmark.sh

Shell скрипт с Apache Bench:

- Использует утилиту `ab` для тестирования
- Аналогичные параметры командной строки
- Сохраняет данные о времени ответа в TSV файл для дальнейшего анализа
- Проверяет наличие `ab` перед запуском

Оба скрипта готовы к использованию. Для запуска Python скрипта потребуется aiohttp (`pip install aiohttp`).


Добавлен сервис Traefik, который слушает порт 443 и проксирует HTTPS-запросы на localhost к сервису l2-proxy на порт 8888. Конфигурация проверена на валидность с помощью docker-compose config.

## Added Metrics

The following Prometheus counters with the `l2_proxy_` prefix have been added:

1. __`l2_proxy_client_requests_total`__ - Total number of client requests received
2. __`l2_proxy_redis_requests_total`__ - Total number of Redis operations performed
3. __`l2_proxy_client_request_errors_total`__ - Total number of client request errors
4. __`l2_proxy_redis_errors_total`__ - Total number of Redis operation errors

## Implementation Details

- __Prometheus Registry__: Created a global registry to manage all metrics

- __Exposer__: Added a Prometheus HTTP exposer on port 9090 at `/metrics` endpoint

- __Counter Integration__:

  - Client requests are counted in `RequestHandler::handle_request()`
  - Redis operations are counted for all Redis commands (GET, SET, RPUSH, INCR, PING)
  - Errors are tracked for both client requests (when Redis push fails) and Redis operations (when commands fail or return unexpected types)

- __Health Endpoint__: Added `/health` handler that performs Redis PING and counts it as a Redis operation

## Endpoints

- __Main Service__: `http://0.0.0.0:8888` (existing HTTP proxy)
- __Metrics__: `http://0.0.0.0:9090/metrics` (new Prometheus metrics endpoint)
- __Health Check__: `http://0.0.0.0:8888/health` (new health check endpoint)

The service has been built successfully and is ready for deployment. The metrics will help monitor client request volume, Redis operation performance, and error rates as requested.

**

## ummary of Changes

1. __Copied prometheus-cpp library__ from l2-proxy to l2-worker directory

2. __Copied CivetServer__ dependency for HTTP server functionality

3. __Updated CMakeLists.txt__ to include all necessary prometheus-cpp and CivetServer source files and include directories

4. __Modified main.cpp__ to include Prometheus metrics with:

   - Registry and counter definitions for worker-specific metrics

   - Metrics tracking:

     - `l2_worker_requests_processed_total`: Total requests processed
     - `l2_worker_redis_operations_total`: Total Redis operations
     - `l2_worker_l2_calls_total`: Total L2 server calls
     - `l2_worker_redis_errors_total`: Redis operation errors
     - `l2_worker_l2_errors_total`: L2 server call errors

   - Counter increments at appropriate points in the code

   - Prometheus Exposer running on port 9091 (different from l2-proxy's 9090)

## Key Differences from l2-proxy

- __Port__: l2-worker exposes metrics on port 9091 instead of 9090
- __Metrics__: Worker-specific metrics focused on request processing, Redis operations, and L2 server calls
- __Same technology stack__: Uses identical prometheus-cpp library and CivetServer for HTTP serving

The implementation has been successfully compiled and is ready for deployment. The metrics will be available at `http://0.0.0.0:9091/metrics` once the worker is running.

**

I've successfully added the requested functionality to `test.py`. Here's what was implemented:

## Changes Made:

1. __Added command-line options__ for specifying JSON payload size range:

   - `--min-size`: Minimum payload size in bytes (default: 200)
   - `--max-size`: Maximum payload size in bytes (default: 1,000,000)

2. __Created a function__ `generate_random_json_payload(size_bytes)` that generates random JSON payloads of approximately the specified size in bytes.

3. __Modified payload generation__:

   - Functionality test uses a fixed 500-byte payload
   - Load test requests use random payload sizes between the specified min and max values

4. __Added necessary imports__: `argparse`, `random`, `string`

## Usage:

Run the script with default sizes (200 bytes to 1 MB):

```bash
python test.py
```

Specify custom size range:

```bash
python test.py --min-size 500 --max-size 500000
```

The script now generates random JSON messages with varying sizes within the specified range for load testing, while maintaining the original functionality for basic endpoint testing.

I've added comprehensive diagnostic logging to test.py to help identify where the hanging occurs. Here's what I added:

1. __Logging Setup__: Added logging configuration that outputs to both console and a file (`test_debug.log`) with timestamps.

2. __Functionality Test Diagnostics__: Added logs for the start and end of the JSON endpoint test, including error details.

3. __Load Test Diagnostics__: Added detailed logs throughout the async load testing:

   - Start of load test
   - Creation of aiohttp session
   - Task creation
   - Start and completion of `asyncio.gather`
   - Individual request start/completion with timing and status
   - Exception handling with details

4. __Main Function Logs__: Added logs for script start, configuration, and final results.

The logs will show:

- Timestamps for all major operations
- Which request is being processed (if hanging on a specific one)
- How long each request takes
- Any exceptions or errors during requests
- Progress through the load test phases

When you run the test now, check both the console output and the `test_debug.log` file. The logs will help pinpoint exactly where the script hangs - whether it's during task creation, the gather operation, or specific requests.

To run the test and capture the diagnostics, use:

```bash
python3 test.py
```

The detailed logging should reveal the exact point of hanging.

I've successfully merged the l2-proxy and l2-worker projects into a single l2-service project. Here's what was accomplished:

1. __Analyzed both projects__: Understood that l2-proxy acts as an HTTP server that queues requests to Redis, and l2-worker processes those requests by calling an external L2 server.

2. __Combined main.cpp__: Created a single executable that checks the `MODE` environment variable to run either as "proxy" or "worker" mode.

3. __Updated CMakeLists.txt__: Added CURL dependency, changed project name to l2-service, and updated executable name and linking.

4. __Renamed directory__: Moved cpp/l2-proxy to cpp/l2-service.

5. __Updated Dockerfile__: Added libcurl dependencies, changed executable name, exposed additional ports (9090 for proxy metrics, 9091 for worker metrics).

6. __Removed l2-worker directory__: Cleaned up the old separate project.

The merged project now uses a single binary that can be run in either mode by setting the `MODE` environment variable to "proxy" or "worker". The proxy mode listens on port 8888 and serves metrics on 9090, while worker mode processes requests and serves metrics on 9091.

I've updated the docker-compose.yml to run the merged l2-service in two instances with the appropriate modes:

- __l2-service-proxy__: Runs with `MODE=proxy`, exposes ports 8888 (HTTP) and 9090 (Prometheus metrics), and acts as the HTTP proxy that queues requests to Redis.

- __l2-service-worker__: Runs with `MODE=worker`, exposes port 9091 (Prometheus metrics), and processes requests from the Redis queue by calling the l2-server.

The services are properly configured with dependencies:

- The proxy depends on Valkey being healthy
- The worker depends on both Valkey and l2-server
- Traefik is updated to depend on the proxy service

Now you can run `docker-compose up` and it will start:

- Valkey (Redis) on port 6379
- l2-server (echo server) on port 3000
- l2-service-proxy on ports 8888/9090
- l2-service-worker on port 9091
- RedisInsight on port 5540
- Traefik on port 443

The system will work as before: HTTP requests to port 8888 get queued, processed by the worker, and sent to the l2-server.


I've successfully added OpenObserve tracing integration to the http-redis-proxy project. Here's what was implemented:

## Changes Made:

### 1. __Dependencies Update__

- Updated `CMakeLists.txt` to fetch OpenTelemetry C++ SDK v1.14.2 using FetchContent
- Added OpenTelemetry libraries to the target link libraries
- Updated `Dockerfile` to include protobuf and absl dependencies required by OpenTelemetry

### 2. __Tracing Implementation__

- Added OpenTelemetry initialization in `main.cpp` with `init_tracing()` function

- Configured OTLP HTTP exporter to send traces to `http://localhost:5080/api/traces`

- Implemented basic authentication using base64-encoded login:password from environment variables

- Added tracing spans to key operations:

  - Proxy: `handle_request` span for incoming HTTP requests
  - Worker: `process_request` span for request processing

### 3. __Environment Configuration__

- Added environment variables to `docker-compose.yml` for both proxy and worker services:

  - `OPENOBSERVE_URL=http://localhost:5080`
  - `OPENOBSERVE_LOGIN=${OPENOBSERVE_LOGIN}`
  - `OPENOBSERVE_PASSWORD=${OPENOBSERVE_PASSWORD}`

### 4. __Asynchronous Operation__

- Tracing is implemented asynchronously using OpenTelemetry's default batch processor with background export threads
- Does not block the main service threads during trace collection and export

## Usage:

Set the OpenObserve credentials in your environment before running:

```bash
export OPENOBSERVE_LOGIN=your_login
export OPENOBSERVE_PASSWORD=your_password
docker-compose up
```

docker run -v $PWD/data:/data -e ZO_DATA_DIR="/data" -p 5080:5080 -e ZO_ROOT_USER_EMAIL="root@example.com" -e ZO_ROOT_USER_PASSWORD="admin" o2cr.ai/openobserve/openobserve-enterprise:v0.20.2
The traces will be sent to the OpenObserve instance at `localhost:5080` with proper authentication, allowing you to monitor request flows through the proxy and worker components without impacting performance.


Perfect! The build completed successfully and quickly. I've successfully removed all OpenTelemetry dependencies and integrated the TraceLogger class for tracing HTTP requests via simple HTTP calls.

## Summary of Changes:

1. __Removed OpenTelemetry dependencies__ from CMakeLists.txt
2. __Added nlohmann/json__ (header-only library) as a replacement
3. __Removed all OpenTelemetry includes__ from main.cpp
4. __Integrated TraceLogger__ with proper initialization using environment variables (OPENOBSERVE_URL, OPENOBSERVE_LOGIN, OPENOBSERVE_PASSWORD)
5. __Replaced tracing logic__ in both proxy (`handle_request`) and worker (`process_request`) methods with TraceLogger usage
6. __Verified fast build__ - the project now builds much quicker without the heavy OpenTelemetry dependency

The TraceLogger sends traces to the configured OpenObserve endpoint with attributes like HTTP method, URL, status code, and request ID, exactly as requested. The build time has been significantly improved by removing the large OpenTelemetry library.


## How to Use:

To enable Jaeger tracing:

```bash
export USE_JAEGER=ON
docker-compose up
```

The Jaeger UI will be available at `http://localhost:16686` when the service is running.

Both tracing backends are mutually exclusive at compile ti


The code has been fixed to properly send traces to Jaeger:2.13.0. Here are the key changes made:

1. __Fixed the Jaeger endpoint duplication bug__: In `main.cpp`, the JaegerLogger was incorrectly appending "/api/traces" to an already complete JAEGER_URL environment variable, resulting in "[](http://jaeger:14268/api/traces/api/traces)<http://jaeger:14268/api/traces/api/traces>". This has been corrected to use the endpoint directly.

2. __Enabled Jaeger tracing__: Updated `docker-compose.yml` to set `USE_JAEGER=ON` for both the l2-service-proxy and l2-service-worker services.

The JaegerLogger sends traces in the correct Jaeger JSON format to `http://jaeger:14268/api/traces`, which Jaeger 2.13.0 accepts via its HTTP collector API. Jaeger is configured with OTLP enabled, but it still supports the legacy Jaeger format at this endpoint.

The services will now send traces for HTTP requests processed by the proxy and worker components. You can view the traces in the Jaeger UI at [](http://localhost:16686)<http://localhost:16686> after making requests to the proxy at [](http://localhost:8888.)<http://localhost:8888.>

've successfully updated the code to read the number of threads from an environment variable. Here's what I changed:

1. __Added environment variable constant__: Defined `NUM_THREADS_ENV = "NUM_THREADS"` at the top of the file alongside other environment variable constants.

2. __Modified `run_proxy()` function__: Added code to read the `NUM_THREADS` environment variable:

   ```cpp
   const char* num_threads_env = std::getenv(NUM_THREADS_ENV);
   std::string num_threads = num_threads_env ? std::string(num_threads_env) : "32";
   ```

3. __Updated CivetServer options__: Replaced the hardcoded options array with a dynamic `std::vector<std::string>` that uses the environment variable value for `num_threads`.

The code now defaults to 32 threads if `NUM_THREADS` is not set, maintaining backward compatibility. You can now configure the number of threads by setting the `NUM_THREADS` environment variable when running the proxy.

I have successfully rewritten the trace transmission through headers to use the W3C Trace Context format. Here's what was implemented:

## Changes Made

### 1. __Added W3C Trace Context Helper Methods__ (`trace_loger.hpp`)

- `generate_traceparent(trace_id, span_id, sampled)`: Creates traceparent header in format `00-{trace-id}-{parent-id}-{flags}`
- `parse_traceparent(traceparent, trace_id, parent_id, sampled)`: Parses incoming traceparent header and extracts components

### 2. __Updated RequestHandler__ (`main.cpp`)

- __Incoming requests__: Now parses `traceparent` header instead of custom `trace_id`/`span_id` headers
- __Fallback__: If no valid traceparent header is present, generates new trace context
- __Response headers__: Returns `traceparent` header in responses for proper trace propagation

### 3. __Updated L2Worker__ (`main.cpp`)

- __Child span creation__: Generates new span IDs for L2 server calls
- __Header propagation__: Sets `traceparent` header when making HTTP requests to L2 server
- __Trace continuity__: Maintains trace context across service boundaries

### 4. __Format Compliance__

The implementation follows the W3C Trace Context specification:

- __Header name__: `traceparent`
- __Format__: `00-{trace-id}-{parent-id}-{flags}`
- __Example__: `traceparent: 00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01`

## Key Features

✅ __Backward Compatible__: Still works with existing trace logging infrastructure\
✅ __Standards Compliant__: Uses official W3C Trace Context format\
✅ __Distributed Tracing__: Properly propagates trace context across service calls\
✅ __Error Handling__: Validates traceparent format and falls back gracefully\
✅ __Build Verified__: Code compiles successfully without errors

The proxy now correctly handles W3C Trace Context headers for distributed tracing, enabling proper trace correlation across microservices that follow the W3C standard.


Refactoring Summary: Unified JSON Library Usage
Problem: The code was using two different JSON libraries inconsistently:

nlohmann::json in trace_loger.hpp for tracing spans
JsonCpp (Json::Value, Json::Reader, Json::StreamWriterBuilder) in main.cpp for request/response handling
Solution: Standardized on nlohmann::json throughout main.cpp for consistency and simplicity.

Key Changes:

Removed #include "jsoncpp/json.h"
Added using json = nlohmann::json; for cleaner syntax
Replaced all Json::Value declarations with json
Updated JSON parsing from Json::Reader to exception-safe json::parse() with try-catch
Simplified serialization from Json::StreamWriterBuilder to json.dump()
Modernized JSON access methods:
value.asString() → value (implicit conversion)
obj.isMember("key") → obj.contains("key")
Improved JSON object construction using initializer lists
Benefits:

Consistency: Single JSON library across the entire codebase
Simplicity: Less boilerplate code for JSON operations
Modern C++: Exception-safe parsing, cleaner syntax
Maintainability: Easier to work with one JSON API
Performance: nlohmann::json is header-only and efficient
The code compiles successfully and maintains all existing functionality. This refactoring makes the codebase cleaner and more maintainable by eliminating redundant dependencies and inconsistent APIs.