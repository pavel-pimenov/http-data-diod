#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>
#include <random>
#include <string>
#include <ctime>
#include <unistd.h>
#include <mutex>
#include <cstdlib>
#include <csignal>
#include <atomic>

#include "CivetServer.h"
#include <hiredis/hiredis.h>
#include <curl/curl.h>

#include "jsoncpp/json.h"

#include <prometheus/registry.h>
#include <prometheus/exposer.h>
#include <prometheus/counter.h>

#include "nlohmann/json.hpp"

#if !defined(USE_JAEGER)
#define USE_JAEGER
#endif

#include "trace_loger.hpp"


using TracerType = JaegerLogger;

// Simple base64 encoding function
std::string base64_encode(const std::string& input) {
    const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    int i = 0;
    int j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    for (char c : input) {
        char_array_3[i++] = c;
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for (i = 0; i < 4; i++)
                encoded += base64_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for (j = i; j < 3; j++)
            char_array_3[j] = '\0';

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;

        for (j = 0; j < i + 1; j++)
            encoded += base64_chars[char_array_4[j]];

        while (i++ < 3)
            encoded += '=';
    }

    return encoded;
}



// Common variables
std::atomic<bool> shutdown_flag(false);

// Environment variable for mode
const char* MODE_ENV = "MODE";

// Environment variable for number of threads
const char* NUM_THREADS_ENV = "NUM_THREADS";

// Initialize Tracer

std::unique_ptr<JaegerLogger> tracer;
void init_tracer() {
    const char* jaeger_url = std::getenv("JAEGER_URL");

    if (!jaeger_url) {
        std::cerr << "JAEGER_URL not set, tracing disabled" << std::endl;
        return;
    }

    const std::string endpoint = std::string(jaeger_url);

    tracer = std::make_unique<JaegerLogger>(endpoint);
    std::cout << "JAEGER_URL set, tracing enbaled: " << endpoint << std::endl;
}

// Prometheus registry for proxy
std::shared_ptr<prometheus::Registry> proxy_registry = std::make_shared<prometheus::Registry>();

// Prometheus counters for proxy
auto& l2_proxy_client_requests_total = prometheus::BuildCounter()
    .Name("l2_proxy_client_requests_total")
    .Help("Total number of client requests received")
    .Register(*proxy_registry);

auto& l2_proxy_redis_requests_total = prometheus::BuildCounter()
    .Name("l2_proxy_redis_requests_total")
    .Help("Total number of Redis operations performed")
    .Register(*proxy_registry);

auto& l2_proxy_client_request_errors_total = prometheus::BuildCounter()
    .Name("l2_proxy_client_request_errors_total")
    .Help("Total number of client request errors")
    .Register(*proxy_registry);

auto& l2_proxy_redis_errors_total = prometheus::BuildCounter()
    .Name("l2_proxy_redis_errors_total")
    .Help("Total number of Redis operation errors")
    .Register(*proxy_registry);

auto& l2_proxy_bytes_received_total = prometheus::BuildCounter()
    .Name("l2_proxy_bytes_received_total")
    .Help("Total number of bytes received from clients")
    .Register(*proxy_registry);

auto& l2_proxy_bytes_sent_total = prometheus::BuildCounter()
    .Name("l2_proxy_bytes_sent_total")
    .Help("Total number of bytes sent to clients")
    .Register(*proxy_registry);

// Counter instances for proxy
prometheus::Counter& proxy_client_requests_counter = l2_proxy_client_requests_total.Add({});
prometheus::Counter& proxy_redis_requests_counter = l2_proxy_redis_requests_total.Add({});
prometheus::Counter& proxy_client_errors_counter = l2_proxy_client_request_errors_total.Add({});
prometheus::Counter& proxy_redis_errors_counter = l2_proxy_redis_errors_total.Add({});
prometheus::Counter& proxy_bytes_received_counter = l2_proxy_bytes_received_total.Add({});
prometheus::Counter& proxy_bytes_sent_counter = l2_proxy_bytes_sent_total.Add({});


class HealthHandler : public CivetHandler {
private:
    redisContext* redis;
    std::mutex redis_mutex;

public:
    HealthHandler(redisContext* r) : redis(r) {}

    bool handleGet(CivetServer *server, struct mg_connection *conn) {
        std::lock_guard<std::mutex> lock(redis_mutex);
        if (redis && redis->err) {
            mg_printf(conn, "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n\r\nRedis unavailable");
            return true;
        }

        redisReply* reply = (redisReply*)redisCommand(redis, "PING");
        proxy_redis_requests_counter.Increment();
        if (reply && reply->type == REDIS_REPLY_STATUS) {
            mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nOK");
        } else {
            proxy_redis_errors_counter.Increment();
            mg_printf(conn, "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n\r\nRedis unavailable");
        }
        if (reply) freeReplyObject(reply);
        return true;
    }
};

class StatsHandler : public CivetHandler {
private:
    redisContext* redis;
    std::mutex redis_mutex;

public:
    StatsHandler(redisContext* r) : redis(r) {}

    bool handleGet(CivetServer *server, struct mg_connection *conn) {
        std::lock_guard<std::mutex> lock(redis_mutex);
        if (!redis || redis->err) {
            mg_printf(conn, "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n\r\nRedis unavailable");
            return true;
        }

        redisReply* writes_reply = (redisReply*)redisCommand(redis, "GET stats:redis_writes");
        proxy_redis_requests_counter.Increment();
        if (!(writes_reply && writes_reply->type == REDIS_REPLY_STRING)) {
            proxy_redis_errors_counter.Increment();
        }

        redisReply* reads_reply = (redisReply*)redisCommand(redis, "GET stats:redis_reads");
        proxy_redis_requests_counter.Increment();
        if (!(reads_reply && reads_reply->type == REDIS_REPLY_STRING)) {
            proxy_redis_errors_counter.Increment();
        }

        long long writes = 0;
        long long reads = 0;

        if (writes_reply && writes_reply->type == REDIS_REPLY_STRING) {
            writes = atoll(writes_reply->str);
        }
        if (reads_reply && reads_reply->type == REDIS_REPLY_STRING) {
            reads = atoll(reads_reply->str);
        }

        Json::Value stats;
        stats["redis_writes"] = (Json::Int64)writes;
        stats["redis_reads"] = (Json::Int64)reads;

        Json::StreamWriterBuilder writer;
        std::string stats_json = Json::writeString(writer, stats);
        mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n%s", stats_json.c_str());

        if (writes_reply) freeReplyObject(writes_reply);
        if (reads_reply) freeReplyObject(reads_reply);
        return true;
    }
};

class RequestHandler : public CivetHandler {
private:
    redisContext* redis;
    std::mutex redis_mutex;
    bool use_sequential_id = true;
    long long request_id_counter = 0;

    std::string generate_uuid() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);
        std::stringstream ss;
        ss << std::hex;
        for (int i = 0; i < 32; i++) {
            ss << dis(gen);
        }
        return ss.str();
    }

    std::string generate_sequential_id() {
        std::lock_guard<std::mutex> lock(redis_mutex);
        request_id_counter++;
        return std::to_string(request_id_counter);
    }

    void save_counter() {
        std::lock_guard<std::mutex> lock(redis_mutex);
        redisReply* reply = (redisReply*)redisCommand(redis, "SET request_id_counter %lld", request_id_counter);
        proxy_redis_requests_counter.Increment();
        if (!(reply && reply->type == REDIS_REPLY_STATUS)) {
            proxy_redis_errors_counter.Increment();
        }
        if (reply) freeReplyObject(reply);
    }

public:
    RequestHandler(redisContext* r) : redis(r), request_id_counter(0) {
        // const char* env = std::getenv("USE_SEQUENTIAL_REQUEST_ID");
        // use_sequential_id = env && std::string(env) == "true";

        // Load counter from Redis
        std::lock_guard<std::mutex> lock(redis_mutex);
        redisReply* reply = (redisReply*)redisCommand(redis, "GET request_id_counter");
        proxy_redis_requests_counter.Increment();
        if (reply && reply->type == REDIS_REPLY_STRING) {
            request_id_counter = atoll(reply->str);
        } else if (reply && reply->type == REDIS_REPLY_NIL) {
            request_id_counter = 0;
        } else {
            proxy_redis_errors_counter.Increment();
            std::cerr << "Failed to load request_id_counter from Redis, starting from 0" << std::endl;
            request_id_counter = 0;
        }
        if (reply) freeReplyObject(reply);
    }

    ~RequestHandler() {
        save_counter();
    }

    bool handleGet(CivetServer *server, struct mg_connection *conn) {
        return handle_request(server, conn, "GET", "");
    }

    bool handlePost(CivetServer *server, struct mg_connection *conn) {
    const struct mg_request_info *req_info = mg_get_request_info(conn);
    std::string body;

    if (req_info->content_length > 0) {
        body.resize(req_info->content_length);
        const int read_len = mg_read(conn, &body[0], req_info->content_length);
        if (read_len >= 0) {
            body.resize(read_len);
        } else {
            body.clear();
        }
    }
    proxy_bytes_received_counter.Increment(body.size());
    return handle_request(server, conn, "POST", std::move(body));
    }

    bool handle_request(CivetServer *server, struct mg_connection *conn, const std::string& method, const std::string& body = "") {
        auto start_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        proxy_client_requests_counter.Increment();

        const struct mg_request_info *req_info = mg_get_request_info(conn);
        const std::string path = req_info->request_uri ? req_info->request_uri : "/";

        const std::string request_id = use_sequential_id ? generate_sequential_id() : generate_uuid();

        // Extract trace context from W3C traceparent header, or generate new ones for tracing propagation

        std::string trace_id;
        std::string span_id;
        std::string parent_id;
        std::string traceparent_header;
        bool sampled = true;

        const char* traceparent_raw = CivetServer::getHeader(conn, "traceparent");
        std::string incoming_span_id;
        if (traceparent_raw && tracer && tracer->parse_traceparent(traceparent_raw, trace_id, incoming_span_id, sampled)) {
            // Successfully parsed traceparent, use existing trace context
            parent_id = incoming_span_id;
            span_id = tracer->generate_span_id();  // Generate new span ID for proxy
            traceparent_header = tracer->generate_traceparent(trace_id, span_id, sampled);
            std::cout << "Successfully parsed traceparent, use existing trace context:" << traceparent_header;
        } else {
            // Generate new trace context
            trace_id = tracer->generate_trace_id();
            span_id = tracer->generate_span_id();
            parent_id = "";
            traceparent_header = tracer->generate_traceparent(trace_id, span_id, sampled);
            std::cout << "Generate new trace context:" << traceparent_header << std::endl;;
        }
        // Prepare request data for Redis
        Json::Value request_data;
        request_data["id"] = request_id;
        request_data["method"] = method;
        request_data["path"] = path;
        if (!body.empty()) {
            request_data["body"] = body;
        }
        request_data["traceparent"] = traceparent_header;
        std::cout << " traceparent:" << traceparent_header << std::endl << "request_data: " << request_data << std::endl;

        bool redis_push_success = false;

        // Push to Redis queue and measure timing
        auto redis_push_start = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        if (redis && !redis->err) {
            std::lock_guard<std::mutex> lock(redis_mutex);
            Json::StreamWriterBuilder writer;
            std::string request_json = Json::writeString(writer, request_data);
            redisReply* reply = (redisReply*)redisCommand(redis, "RPUSH http:requests %s", request_json.c_str());
            proxy_redis_requests_counter.Increment();
            if (reply && reply->type == REDIS_REPLY_INTEGER) {
                redis_push_success = true;
            } else {
                proxy_redis_errors_counter.Increment();
            }
            if (reply) freeReplyObject(reply);
            // Increment write counter
            redisReply* incr_reply = (redisReply*)redisCommand(redis, "INCR stats:redis_writes");
            proxy_redis_requests_counter.Increment();
            if (!(incr_reply && incr_reply->type == REDIS_REPLY_INTEGER)) {
                proxy_redis_errors_counter.Increment();
            }
            if (incr_reply) freeReplyObject(incr_reply);

        } else {
            proxy_redis_errors_counter.Increment();
        }

        auto redis_push_end = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        if (tracer) {
            // Generate unique span ID for Redis push operation
            std::string redis_span_id = tracer->generate_span_id();
            tracer->log_request(method, path, 200, redis_push_start, redis_push_end, "Redis-push", request_id, trace_id, redis_span_id, span_id);
        }

        if (!redis_push_success) {
            proxy_client_errors_counter.Increment();
        }

        // Wait for response (simplified - in real implementation would poll Redis)
        // std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Send response
        Json::Value response;
        response["request_id"] = request_id;
        response["traceparent"] = traceparent_header;

        // Получаем timestamp в микросекундах UTC (стандарт для OpenObserve)
        const auto now = std::chrono::system_clock::now();
        const auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()
        ).count();
        response["timestamp"] = (Json::Int64)timestamp_us; // ← микросекунды UTC

        std::cout << "response" << response << std::endl;

        Json::StreamWriterBuilder writer;
        std::string response_json = Json::writeString(writer, response);
        proxy_bytes_sent_counter.Increment(response_json.size());
        
        // Build response headers including W3C Trace Context
        std::string response_headers = "HTTP/1.1 200 OK\r\nContent-Type: application/json";
        if (!traceparent_header.empty()) {
            response_headers += "\r\ntraceparent: " + traceparent_header;
        }
        response_headers += "\r\n\r\n";

        mg_printf(conn, "%s%s", response_headers.c_str(), response_json.c_str());
        
        /*
        // Build response headers including trace information
        std::string response_headers = "HTTP/1.1 200 OK\r\nContent-Type: application/json";
        if (!trace_id.empty()) {
            response_headers += "\r\ntrace_id: " + trace_id;
        }
        if (!span_id.empty()) {
            response_headers += "\r\nspan_id: " + span_id;
        }
        response_headers += "\r\n\r\n";

        mg_printf(conn, "%s%s", response_headers.c_str(), response_json.c_str());
        */

        // Send tracing span for the complete proxy operation
        if (tracer) {
            auto end_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();

            tracer->log_request(method, path, 200, start_us, end_us, "l2-proxy", request_id, trace_id, span_id, parent_id);
        }

        return true;
    }
};

void run_proxy(const std::string& redis_host, int redis_port) {

    redisContext* redis = redisConnect(redis_host.c_str(), redis_port);
    if (redis == NULL || redis->err) {
        std::cerr << "Redis connection error: " << (redis ? redis->errstr : "can't allocate redis context") << std::endl;
        return;
    }

    HealthHandler health_handler(redis);
    RequestHandler request_handler(redis);
    StatsHandler stats_handler(redis);

    // Read number of threads from environment variable
    const char* num_threads_env = std::getenv(NUM_THREADS_ENV);
    const std::string num_threads = num_threads_env ? std::string(num_threads_env) : "32";

	std::vector<std::string> cpp_options;
	cpp_options.push_back("listening_ports");
	cpp_options.push_back("8888");
	cpp_options.push_back("num_threads");
	cpp_options.push_back(num_threads);
	cpp_options.push_back("enable_directory_listing");
	cpp_options.push_back("no");
//      cpp_options.push_back("max_connections");
//      cpp_options.push_back("1024");
	cpp_options.push_back("request_timeout_ms");
	cpp_options.push_back("30000");

    // Start Prometheus exposer
    prometheus::Exposer exposer{"0.0.0.0:9090"};
    exposer.RegisterCollectable(proxy_registry);

    try {
        CivetServer server(cpp_options);
        server.addHandler("/health", &health_handler);
        server.addHandler("/stats", &stats_handler);
        server.addHandler("/", &request_handler);

        std::cout << "C++ DMZ Proxy listening on http://0.0.0.0:8888" << std::endl;
        std::cout << "Prometheus metrics available at http://0.0.0.0:9090/metrics" << std::endl;

        while (!shutdown_flag)
        {
            sleep(1);
        }
    }
    catch (CivetException& e)
    {
        std::cout << "CivetException:" << e.what() << std::endl;
    }

    redisFree(redis);
}

// Prometheus registry for worker
std::shared_ptr<prometheus::Registry> worker_registry = std::make_shared<prometheus::Registry>();

// Prometheus counters for worker
auto& l2_worker_requests_processed_total = prometheus::BuildCounter()
    .Name("l2_worker_requests_processed_total")
    .Help("Total number of requests processed by L2 worker")
    .Register(*worker_registry);

auto& l2_worker_redis_operations_total = prometheus::BuildCounter()
    .Name("l2_worker_redis_operations_total")
    .Help("Total number of Redis operations performed by L2 worker")
    .Register(*worker_registry);

auto& l2_worker_l2_calls_total = prometheus::BuildCounter()
    .Name("l2_worker_l2_calls_total")
    .Help("Total number of L2 server calls made by worker")
    .Register(*worker_registry);

auto& l2_worker_redis_errors_total = prometheus::BuildCounter()
    .Name("l2_worker_redis_errors_total")
    .Help("Total number of Redis operation errors in L2 worker")
    .Register(*worker_registry);

auto& l2_worker_l2_errors_total = prometheus::BuildCounter()
    .Name("l2_worker_l2_errors_total")
    .Help("Total number of L2 server call errors in worker")
    .Register(*worker_registry);

auto& l2_worker_bytes_received_total = prometheus::BuildCounter()
    .Name("l2_worker_bytes_received_total")
    .Help("Total number of bytes received from Redis")
    .Register(*worker_registry);

auto& l2_worker_bytes_sent_total = prometheus::BuildCounter()
    .Name("l2_worker_bytes_sent_total")
    .Help("Total number of bytes sent to Redis")
    .Register(*worker_registry);

// Counter instances for worker
prometheus::Counter& worker_requests_processed_counter = l2_worker_requests_processed_total.Add({});
prometheus::Counter& worker_redis_operations_counter = l2_worker_redis_operations_total.Add({});
prometheus::Counter& worker_l2_calls_counter = l2_worker_l2_calls_total.Add({});
prometheus::Counter& worker_redis_errors_counter = l2_worker_redis_errors_total.Add({});
prometheus::Counter& worker_l2_errors_counter = l2_worker_l2_errors_total.Add({});
prometheus::Counter& worker_bytes_received_counter = l2_worker_bytes_received_total.Add({});
prometheus::Counter& worker_bytes_sent_counter = l2_worker_bytes_sent_total.Add({});

class L2Worker {
private:
    redisContext* redis = nullptr;
    CURL* curl = nullptr;
    std::string l2_server_url;

    static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* response) {
        const size_t total_size = size * nmemb;
        response->append((char*)contents, total_size);
        return total_size;
    }

public:
    L2Worker(const std::string& redis_host, int redis_port, const std::string& server_url) 
        : l2_server_url(server_url) {

        curl = curl_easy_init();
        if (!curl) {
            std::cerr << "CURL initialization failed" << std::endl;
            exit(1);
        }
        
        redis = redisConnect(redis_host.c_str(), redis_port);
        if (redis == NULL || redis->err) {
            std::cerr << "Redis connection error: " << (redis ? redis->errstr : "can't allocate redis context") << std::endl;
            exit(1);
        }

    }

    ~L2Worker() {
        if (redis) {
            redisFree(redis);
        }
        if (curl) {
            curl_easy_cleanup(curl);
        }
    }

    std::string call_l2_server(const std::string& path, const std::string& body, const std::string& traceparent = "") {
        worker_l2_calls_counter.Increment();

        const std::string url = l2_server_url + path;
        std::string response_string;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        // Add W3C Trace Context header if provided
        if (!traceparent.empty()) {
            std::string traceparent_header = "traceparent: " + traceparent;
            headers = curl_slist_append(headers, traceparent_header.c_str());
        }

        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.length());
        }

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        const CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            worker_l2_errors_counter.Increment();
            curl_slist_free_all(headers);
            return "{\"error\": \"Failed to call L2 server: " + std::string(curl_easy_strerror(res)) + "\"}";
        }

        curl_slist_free_all(headers);
        return response_string;
    }

    void process_request_from_redis(const std::string& request_json) {
        auto start_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        worker_requests_processed_counter.Increment();
        worker_bytes_received_counter.Increment(request_json.size());

        Json::Value request_data;
        Json::Reader reader;

        if (!reader.parse(request_json, request_data)) {
            std::cerr << "Failed to parse JSON request" << std::endl;
            return;
        }

        const std::string method = request_data["method"].asString();
        if (method != "POST") {
            std::cout << "Skipping non-POST request: " << method << std::endl;
            return;
        }

        const std::string request_id = request_data["id"].asString();
        const std::string path = request_data["path"].asString();
        const std::string body = request_data["body"].asString();

        // Extract trace and span IDs for propagation from traceparent header
        std::string parent_trace_id;
        std::string parent_span_id;
        bool sampled = true;
        const std::string traceparent = request_data.isMember("traceparent") ? request_data["traceparent"].asString() : "";
        if (!traceparent.empty() && tracer && tracer->parse_traceparent(traceparent.c_str(), parent_trace_id, parent_span_id, sampled)) {
            std::cout << "Successfully parsed traceparent" << std::endl;
        } else {
            parent_trace_id = "";
            parent_span_id = "";
        }

        std::cout << "Processing POST request: " << request_id << " traceparent:" << traceparent << " path: " << path <<  "body:" << body << std::endl;

        // Generate child span for L2 call and create traceparent header
        std::string traceparent_header;
        std::string child_span_id;
        if (tracer && !parent_trace_id.empty()) {
            child_span_id = tracer->generate_span_id();
            traceparent_header = tracer->generate_traceparent(parent_trace_id, child_span_id, true);
            std::cout << "traceparent_header: " << traceparent_header << std::endl;
        }

        // Call L2 server with trace context
        const std::string l2_response = call_l2_server(path, body, traceparent_header);

        // Prepare response for Redis
        Json::Value response_data;
        response_data["status_code"] = 200;
        response_data["headers"]["Content-Type"] = "application/json";

        Json::Value response_body;
        response_body["request_id"] = request_id;
        response_body["l2_response"] = l2_response;
        if(!traceparent_header.empty())
          {
            response_body["traceparent"] = traceparent_header;
            // TODO - add http header
          } 
        
        // Получаем timestamp в микросекундах UTC (стандарт для OpenObserve)
        const auto now = std::chrono::system_clock::now();
        const auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()
        ).count();
        response_body["timestamp"] = (Json::Int64)timestamp_us; // ← микросекунды UTC

        response_data["body"] = response_body;

        // Store response in Redis
        Json::StreamWriterBuilder writer;
        const std::string response_str = Json::writeString(writer, response_data);
        worker_bytes_sent_counter.Increment(response_str.size());

        if (tracer) {
            const auto end_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            tracer->log_request(method, path, 200, start_us, end_us, "l2-worker-redis-set-response", request_id, parent_trace_id, child_span_id, parent_span_id);
            start_us = end_us;
        }

        worker_redis_operations_counter.Increment();
        redisReply* reply = (redisReply*)redisCommand(redis, "SETEX http:response:%s 60 %s",
                                                      request_id.c_str(), response_str.c_str());
        if (reply && reply->type != REDIS_REPLY_STATUS) {
            worker_redis_errors_counter.Increment();
        }
        if (reply) freeReplyObject(reply);

        if (tracer) {
            const auto end_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            const std::string worker_span_id = tracer->generate_span_id();
            tracer->log_request(method, path, 200, start_us, end_us, "l2-worker", request_id, parent_trace_id, worker_span_id, parent_span_id);
        }
    }

    void run() {
        std::cout << "C++ L2 Worker started. Waiting for requests..." << std::endl;

        while (!shutdown_flag) {
            worker_redis_operations_counter.Increment();
            redisReply* reply = (redisReply*)redisCommand(redis, "BLPOP http:requests 10");

            if (reply && reply->type == REDIS_REPLY_ARRAY && reply->elements == 2) {
                const std::string request_json = reply->element[1]->str;
                process_request_from_redis(request_json);
                // Increment read counter
                worker_redis_operations_counter.Increment();
                redisReply* incr_reply = (redisReply*)redisCommand(redis, "INCR stats:redis_reads");
                if (!(incr_reply && incr_reply->type == REDIS_REPLY_INTEGER)) {
                    worker_redis_errors_counter.Increment();
                }
                if (incr_reply) freeReplyObject(incr_reply);
            } else if (reply && reply->type != REDIS_REPLY_ARRAY) {
                worker_redis_errors_counter.Increment();
            }

            if (reply) freeReplyObject(reply);
            // std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "Shutting down gracefully..." << std::endl;
    }
};

void run_worker(const std::string& redis_host, int redis_port, const std::string& l2_server_url) {

    // Start Prometheus exposer
    prometheus::Exposer exposer{"0.0.0.0:9091"};
    exposer.RegisterCollectable(worker_registry);

    L2Worker worker(redis_host, redis_port, l2_server_url);

    std::cout << "C++ L2 Worker Prometheus metrics available at http://0.0.0.0:9091/metrics" << std::endl;
    worker.run();
}

void signal_handler(int signum) {
    std::cout << "Received signal " << signum << ", exiting..." << std::endl;
    shutdown_flag = true;
}

int main() {
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    // Get configuration from environment variables
    const char* redis_host_env = std::getenv("REDIS_HOST");
    const std::string redis_host = redis_host_env ? std::string(redis_host_env) : "valkey";

    const char* redis_port_env = std::getenv("REDIS_PORT");
    const int redis_port = redis_port_env ? std::stoi(std::string(redis_port_env)) : 6379;

    const char* l2_server_url_env = std::getenv("L2_SERVER_URL");
    const std::string l2_server_url = l2_server_url_env ? std::string(l2_server_url_env) : "http://l2-server:3000";

    // Initialize Tracer
    init_tracer();

    const char* mode = std::getenv(MODE_ENV);
    if (!mode) {
        std::cerr << "Environment variable " << MODE_ENV << " not set. Please set MODE=proxy or MODE=worker" << std::endl;
        return 1;
    }

    const std::string mode_str = mode;
    if (mode_str == "proxy") {
        std::cout << "Starting in proxy mode" << std::endl;
        run_proxy(redis_host, redis_port);
    } else if (mode_str == "worker") {
        std::cout << "Starting in worker mode" << std::endl;
        run_worker(redis_host, redis_port, l2_server_url);
    } else {
        std::cerr << "Invalid mode: " << mode_str << ". Use proxy or worker" << std::endl;
        return 1;
    }

    return 0;
}
