#!/bin/bash

# Script to automatically check if all source files are included in Dockerfile

echo "Checking if all source files are included in Dockerfile..."

# Get list of source files from the current directory
SOURCE_FILES=$(find . -name "*.cpp" -o -name "*.hpp" | grep -v "build/" | grep -v "test_" | sort)

# Get list of files mentioned in Dockerfile COPY command
DOCKER_FILES=$(grep "COPY.*\.\*" Dockerfile | sed 's/COPY //; s/ \.\///g' | tr ' ' '\n' | grep -v "CMakeLists.txt" | grep -v "civetweb/" | grep -v "nlohmann/" | grep -v "prometheus-cpp/" | grep -v "COPY" | grep -v "\\" | sort)

# Check if all source files are included
MISSING_FILES=""
for file in $SOURCE_FILES; do
    if ! echo "$DOCKER_FILES" | grep -q "$(basename $file)"; then
        MISSING_FILES="$MISSING_FILES $file"
    fi
done

if [ -z "$MISSING_FILES" ]; then
    echo "✅ All source files are included in Dockerfile"
    exit 0
else
    echo "❌ Missing files in Dockerfile:"
    for file in $MISSING_FILES; do
        echo "  - $file"
    done
    echo ""
    echo "Please add these files to the Dockerfile COPY command"
    exit 1
fi