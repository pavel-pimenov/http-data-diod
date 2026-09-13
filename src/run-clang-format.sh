#!/bin/bash
# Format C/C++ sources in the l2-proxy module using clang-format

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if ! command -v clang-format >/dev/null 2>&1; then
    echo "Error: clang-format is not installed or not available in PATH"
    exit 1
fi

echo "=========================================="
echo "Formatting l2-proxy sources with clang-format"
echo "=========================================="
echo ""

mapfile -t FILES < <(find . \
    -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" \) \
    -not -path "./build/*" \
    -not -path "./build_tests/*" \
    -not -path "./nats/*" \
    -not -path "./httplib/*" \
    -not -path "./base64/*" \
    -not -path "./certs/*" \
    | sort)

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "No source files found for formatting"
    exit 0
fi

echo "Files to format: ${#FILES[@]}"
printf ' - %s\n' "${FILES[@]}"
echo ""

clang-format -i "${FILES[@]}"

echo ""
echo "Formatting completed successfully"