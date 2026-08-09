#!/bin/bash

# Script to run the l2-proxy with heap profiling using gperftools

# Check if we're in the right directory
if [ ! -f "CMakeLists.txt" ]; then
    echo "Error: CMakeLists.txt not found. Please run this script from the cpp/l2-proxy directory."
    exit 1
fi

# Install gperftools if not already installed
if ! command -v pprof &> /dev/null; then
    echo "Installing gperftools..."
    sudo apt-get update
    sudo apt-get install -y google-perftools libgoogle-perftools-dev
fi

# Create build directory if it doesn't exist
if [ ! -d "build-profile" ]; then
    mkdir build-profile
fi

echo "Building l2-proxy with profiling enabled..."

# Build with profiling
cd build-profile
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCPPHTTPLIB_OPENSSL_SUPPORT=ON ..
make l2-proxy -j2

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

cd ..

echo "Build successful!"

# Set environment variables for heap profiling
export LD_PRELOAD="/usr/lib/x86_64-linux-gnu/libprofiler.so.0"
export CPUPROFILE=./l2-proxy.prof
export HEAPPROFILE=./l2-proxy.hprof
export HEAP_PROFILE_ALLOCATION_INTERVAL=1048576  # 1MB
export HEAP_PROFILE_INUSE_INTERVAL=1048576       # 1MB

echo "Running l2-proxy with heap profiling..."
echo "Mode: ${MODE:-proxy}"
echo "Log Level: ${LOG_LEVEL:-INFO}"
echo "Heap profile will be saved to ./l2-proxy.hprof.*"

# Run the application
./build-profile/l2-proxy

echo ""
echo "Heap profiling completed."
echo "To analyze the heap profile, run: pprof ./build-profile/l2-proxy ./l2-proxy.hprof.*"