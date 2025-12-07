FROM ubuntu:24.04 AS builder

ARG USE_JAEGER=OFF

RUN apt-get update && apt-get install -y \
    g++ \
    cmake \
    pkg-config \
    libhiredis-dev \
    libssl-dev \
    libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY CMakeLists.txt main.cpp trace_loger.hpp ./
COPY civetweb/ civetweb/
# COPY jsoncpp/ jsoncpp/
COPY nlohmann/ nlohmann
COPY prometheus-cpp/ prometheus-cpp/
RUN mkdir build && cd build && cmake -DUSE_JAEGER=${USE_JAEGER} .. && make

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y \
    libhiredis1.1.0 \
    libssl3t64 \
    libcurl4t64 \
    chrony \
    && rm -rf /var/lib/apt/lists/*

RUN echo "makestep 0.001 -1" >> /etc/chrony/chrony.conf

WORKDIR /root/
COPY --from=builder /app/build/l2-proxy .
EXPOSE 8888 9090 9091
CMD ["sh", "-c", "chronyd -s & ./l2-proxy"]
