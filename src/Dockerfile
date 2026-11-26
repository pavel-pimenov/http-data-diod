# syntax=docker/dockerfile:1

# Build location is configurable for an isolated (offline) contour:
#   - BASE_IMAGE:   the Ubuntu base image, fully resolved (optionally prefixed
#                   with the registry proxy), e.g. docker-registry.dp.nlmk.com/ubuntu:26.04
#   - REGISTRY_MIRROR: prefix prepended to the other external image references
#                   (FROM nats:2.15.2-alpine, oracle-libs, ...) so they are pulled
#                   through a registry proxy instead of Docker Hub.
#   - APT_MIRROR:   apt repository mirror for Ubuntu package installs (defaults
#                   to the proxy repo; set empty to use the distro defaults).
# All three default to the isolated-contour proxy endpoints and can be
# overridden per build to point at another registry/apt proxy.
ARG BASE_IMAGE=ubuntu:26.04
ARG REGISTRY_MIRROR=
ARG APT_MIRROR=
ARG GIT_SHA=unknown

# Shared base layer: run apt-get update once, reuse across all stages
FROM ${BASE_IMAGE} AS ubuntu-base
ARG DEBIAN_FRONTEND=noninteractive
ARG APT_MIRROR=
# DEBIAN_FRONTEND as ENV (not just ARG) so it is inherited by every derived
# stage and suppresses apt's debconf Dialog/Readline noise in the build log.
ENV DEBIAN_FRONTEND=${DEBIAN_FRONTEND:-noninteractive}
ENV APT_MIRROR=${APT_MIRROR}

# Configure apt mirror for closed network (when APT_MIRROR is set)
RUN set -eux; \
    if [ -n "${APT_MIRROR}" ]; then \
        . /etc/os-release; codename="${UBUNTU_CODENAME}"; \
        rm -f /etc/apt/sources.list; rm -f /etc/apt/sources.list.d/*; \
        { \
          echo "deb ${APT_MIRROR} ${codename} main restricted universe multiverse"; \
          echo "deb ${APT_MIRROR} ${codename}-updates main restricted universe multiverse"; \
          echo "deb ${APT_MIRROR} ${codename}-backports main restricted universe multiverse"; \
          echo "deb ${APT_MIRROR} ${codename}-security main restricted universe multiverse"; \
        } > /etc/apt/sources.list; \
        echo 'Acquire::Languages "none";' > /etc/apt/apt.conf.d/99no-translations; \
        echo 'APT::Install-Recommends "false";' > /etc/apt/apt.conf.d/99no-recommends; \
        echo 'Acquire::Retries "3";' > /etc/apt/apt.conf.d/99retries; \
        echo 'Acquire::https::Verify-Peer "false";' > /etc/apt/apt.conf.d/99insecure-artifactory; \
        echo 'Acquire::https::Verify-Host "false";' >> /etc/apt/apt.conf.d/99insecure-artifactory; \
    fi

# Runtime libraries for all runtime stages, installed once here so
# runtime-base and runtime need no apt of their own: one fewer
# apt-get update per cold build and no apt lock contention between
# the runtime and builder stages.
# Only libs the l2-proxy binary actually links (ldd) are kept: libssl3t64
# and libpq5 (plus their transitive deps libzstd1/zlib1g/libstdc++6). fmt and
# spdlog are vendored header-only (fmt 12.2.1 + spdlog 1.17.0), so neither
# libfmt10 nor libspdlog are runtime deps anymore.
RUN \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    libssl3t64 \
    libpq5 \
    ca-certificates \
    curl \
    && apt-get clean

# Main build stage: apt deps + NATS C client + application.
# NATS is built here (no separate deps-builder stage) so the pipeline runs
# apt-get update/install only ONCE. The old two-stage layout installed the
# same base packages (build-essential, cmake, ...) twice, wasting ~2-4 min
# of every cold build.
FROM ubuntu-base AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG GIT_SHA=unknown
ENV GIT_SHA=${GIT_SHA}

# Install build dependencies (stable layer — rarely changes)
# fmt and spdlog are vendored header-only (fmt/ and spdlog/ in the source
# tree), so neither libfmt-dev nor libspdlog-dev are installed; the older apt
# versions would otherwise override the vendored ones. libzstd-dev and
# pkg-config are not referenced by the application or either CMake build (zstd
# comes at link time from the libzstd1 runtime lib, pulled in by libssl3t64).
# libcli11-dev is gone: CLI11 was never included by any project source, so the
# package only added download/install time to every build.
# libgoogle-perftools-dev is NOT installed here: it is needed only by
# ENABLE_PROFILER=ON builds and is installed in its own conditional layer
# below (keeps the heavy base apt layer cached and out of the default image).
RUN \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    build-essential \
    ninja-build \
    ca-certificates \
    cmake \
    zlib1g-dev \
    libssl-dev \
    libpq-dev \
    && apt-get clean

# NOTE: ccache is intentionally NOT used in this deployment Dockerfile. It
# relies on a BuildKit `RUN --mount=type=cache,target=/root/.ccache` (present
# only in Dockerfile.test), which kaniko cannot run, so its cache never
# survives a kaniko rebuild — hits stay at 0% and it only adds a wasted 10 GiB
# layer. kaniko layer caching (--cache-repo) covers the apt/NATS/Oracle layers
# instead; the app build layer recompiles on every source change.

WORKDIR /app

# Build NATS C client (static) in its own COPY + RUN, BEFORE the app sources:
# the NATS layer then only rebuilds when nats/ changes, not on every commit
# that touches the application sources (COPY . . below comes after).
COPY nats ./nats

RUN \
    echo "=== Building NATS C client ===" && \
    cd /app/nats && \
    rm -rf build && mkdir build && cd build && \
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DNATS_BUILD_LIB_STATIC=ON \
          -DNATS_BUILD_LIB_SHARED=OFF \
          -DNATS_BUILD_EXAMPLES=OFF \
          -DNATS_BUILD_STREAMING=OFF \
          -DBUILD_TESTING=OFF .. && \
    ninja -j$(nproc) && \
    ninja install && \
    echo "=== NATS C client build complete ==="

# NATS installs as libnats_static.a; CMake's find_library looks for libnats.a
RUN ln -sf libnats_static.a /usr/local/lib/libnats.a 2>/dev/null || true

# Update library cache
RUN ldconfig

# Application sources last: changes here never invalidate the NATS layers.
COPY . .

# l2-proxy-version.h is build-generated and not tracked (rebuild-and-run.sh and
# CI regenerate it before the build). This fallback keeps a bare
# `docker compose build` on a fresh clone working (git SHA unavailable in the
# container -> generate_version.sh falls back to "unknown").
RUN if [ ! -f ./l2-proxy-version.h ]; then ./generate_version.sh > ./l2-proxy-version.h; fi

# Build application. For ASan builds use Debug + ENABLE_ASAN=ON.
# For profiler builds use ENABLE_PROFILER=ON (links tcmalloc + profiler).
# Use BuildKit cache for CMake and object files
ARG ENABLE_PROFILER=false
ARG ENABLE_ASAN=false
# Declared just before its use so changing it only invalidates the build RUN,
# not the apt-install layers above (an ARG in scope re-keys every later step).
# CACHE_BUST forces rebuild when changed: bump value or set via env to invalidate cache
ARG CACHE_BUST=1
# gperftools is needed only by ENABLE_PROFILER=ON builds (CMake links
# tcmalloc_and_profiler). It lives in its own layer so the default build
# neither downloads nor carries it, and so the heavy base apt layer above
# stays cached for both build modes.
RUN \
    if [ "$ENABLE_PROFILER" = "true" ]; then \
        apt-get update && \
        apt-get install -y --no-install-recommends libgoogle-perftools-dev && \
        apt-get clean; \
    fi
# The /app/build dir is recreated each build (kaniko keeps no cache mount), so
# the app compiles from scratch; CACHE_BUST lets CI force a clean rebuild by
# bumping the value or setting it via env, independent of kaniko layer cache.
RUN \
    echo "CACHE_BUST=${CACHE_BUST}" && \
    mkdir -p build && cd build && \
    if [ "$ENABLE_ASAN" = "true" ]; then \
        echo "Build mode: DEBUG + sanitizers (ASan, LSan via detect_leaks=1, UBSan) -fsanitize=address,undefined" && \
        cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug \
              -DENABLE_ASAN=ON \
              -DBUILD_TESTS=OFF ..; \
    elif [ "$ENABLE_PROFILER" = "true" ]; then \
        echo "Build mode: RelWithDebInfo + gperftools heap profiler" && \
        cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
              -DENABLE_PROFILER=ON \
              -DBUILD_TESTS=OFF ..; \
    else \
        echo "Build mode: RelWithDebInfo (optimized, no sanitizers)" && \
        cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
              -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++" \
              -DBUILD_TESTS=OFF ..; \
    fi && \
    echo "=== cmake configure done ===" && \
    _T1=$(date +%s) && \
    NINJA_JOBS=$(nproc); if [ "$ENABLE_ASAN" = "true" ]; then NINJA_JOBS=2; fi; echo "ninja jobs: $NINJA_JOBS (nproc $(nproc))" && \
    ninja -j$NINJA_JOBS && \
    _T2=$(date +%s) && \
    echo "=== build timing ===" && \
    echo "  compile:    $((_T2 - _T1))s" && \
    mkdir -p /app/out && cp /app/build/l2-proxy /app/out/l2-proxy

# PDF report stage: converts a markdown report (l2-db-spider output) into a
# landscape A4 PDF with a left-hand section tree (bookmarks/outline) for
# navigation. Built on the shared ubuntu-base (Ubuntu 26.04) so it gets the
# same apt-mirror handling and a modern Python (no 3.7 workarounds needed for
# reportlab). Only used via `docker build --target pdf-report` — it is NOT part
# of any runtime image.
FROM ubuntu-base AS pdf-report
ARG DEBIAN_FRONTEND=noninteractive

RUN \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    python3 \
    python3-pip \
    fonts-dejavu-core \
    && apt-get clean

# reportlab is the only dependency (pure-Python PDF generation; DejaVuSans
# provides the Cyrillic glyphs). No native build deps (PIL) are needed for
# text/table-only reports. pip installs through the corporate PyPI proxy
# (same artifactory as the host pip.conf): pypi.org is unreachable in a
# closed contour.
RUN python3 -m pip install --no-cache-dir --break-system-packages \
    --index-url https://repos.dp.nlmk.com/artifactory/api/pypi/pypi/simple \
    --trusted-host repos.dp.nlmk.com \
    reportlab==4.4.3

WORKDIR /work
COPY scripts/spider_report_pdf.py /usr/local/bin/spider_report_pdf.py
RUN chmod 755 /usr/local/bin/spider_report_pdf.py

# Usage: docker run --rm -v <abs>/spider-output:/data pdf-report \
#            /data/report.md /data/report.pdf
ENTRYPOINT ["python3", "/usr/local/bin/spider_report_pdf.py"]

# AI report service stage: a web server that shows the live progress of the
# l2-db-spider AI report generation (reads spider-output JSONs, sends them to an
# external model endpoint, writes report.md/report.pdf). Listens on SPIDER_AI_PORT
# (default 19096). Reuses pdf-report (has reportlab + Cyrillic font) and adds the
# web server script. NOT part of any runtime image.
FROM pdf-report AS spider-ai
COPY scripts/spider_report.py /usr/local/bin/spider_report.py
COPY scripts/spider-ai-server.py /usr/local/bin/spider-ai-server.py
RUN chmod 755 /usr/local/bin/spider_report.py /usr/local/bin/spider-ai-server.py \
    && mkdir -p /work/spider-output

EXPOSE 19096
# Usage: docker run --rm -p 19096:19096 -v <abs>/spider-output:/work/spider-output \
#            -e AI_API_URL=... -e AI_API_KEY=... -e AI_MODEL=... spider-ai \
#            --input-dir /work/spider-output --output-dir /work/spider-output/ai
ENTRYPOINT ["python3", "/usr/local/bin/spider-ai-server.py"]
CMD ["--input-dir", "/work/spider-output", "--output-dir", "/work/spider-output/ai"]

# heat-scraper: перебор плавок (GetHeatData) через прокси-цепочку l2 и сохранение
# ответа каждой плавки в отдельный JSON-файл. На базе pdf-report (python3 уже
# установлен); использует только встроенный urllib — внешних зависимостей нет.
# NOT part of any runtime image; запускается по demand (docker compose run).
FROM pdf-report AS heat-scraper
COPY scripts/heat-scraper.py /usr/local/bin/heat-scraper.py
RUN chmod 755 /usr/local/bin/heat-scraper.py
WORKDIR /work
ENTRYPOINT ["python3", "/usr/local/bin/heat-scraper.py"]

# heat-ai: веб-сервер прогресса AI-генерации отчёта по плавкам (GetHeatData).
# На базе pdf-report (python3 + reportlab + шрифты). Копирует heat_report.py
# (анализ плавок), heat-ai-server.py (веб-сервер) и spider_report.py
# (переиспользуется для конфига AI/чата), spider_report_pdf.py (PDF).
# Слушает на HEAT_AI_PORT (default 19097). NOT part of any runtime image.
FROM pdf-report AS heat-ai
COPY scripts/heat_report.py /usr/local/bin/heat_report.py
COPY scripts/heat-ai-server.py /usr/local/bin/heat-ai-server.py
COPY scripts/spider_report.py /usr/local/bin/spider_report.py
COPY scripts/spider_report_pdf.py /usr/local/bin/spider_report_pdf.py
RUN chmod 755 /usr/local/bin/heat_report.py /usr/local/bin/heat-ai-server.py \
    && mkdir -p /work/heat-output
EXPOSE 19097
ENTRYPOINT ["python3", "/usr/local/bin/heat-ai-server.py"]
CMD ["--input-dir", "/work/heat-output", "--output-dir", "/work/heat-output/ai"]

# Grafana dashboard sync stage: a one-shot sidecar that generates/updates
# dashboards on a Grafana instance via its API. Runs as a separate compose
# service so the target Grafana can live on any host (GRAFANA_URL env). Needs
# python3 + requests and the dashboard generator + its metric-consistency
# dependencies (metrics-common/metrics-golden-check). Not part of any runtime
# image.
FROM ubuntu-base AS grafana-sync
ARG DEBIAN_FRONTEND=noninteractive

RUN \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    python3 \
    python3-pip \
    curl \
    && apt-get clean

RUN python3 -m pip install --no-cache-dir --break-system-packages \
    --index-url https://repos.dp.nlmk.com/artifactory/api/pypi/pypi/simple \
    --trusted-host repos.dp.nlmk.com \
    requests

WORKDIR /opt/dashboards
COPY scripts/generate-grafana-dashboards.py /opt/dashboards/generate-grafana-dashboards.py
COPY scripts/metrics-consistency-check.py /opt/dashboards/metrics-consistency-check.py
COPY scripts/metrics_common.py /opt/dashboards/metrics_common.py
COPY scripts/metrics-golden-check.py /opt/dashboards/metrics-golden-check.py
RUN chmod 755 /opt/dashboards/generate-grafana-dashboards.py

# One-shot: waits for Grafana to become ready (GRAFANA_URL), then applies
# dashboards. Defaults mirror rebuild-and-run.sh. Exits 0 on success so
# restart: "on-failure" does not loop a healthy run.
ENTRYPOINT ["sh", "-c", "GRAFANA_URL=${GRAFANA_URL:-http://localhost:3000} python3 /opt/dashboards/generate-grafana-dashboards.py --correct-dashboards"]

# Registry report server stage: a small web UI that collects and shows reports
# about Docker registry repositories (sizes, tags), saves them to a volume and
# renders a page with charts. Built on the shared ubuntu-base (Ubuntu 26.04) so
# it gets a modern Python. Needs python3 + flask + the registry-report logic.
# nodejs is added so the inline <script> of the served HTML can be syntax-checked
# with `node --check` (scripts/check-registry-report-js.sh) — the UI's JS is a
# single in-page script and a syntax error silently kills the whole page.
# Runs as its own compose service on a dedicated port (REPORT_SERVER_PORT).
FROM ubuntu-base AS registry-report-server
ARG DEBIAN_FRONTEND=noninteractive

RUN \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    python3 \
    python3-pip \
    nodejs \
    && apt-get clean

RUN python3 -m pip install --no-cache-dir --break-system-packages \
    --index-url https://repos.dp.nlmk.com/artifactory/api/pypi/pypi/simple \
    --trusted-host repos.dp.nlmk.com \
    flask

WORKDIR /opt/registry-report
COPY scripts/registry-report-server.py /opt/registry-report/registry-report-server.py
COPY config/registries.json /opt/registry-report/config/registries.json
RUN chmod 755 /opt/registry-report/registry-report-server.py

# Serves the report UI on REPORT_SERVER_PORT (default 8083). Reports are
# persisted to REPORT_DIR (default /data), a named volume mounted by compose.
ENV REPORT_DIR=/data
ENV REPORT_SERVER_PORT=8083
EXPOSE 8083
ENTRYPOINT ["python3", "/opt/registry-report/registry-report-server.py"]

# Oracle OCI client libraries for the DB gateway. The libs are pulled from the
# gvenzl/oracle-xe:21.3.0-slim image (already used for the oracle service)
# instead of downloading the Instant Client from download.oracle.com — that
# host is not reachable in a closed network.
# REGISTRY_MIRROR prefixes the image for the isolated (registry-proxy) contour.
ARG REGISTRY_MIRROR=
FROM ${REGISTRY_MIRROR}gvenzl/oracle-xe:21.3.0-slim AS oracle-libs

# Shared runtime base - common filesystem layout, binary, ports and base runtime libraries
FROM ubuntu-base AS runtime-base

# addr2line (binutils) is needed at runtime: the crash handler resolves stack
# frames to src file:line in-place (crash_handler.hpp => annotate_exe_frames).
RUN \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    binutils \
    && apt-get clean

# Runtime libraries come from the ubuntu-base stage; here only the image
# layout is prepared (no apt, no network).
RUN rm -rf /tmp/* \
    && rm -rf /usr/share/doc/* \
    && rm -rf /usr/share/man/* \
    && rm -rf /usr/share/locale/* \
    && mkdir -p /memory-logs /profiles /crash-dumps \
    && chmod 777 /memory-logs /profiles /crash-dumps

WORKDIR /root/

# Binary is copied out of the cache-mounted build dir (/app/build) into the
# image layer (/app/out) so it survives for COPY --from=builder.
COPY --from=builder /app/out/l2-proxy .

# Oracle OCI client libraries — baked into the SHARED runtime-base because the
# single l2-proxy binary runs in any MODE (proxy/worker/server) and the worker
# mode connects to Oracle DBs. The libs are pulled from the gvenzl/oracle-xe
# image (already used for the oracle service) instead of download.oracle.com
# (unreachable in a closed network). REGISTRY_MIRROR prefixes the image for the
# isolated (registry-proxy) contour.
# The Oracle Home of Oracle XE 21c ships the same OCI client libraries (the
# client is a subset of the DB install). Only the client libs ODPI-C needs are
# copied: libclntsh.so.21.1 plus its only non-system dependencies
# (libclntshcore.so.21.1, libnnz21.so). A symlink without the version suffix is
# created below for dlopen("libclntsh.so"). The timezone data under
# oracore/zoneinfo is required too (Oracle client fails with ORA-01804 without
# it) and is resolved relative to the directory of libclntsh. The NLS data
# under nls/ is needed by the full OCI implementation to initialize
# national-language support (fails with ORA-12715 otherwise).
COPY --from=oracle-libs /opt/oracle/product/21c/dbhomeXE/lib/libclntsh.so.21.1 /opt/oracle/instantclient_21_13/
COPY --from=oracle-libs /opt/oracle/product/21c/dbhomeXE/lib/libclntshcore.so.21.1 /opt/oracle/instantclient_21_13/
COPY --from=oracle-libs /opt/oracle/product/21c/dbhomeXE/lib/libnnz21.so /opt/oracle/instantclient_21_13/
COPY --from=oracle-libs /opt/oracle/product/21c/dbhomeXE/oracore/zoneinfo /opt/oracle/instantclient_21_13/oracore/zoneinfo
COPY --from=oracle-libs /opt/oracle/product/21c/dbhomeXE/nls /opt/oracle/instantclient_21_13/nls

RUN \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    libaio1t64 \
    libnsl2 \
    && apt-get clean \
    && rm -rf /tmp/*

# The Oracle Home of Oracle XE 21c ships a full (non-Instant-Client) OCI
# implementation which locates NLS/timezone data via $ORACLE_HOME. Point it at
# the copied client dir so libclntsh.so finds oracore/zoneinfo (ORA-01804
# otherwise) and network/admin for TNSNAMES lookups.
ENV ORACLE_HOME=/opt/oracle/instantclient_21_13

# Point ld.so at the unpacked client. The Ubuntu t64 transition ships libaio
# only as libaio.so.1t64, but libclntsh.so links against the legacy SONAME
# libaio.so.1 — provide the ABI-compatible compat symlink.
RUN set -eux; \
    INSTANT_DIR=/opt/oracle/instantclient_21_13; \
    [ -n "${INSTANT_DIR}" ]; \
    echo "${INSTANT_DIR}" > /etc/ld.so.conf.d/oracle-instantclient.conf; \
    ln -sf libclntsh.so.21.1 "${INSTANT_DIR}/libclntsh.so"; \
    ln -sf libclntshcore.so.21.1 "${INSTANT_DIR}/libclntshcore.so"; \
    LIBAIO_PATH=$(find /usr/lib -name 'libaio.so.1t64' | head -1) && \
    LIBAIO_DIR=$(dirname "$LIBAIO_PATH") && \
    ln -sf "$LIBAIO_PATH" "${LIBAIO_DIR}/libaio.so.1"; \
    ldconfig

# Unprivileged runtime: every entry drops to the 'app' user (uid 10001) via
# docker-entrypoint.sh (chown of host-mounted writable dirs happens at start).
# Host dirs and ports are all above 1024, so no capabilities are required for
# the payload; docker-compose additionally drops all caps (see cap_drop).
RUN useradd --uid 10001 --no-create-home --shell /usr/sbin/nologin app \
    && chown -R app:app /root /memory-logs /profiles /crash-dumps

COPY docker-entrypoint.sh /app/docker-entrypoint.sh
RUN chmod 755 /app/docker-entrypoint.sh

ENTRYPOINT ["/app/docker-entrypoint.sh"]

EXPOSE 8888 19090 19091 19092 19093 19094 19095 8088

HEALTHCHECK --interval=15s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -sf http://localhost:8888/health/ready || exit 1

# exec replaces the shell so the binary becomes PID 1: docker stop (SIGTERM)
# reaches the process directly and graceful shutdown (drain, trace flush) runs.
CMD ["sh", "-c", "exec ./l2-proxy"]

# Optimized runtime stage - minimal Ubuntu with only essential libraries.
# All runtime libs (incl. libssl3t64) come from the ubuntu-base stage.
FROM runtime-base AS runtime

# Runtime stage with AddressSanitizer/LeakSanitizer support
FROM runtime-base AS runtime-asan

RUN \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    libasan8 \
    libubsan1 \
    && apt-get clean \
    && rm -rf /tmp/*

ENV ASAN_OPTIONS=detect_leaks=1:halt_on_error=0:log_path=/memory-logs/asan
ENV LSAN_OPTIONS=report_objects=1

# runtime-db is kept as an alias of runtime: the Oracle OCI client is now baked
# into the shared runtime-base, so any mode (proxy/worker/server) of the single
# l2-proxy binary can connect to Oracle DBs.
FROM runtime AS runtime-db

# Runtime stage with gperftools heap profiling support
FROM runtime-base AS runtime-profiler

RUN \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    libgoogle-perftools4t64 \
    && apt-get clean \
    && rm -rf /tmp/*

RUN ln -sf $(find /usr/lib -name 'libtcmalloc_and_profiler.so*' | head -1) /usr/lib/libtcmalloc_and_profiler.so 2>/dev/null || true
ENV LD_LIBRARY_PATH=/usr/lib:${LD_LIBRARY_PATH:-}
ENV CPUPROFILE=/profiles/l2-proxy-cpu.prof
ENV CPUPROFILE_FREQUENCY=100
ENV HEAPPROFILE=/profiles/l2-proxy.hprof
ENV HEAP_PROFILE_ALLOCATION_INTERVAL=1073741824
ENV HEAP_PROFILE_INUSE_INTERVAL=1073741824

# Runtime stage for valgrind leak detection.
# NOTE: valgrind is incompatible with ASan binaries, so this stage
# always uses the plain (non-sanitized) build.
FROM runtime-base AS runtime-valgrind

RUN \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    valgrind \
    && apt-get clean \
    && rm -rf /tmp/*
