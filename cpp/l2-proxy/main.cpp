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


#include <prometheus/registry.h>
#include <prometheus/exposer.h>
#include <prometheus/counter.h>

#include "nlohmann/json.hpp"

using json = nlohmann::json;

#if !defined(USE_JAEGER)
#define USE_JAEGER
#endif

#include "trace_loger.hpp"


using TracerType = JaegerLogger;


std::atomic<bool> shutdown_flag(false);

const char* MODE_ENV = "MODE";
const char* NUM_THREADS_ENV = "NUM_THREADS";

std::string g_mode_proxy_worker;

std::unique_ptr<JaegerLogger> tracer;

void init_tracer() {
    const char* jaeger_url = std::getenv("JAEGER_URL");

    if (!jaeger_url) {
        std::cerr << "JAEGER_URL not set, tracing disabled" << std::endl;
        return;
    }

    const std::string endpoint = std::string(jaeger_url);

    tracer = std::make_unique<JaegerLogger>(endpoint);
    std::cout << "JAEGER_URL set, tracing enabled: " << endpoint << std::endl;
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

        json stats;
        stats["redis_writes"] = writes;
        stats["redis_reads"] = reads;

        std::string stats_json = stats.dump();
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

    std::tuple<std::string, std::string, std::string, std::string, bool> handle_trace_context(mg_connection* conn) {
        std::string trace_id, span_id, parent_id, traceparent_header;
        bool sampled = true;

        const char* traceparent_raw = CivetServer::getHeader(conn, "traceparent");
        std::string incoming_span_id;
        if (traceparent_raw && tracer && tracer->parse_traceparent(traceparent_raw, trace_id, incoming_span_id, sampled)) {
            parent_id = incoming_span_id;
            span_id = tracer->generate_span_id();
            traceparent_header = tracer->generate_traceparent(trace_id, span_id, sampled);
            std::cout << "Successfully parsed traceparent, use existing trace context: " << traceparent_header << std::endl;
        } else {
            trace_id = tracer->generate_trace_id();
            span_id = tracer->generate_span_id();
            parent_id = "";
            traceparent_header = tracer->generate_traceparent(trace_id, span_id, sampled);
            std::cout << "Generate new trace context: " << traceparent_header << std::endl;
        }
        return {trace_id, span_id, parent_id, traceparent_header, sampled};
    }

    bool push_to_redis(const json& request_data, const std::string& trace_id, const std::string& span_id) {
        auto redis_push_start = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        bool success = false;
        if (redis && !redis->err) {
            std::lock_guard<std::mutex> lock(redis_mutex);
            std::string request_json = request_data.dump();
            redisReply* reply = (redisReply*)redisCommand(redis, "RPUSH http:requests %s", request_json.c_str());
            proxy_redis_requests_counter.Increment();
            if (reply && reply->type == REDIS_REPLY_INTEGER) {
                success = true;
            } else {
                proxy_redis_errors_counter.Increment();
            }
            if (reply) freeReplyObject(reply);
#ifdef USE_REDIS_INCR
            // Increment write counter
            redisReply* incr_reply = (redisReply*)redisCommand(redis, "INCR stats:redis_writes");
            proxy_redis_requests_counter.Increment();
            if (!(incr_reply && incr_reply->type == REDIS_REPLY_INTEGER)) {
                proxy_redis_errors_counter.Increment();
            }
            if (incr_reply) freeReplyObject(incr_reply);
#endif
        } else {
            proxy_redis_errors_counter.Increment();
        }

        auto redis_push_end = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        if (tracer) {
            std::string redis_span_id = tracer->generate_span_id();
            tracer->log_request("RPUSH", "http:requests", 200, redis_push_start, redis_push_end, "l2-proxy-" + g_mode_proxy_worker, request_data["id"], trace_id, redis_span_id, span_id);
        }

        return success;
    }

    std::string poll_for_response(const std::string& request_id, int timeout_seconds = 30) {
        auto start_time = std::chrono::steady_clock::now();
        std::string response_key = "http:response:" + request_id;

        while (true) {
            {
                std::lock_guard<std::mutex> lock(redis_mutex);
                redisReply* reply = (redisReply*)redisCommand(redis, "GET %s", response_key.c_str());
                proxy_redis_requests_counter.Increment();

                if (reply && reply->type == REDIS_REPLY_STRING) {
                    std::string response_data = reply->str;
                    freeReplyObject(reply);
                    return response_data;
                }

                if (reply) freeReplyObject(reply);
            }

            // Check timeout
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > timeout_seconds) {
                break;
            }

            // Sleep for a short time before polling again
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        return ""; // Timeout or error
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

        auto [trace_id, span_id, parent_id, traceparent_header, sampled] = handle_trace_context(conn);
        // Prepare request data for Redis
        json request_data = {
            {"id", request_id},
            {"method", method},
            {"path", path},
            {"traceparent", traceparent_header}
        };
        if (!body.empty()) {
            request_data["body"] = body;
        }
        std::cout << " traceparent: " << traceparent_header << std::endl << "request_data: " << request_data.dump() << std::endl;

        bool redis_push_success = push_to_redis(request_data, trace_id, span_id);
        if (!redis_push_success) {
            proxy_client_errors_counter.Increment();
            mg_printf(conn, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n\r\n{\"error\": \"Failed to queue request\"}");
            return true;
        }

        // Poll for response from Redis
        std::string response_data_str = poll_for_response(request_id, 30); // 30 second timeout

        if (response_data_str.empty()) {
            proxy_client_errors_counter.Increment();
            std::cout << "Timeout waiting for response for request_id: " << request_id << std::endl;
            mg_printf(conn, "HTTP/1.1 504 Gateway Timeout\r\nContent-Type: application/json\r\n\r\n{\"error\": \"Timeout waiting for response\", \"request_id\": \"%s\"}", request_id.c_str());
            return true;
        }
        else
        {
            std::cout << "response_data_str: " <<  response_data_str << " for request_id: " << request_id << std::endl;
        }


        // Parse the response data from Redis
        json response_data;
        try {
            response_data = json::parse(response_data_str);
            std::cout << "[!] response_data_json: " <<  response_data << " for request_id: " << request_id << std::endl;
        } catch (const std::exception& e) {
            proxy_client_errors_counter.Increment();
            std::cerr << "Failed to parse response data from Redis: " << e.what() << std::endl;
            mg_printf(conn, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n\r\n{\"error\": \"Invalid response format\", \"request_id\": \"%s\"}", request_id.c_str());
            return true;
        }

        // Extract the actual response from l2-server
        std::string l2_response = response_data["body"]["l2_response"];
        int status_code = response_data["status_code"];

        std::cout << "Sending response for request_id: " << request_id << ", status: " << status_code << ", response_size: " << l2_response.size() << std::endl;

        proxy_bytes_sent_counter.Increment(l2_response.size());

        // Build response headers
        std::string response_headers = "HTTP/1.1 " + std::to_string(status_code) + " OK\r\nContent-Type: application/json";
        if (!traceparent_header.empty()) {
            response_headers += "\r\ntraceparent: " + traceparent_header;
        }
        response_headers += "\r\n\r\n";

        mg_printf(conn, "%s%s", response_headers.c_str(), l2_response.c_str());
        
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

            tracer->log_request(method, path, 200, start_us, end_us, "l2-proxy-" + g_mode_proxy_worker, request_id, trace_id, span_id, parent_id);
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

        std::cout << "Calling L2 server: URL=" << l2_server_url + path << ", body_size=" << body.size() << ", traceparent=" << traceparent << std::endl;

        // Extract trace context
        std::string trace_id;
        std::string parent_span_id;
        bool sampled = true;
        if (!traceparent.empty() && tracer && tracer->parse_traceparent(traceparent.c_str(), trace_id, parent_span_id, sampled)) {
            std::cout << "Successfully parsed traceparent for L2 call" << std::endl;
        } else {
            trace_id = "";
            parent_span_id = "";
        }

        // Generate span for L2 server call
        std::string span_id;
        if (tracer) {
            span_id = tracer->generate_span_id();            
        }

        auto start_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        const std::string url = l2_server_url + path;
        std::string response_string;
        long http_code = 0;

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

        // Get HTTP response code
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res != CURLE_OK) {
            worker_l2_errors_counter.Increment();
            std::cout << "L2 server call failed: " << curl_easy_strerror(res) << " http_code: "
             << http_code  << " path: " << path << std::endl;
            curl_slist_free_all(headers);

            // Log failed span
            if (tracer && !trace_id.empty()) {
                auto end_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();
                tracer->log_request("POST", url, 500, start_us, end_us, "l2-proxy-" + g_mode_proxy_worker + "-call-l2-server", "", trace_id, span_id, parent_span_id);
            }

            return "{\"error\": \"Failed to call L2 server: " + std::string(curl_easy_strerror(res)) + "\"}";
        }

        curl_slist_free_all(headers);
        std::cout << "L2 server call succeeded: response_size=" << response_string.size() << std::endl;

        // Log successful span
        if (tracer && !trace_id.empty()) {
            auto end_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            tracer->log_request("POST", url, static_cast<int>(http_code), start_us, end_us, "l2-proxy-" + g_mode_proxy_worker + "-call-l2-server", "", trace_id, span_id, parent_span_id);
        }

        return response_string;
    }

    void process_request_from_redis(const std::string& request_json) {
        auto start_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        worker_requests_processed_counter.Increment();
        worker_bytes_received_counter.Increment(request_json.size());

        json request_data;

        try {
            request_data = json::parse(request_json);
        } catch (const std::exception& e) {
            std::cerr << "Failed to parse JSON request: " << e.what() << std::endl;
            return;
        }

        const std::string method = request_data["method"];
        if (method != "POST") {
            std::cout << "Skipping non-POST request: " << method << std::endl;
            return;
        }

        const std::string request_id = request_data["id"];
        const std::string path = request_data["path"];
        const std::string body = request_data["body"];

        // Extract trace and span IDs for propagation from traceparent header
        std::string parent_trace_id;
        std::string parent_span_id;
        bool sampled = true;
        const std::string traceparent = request_data.contains("traceparent") ? request_data["traceparent"] : "";
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

        // Получаем timestamp в микросекундах UTC (стандарт для OpenObserve)
        const auto now = std::chrono::system_clock::now();
        const auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()
        ).count();

        // Prepare response for Redis
        json response_data = {
            {"status_code", 200},
            {"headers", {{"Content-Type", "application/json"}}},
            {"body", {
                {"request_id", request_id},
                {"l2_response", l2_response},
                {"timestamp", timestamp_us}
            }}
        };

        if(!traceparent_header.empty()) {
            response_data["body"]["traceparent"] = traceparent_header; // TODO - headers
        }

        // Store response in Redis
        const std::string response_str = response_data.dump();
        worker_bytes_sent_counter.Increment(response_str.size());

        if (tracer) {
            const auto end_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            tracer->log_request(method, path, 200, start_us, end_us, "l2-proxy-" + g_mode_proxy_worker, request_id, parent_trace_id, child_span_id, parent_span_id);
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
            tracer->log_request(method, path, 200, start_us, end_us, "l2-proxy-" + g_mode_proxy_worker, request_id, parent_trace_id, worker_span_id, parent_span_id);
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
#ifdef USE_REDIS_INCR            
                // Increment read counter
                worker_redis_operations_counter.Increment();
                redisReply* incr_reply = (redisReply*)redisCommand(redis, "INCR stats:redis_reads");
                if (!(incr_reply && incr_reply->type == REDIS_REPLY_INTEGER)) {
                    worker_redis_errors_counter.Increment();
                }
                if (incr_reply) freeReplyObject(incr_reply);
#endif
            } else if (reply && reply->type != REDIS_REPLY_ARRAY) {
                worker_redis_errors_counter.Increment();
            }

            if (reply) freeReplyObject(reply);
            // TODO std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    const std::string l2_server_url = l2_server_url_env ? std::string(l2_server_url_env) : "http://l2-server:8080";

    // Initialize Tracer
    init_tracer();

    const char* mode = std::getenv(MODE_ENV);
    if (!mode) {
        std::cerr << "Environment variable " << MODE_ENV << " not set. Please set MODE=proxy or MODE=worker" << std::endl;
        return 1;
    }

    g_mode_proxy_worker = std::string(mode);
    if (g_mode_proxy_worker == "proxy") {
        std::cout << "Starting in proxy mode" << std::endl;
        run_proxy(redis_host, redis_port);
    } else if (g_mode_proxy_worker == "worker") {
        std::cout << "Starting in worker mode" << std::endl;
        run_worker(redis_host, redis_port, l2_server_url);
    } else {
        std::cerr << "Invalid mode: " << g_mode_proxy_worker << ". Use proxy or worker" << std::endl;
        return 1;
    }

    return 0;
}
