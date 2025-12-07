#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <curl/curl.h>
#include <iostream>
#include "nlohmann/json.hpp"

#ifdef USE_JAEGER
class JaegerLogger
{

public:
    JaegerLogger(const std::string &endpoint)
        : jaeger_url(endpoint)
    {
        curl_global_init(CURL_GLOBAL_ALL); // Инициализация libcurl (вызывать один раз в программе)
    }

    ~JaegerLogger()
    {
        // curl_global_cleanup(); // Вызывать только при завершении всей программы!
    }

    std::string generate_trace_id()
    {
        return random_hex(32);
    }

    std::string generate_span_id()
    {
        return random_hex(16);
    }

    // W3C Trace Context helpers
    std::string generate_traceparent(const std::string &trace_id, const std::string &span_id, bool sampled = true)
    {
        std::stringstream ss;
        ss << "00-" << trace_id << "-" << span_id << "-" << (sampled ? "01" : "00");
        return ss.str();
    }

    // Parse traceparent header: "00-{trace-id}-{parent-id}-{flags}"
    bool parse_traceparent(const std::string &traceparent, std::string &trace_id, std::string &parent_span_id, bool &sampled)
    {
        if (traceparent.size() != 55 || traceparent.substr(0, 3) != "00-")
        {
            return false;
        }

        // Extract trace-id (32 chars after "00-")
        trace_id = traceparent.substr(3, 32);
        // Extract parent-id (16 chars after trace-id and "-")
        parent_span_id = traceparent.substr(36, 16);
        // Extract flags (2 chars at end)
        std::string flags = traceparent.substr(53, 2);

        sampled = (flags == "01");

        // Basic validation - check if hex
        for (char c : trace_id + parent_span_id + flags)
        {
            if (!std::isxdigit(c))
            {
                return false;
            }
        }

        return true;
    }

    void send_span(
        const std::string &trace_id,
        const std::string &span_id,
        const std::string &parent_span_id,
        const std::string &name,
        uint64_t start_us,
        uint64_t end_us,
        const std::string &service_name,
        const nlohmann::json &attributes = {})
    {
        CURL *curl = curl_easy_init();
        if (!curl)
        {
            std::cerr << "Failed to initialize curl for jaeger trace send\n";
            return;
        }

        // Настройка параметров
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 0L);
        curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 0L);

        // Преобразуем все значения attributes в строки (требование Zipkin)
        nlohmann::json tags;
        for (auto &el : attributes.items())
        {
            if (el.value().is_string())
            {
                tags[el.key()] = el.value();
            }
            else
            {
                // Числа, bool, null → сериализуем как строку без кавычек
                tags[el.key()] = el.value().dump();
            }
        }

        // Формируем span в Zipkin-совместимом формате
        nlohmann::json span = {
            {"id", span_id},
            {"traceId", trace_id},
            {"name", name},
            {"timestamp", start_us},
            {"duration", end_us - start_us},
            {"localEndpoint", {{"serviceName", service_name}}},
            {"tags", tags}};

        if (!parent_span_id.empty())
        {
            span["parentId"] = parent_span_id;
        }

        std::string payload_str = nlohmann::json::array({span}).dump();

        // Заголовки
        struct curl_slist *headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, jaeger_url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload_str.size()));

        // Отладка (опционально)
        std::cout << "JaegerLogger-body: " << payload_str << std::endl;

        // Отправка
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK)
        {
            std::cerr << "Jaeger trace send failed: " << curl_easy_strerror(res)
                      << " (" << jaeger_url << ")\n";
        }

        // Очистка
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    void log_request(
        const std::string &method,
        const std::string &url,
        int status_code,
        uint64_t start_us,
        uint64_t end_us,
        const std::string &service_name,
        const std::string &request_id = "",
        const std::string &trace_id = "",
        const std::string &span_id = "",
        const nlohmann::json &additional_attributes = {})
    {
        std::string actual_trace_id = trace_id.empty() ? generate_trace_id() : trace_id;
        std::string actual_span_id = span_id.empty() ? generate_span_id() : span_id;

        nlohmann::json attrs = {
            {"http.method", method},
            {"http.url", url},
            {"http.status_code", status_code}};

        if (!request_id.empty())
        {
            attrs["request.id"] = request_id;
        }

        for (auto &el : additional_attributes.items())
        {
            attrs[el.key()] = el.value();
        }

        send_span(
            actual_trace_id,
            actual_span_id,
            "",
            "HTTP " + method + " " + url,
            start_us,
            end_us,
            service_name,
            attrs);
    }

private:
    std::string jaeger_url;

    std::string random_hex(size_t len)
    {
        thread_local std::random_device rd;
        thread_local std::mt19937 gen(rd());
        thread_local std::uniform_int_distribution<> dis(0, 15);

        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (size_t i = 0; i < len; ++i)
        {
            ss << std::setw(1) << dis(gen);
        }
        return ss.str();
    }

    std::string random_hex_fast(size_t len)
    {
        thread_local std::random_device rd;
        thread_local std::mt19937_64 gen(rd());

        std::string res;
        res.reserve(len);

        while (res.size() < len)
        {
            uint64_t chunk = gen();
            for (int i = 0; i < 16 && res.size() < len; ++i)
            {
                res += "0123456789abcdef"[(chunk >> (i * 4)) & 0xF];
            }
        }
        return res;
    }
};

#endif
