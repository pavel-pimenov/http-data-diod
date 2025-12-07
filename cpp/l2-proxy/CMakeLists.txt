cmake_minimum_required(VERSION 3.14)
project(l2-proxy)

# Options for tracing backends
option(USE_JAEGER "Use Jaeger tracing" ON)

# Find required packages
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# Find hiredis manually
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h REQUIRED)
find_library(HIREDIS_LIBRARY hiredis REQUIRED)

 
# Add executable
add_executable(l2-proxy
 main.cpp
 jsoncpp/jsoncpp.cpp
 prometheus-cpp/pull/src/basic_auth.cc
 prometheus-cpp/pull/src/endpoint.cc
 prometheus-cpp/pull/src/exposer.cc
 prometheus-cpp/pull/src/handler.cc
 prometheus-cpp/pull/src/metrics_collector.cc
 prometheus-cpp/core/src/check_names.cc
 prometheus-cpp/core/src/counter.cc
 prometheus-cpp/core/src/family.cc
 prometheus-cpp/core/src/gauge.cc
 prometheus-cpp/core/src/histogram.cc
 prometheus-cpp/core/src/info.cc
 prometheus-cpp/core/src/registry.cc
 prometheus-cpp/core/src/serializer.cc
 prometheus-cpp/core/src/summary.cc
 prometheus-cpp/core/src/text_serializer.cc
 prometheus-cpp/core/src/detail/builder.cc
 prometheus-cpp/core/src/detail/ckms_quantiles.cc
 prometheus-cpp/core/src/detail/time_window_quantiles.cc
 prometheus-cpp/core/src/detail/utils.cc
 civetweb/CivetServer.cpp
 civetweb/civetweb.c
)

# Include directories
target_include_directories(l2-proxy PRIVATE ${HIREDIS_INCLUDE_DIR})
target_include_directories(l2-proxy PRIVATE ${SOURCE_DIR}/include)

# Link libraries
target_link_libraries(l2-proxy PRIVATE ${HIREDIS_LIBRARY} OpenSSL::SSL OpenSSL::Crypto CURL::libcurl)

# Set compiler definitions based on options
target_compile_definitions(l2-proxy PRIVATE USE_JAEGER)

include_directories(prometheus-cpp/core/include)
include_directories(prometheus-cpp/include)
include_directories(prometheus-cpp/util/include)
include_directories(prometheus-cpp/pull/include)
include_directories(civetweb)
include_directories(jsoncpp)
include_directories(nlohmann)

#include_directories(sqlite)

target_link_libraries(l2-proxy PRIVATE ${HIREDIS_LIBRARY})
