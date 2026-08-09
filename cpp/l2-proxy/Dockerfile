# syntax=docker/dockerfile:1

ARG BASE_IMAGE=ubuntu:26.04

# Shared base layer: run apt-get update once, reuse across all stages
FROM ${BASE_IMAGE} AS ubuntu-base
ARG DEBIAN_FRONTEND=noninteractive
ARG APT_MIRROR=
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
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    libprometheus-cpp-core1.0 \
    libprometheus-cpp-pull1.0 \
    libfmt10 \
    libspdlog1.15 \
    libssl3t64 \
    ca-certificates \
    curl \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Main build stage: apt deps + NATS C client + application.
# NATS is built here (no separate deps-builder stage) so the pipeline runs
# apt-get update/install only ONCE. The old two-stage layout installed the
# same base packages (build-essential, cmake, ...) twice, wasting ~2-4 min
# of every cold build.
FROM ubuntu-base AS builder

ARG DEBIAN_FRONTEND=noninteractive

# Install build dependencies (stable layer — rarely changes)
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    build-essential \
    ccache \
    ninja-build \
    ca-certificates \
    cmake \
    pkg-config \
    zlib1g-dev \
    libssl-dev \
    libzstd-dev \
    prometheus-cpp-dev \
    libspdlog-dev \
    libfmt-dev \
    libgoogle-perftools-dev \
    clang-tidy \
    clang-format \
    catch2 \
    libcli11-dev \
    nlohmann-json3-dev \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Build NATS C client (static) in its own COPY + RUN, BEFORE the app sources:
# the NATS layer then only rebuilds when nats/ changes, not on every commit
# that touches the application sources (COPY . . below comes after).
COPY nats ./nats

RUN --mount=type=cache,target=/root/.ccache \
    echo "=== Building NATS C client ===" && \
    cd /app/nats && \
    rm -rf build && mkdir build && cd build && \
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DCMAKE_C_COMPILER_LAUNCHER=ccache \
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

# Build application. For ASan builds use Debug + ENABLE_ASAN=ON.
# For profiler builds use ENABLE_PROFILER=ON (links tcmalloc + profiler).
# Use BuildKit cache for CMake and object files
ARG ENABLE_PROFILER=false
ARG ENABLE_ASAN=false
# Declared just before its use so changing it only invalidates the build RUN,
# not the apt-install layers above (an ARG in scope re-keys every later step).
# CACHE_BUST forces rebuild when changed: bump value or set via env to invalidate cache
ARG CACHE_BUST=1
# The /app/build mount persists the ninja build dir between RUN invocations, so
# a code change recompiles only the affected unity batch and relinks instead of
# rebuilding everything via ccache. The id embeds the build mode + CACHE_BUST:
# switching sanitizers/profiler or bumping CACHE_BUST yields a clean build dir.
RUN --mount=type=cache,target=/root/.ccache \
    --mount=type=cache,id=appbuild-${ENABLE_ASAN}-${ENABLE_PROFILER}-${CACHE_BUST},target=/app/build \
    echo "CACHE_BUST=${CACHE_BUST}" && \
    mkdir -p build && cd build && \
    if [ "$ENABLE_ASAN" = "true" ]; then \
        echo "Build mode: DEBUG + sanitizers (ASan, LSan via detect_leaks=1, UBSan) -fsanitize=address,undefined" && \
        cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug \
              -DENABLE_ASAN=ON \
              -DBUILD_TESTS=ON ..; \
    elif [ "$ENABLE_PROFILER" = "true" ]; then \
        echo "Build mode: RelWithDebInfo + gperftools heap profiler" && \
        cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
              -DENABLE_PROFILER=ON \
              -DBUILD_TESTS=ON ..; \
    else \
        echo "Build mode: RelWithDebInfo (optimized, no sanitizers)" && \
        cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
              -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++" \
              -DBUILD_TESTS=ON ..; \
    fi && \
    echo "=== cmake configure done ===" && \
    _T1=$(date +%s) && \
    ninja -j$(nproc) && \
    _T2=$(date +%s) && \
    ninja test_components && \
    _T3=$(date +%s) && \
    ./test_components && \
    _T4=$(date +%s) && \
    echo "=== build timing ===" && \
    echo "  compile:    $((_T2 - _T1))s" && \
    echo "  test build: $((_T3 - _T2))s" && \
    echo "  test run:   $((_T4 - _T3))s" && \
    echo "  total:      $((_T4 - _T1))s" && \
    mkdir -p /app/out && cp /app/build/l2-proxy /app/out/l2-proxy

# Keep debug info (DWARF) for crash handler stack trace resolution

# Lint stage: reconfigure with compile_commands.json + no Unity Build, then run clang-tidy.
# Uses a fresh build dir so per-file compile_commands.json entries are generated
# (a reused Unity build dir would leave only unity_* entries and clang-tidy would
# analyze sources without flags). Runs on all project .cpp files; a curated
# pre-commit gate (scripts/run-clang-tidy.sh) lints only the changed files.
FROM builder AS lint
RUN rm -rf build-lint && mkdir build-lint && cd build-lint && \
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug \
          -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
          -DCMAKE_UNITY_BUILD=OFF \
          .. && \
    find /app -name '*.cpp' ! -path '*/httplib/*' ! -path '*/base64/*' | \
    xargs clang-tidy -p . \
    --header-filter='.*\.(hpp|h)$' \
    2>&1 || true

# Shared runtime base - common filesystem layout, binary, ports and base runtime libraries
FROM ubuntu-base AS runtime-base

# binutils removed to shrink image; crash handler writes raw addresses, resolved on host via scripts/resolve-crash.sh
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

EXPOSE 8888 19090 19091 19092 8088

HEALTHCHECK --interval=15s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -sf http://localhost:8888/health/ready || exit 1

CMD ["sh", "-c", "./l2-proxy"]

# Optimized runtime stage - minimal Ubuntu with only essential libraries.
# All runtime libs (incl. libssl3t64) come from the ubuntu-base stage.
FROM runtime-base AS runtime

# Runtime stage with AddressSanitizer/LeakSanitizer support
FROM runtime-base AS runtime-asan

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    libasan8 \
    libubsan1 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* /tmp/*

ENV ASAN_OPTIONS=detect_leaks=1:halt_on_error=0:log_path=/memory-logs/asan
ENV LSAN_OPTIONS=report_objects=1

# Runtime stage with gperftools heap profiling support
FROM runtime-base AS runtime-profiler

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    libgoogle-perftools4t64 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* /tmp/*

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

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    valgrind \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* /tmp/*
