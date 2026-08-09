#!/bin/bash

# Script to automatically check and update Dockerfile when new files are added

set -e

echo "🔍 Checking Dockerfile for missing source files..."

cd "$(dirname "$0")/.."

# Run the Python script to check Dockerfile
if python3 scripts/update_dockerfile.py; then
    echo "✅ Dockerfile is up to date"
    exit 0
else
    echo "🔄 Updating Dockerfile with missing files..."
    python3 scripts/update_dockerfile.py --update
    echo "✅ Dockerfile updated successfully"
    echo "📝 Please review the changes in Dockerfile and commit if correct"
    exit 0
fi