#!/bin/bash

# Analyze crash dumps from the crash handler (raw addresses)
# Usage:
#   ./scripts/resolve-crash.sh                         # list + summarize all dumps
#   ./scripts/resolve-crash.sh <crash-dump-file>       # analyze one dump
#   ./scripts/resolve-crash.sh --latest                # analyze most recent dump
#   ./scripts/resolve-crash.sh --signal SIGSEGV        # filter by signal

set -e

CRASH_DIR="${CRASH_DIR:-crash-dumps}"
BINARY="${L2_PROXY_BINARY:-cpp/l2-proxy/l2-proxy}"

list_dumps() {
    echo "=== Crash dumps in $CRASH_DIR ==="
    echo ""
    local count=0
    for f in "$CRASH_DIR"/crash_*.txt; do
        [ -f "$f" ] || continue
        count=$((count + 1))
        local signal=$(grep "^Signal:" "$f" 2>/dev/null | head -1 | awk '{print $2}')
        local ts=$(grep "^Timestamp:" "$f" 2>/dev/null | head -1 | awk '{print $2}')
        local pid=$(grep "^PID:" "$f" 2>/dev/null | head -1 | awk '{print $2}')
        local fault=$(grep "^Fault address:" "$f" 2>/dev/null | head -1 | awk '{print $3}')
        local frames=$(grep -c "^#" "$f" 2>/dev/null || echo 0)
        printf "  %-55s  signal=%-8s  pid=%-6s  fault=%-12s  frames=%s\n" \
            "$(basename "$f")" "$signal" "$pid" "$fault" "$frames"
    done
    if [ "$count" -eq 0 ]; then
        echo "  (no crash dumps found)"
    else
        echo ""
        echo "  Total: $count crash dump(s)"
    fi
}

analyze_file() {
    local file="$1"
    if [ ! -f "$file" ]; then
        echo "ERROR: File not found: $file"
        return 1
    fi

    echo "============================================================"
    echo "Crash Dump Analysis"
    echo "============================================================"
    echo "File: $file"
    echo ""

    # Header
    grep -E "^(===|Signal:|PID:|Timestamp:|Fault)" "$file" 2>/dev/null | while IFS= read -r line; do
        echo "  $line"
    done
    echo ""

    # Resolve raw-address frames via addr2line when binary is available
    local use_addr2line=0
    if command -v addr2line >/dev/null 2>&1 && [ -f "$BINARY" ]; then
        use_addr2line=1
    fi

    echo "--- STACK TRACE ---"
    echo ""
    local frame_idx=0
    while IFS= read -r line; do
        if ! echo "$line" | grep -q "^#[0-9]"; then
            continue
        fi
        local addr=$(echo "$line" | awk '{print $2}')
        if [ "$use_addr2line" -eq 1 ]; then
            local resolved=$(addr2line -e "$BINARY" -fC "$addr" 2>/dev/null || echo "?")
            echo "  $line  ->  $resolved"
        else
            echo "  $line"
        fi
        frame_idx=$((frame_idx + 1))
    done < "$file"

    if [ "$use_addr2line" -eq 0 ]; then
        echo ""
        echo "  (raw addresses shown; install binutils and set L2_PROXY_BINARY"
        echo "   to the binary with symbols to resolve source locations)"
    elif [ "$frame_idx" -eq 0 ]; then
        echo "  (no frames found)"
    fi

    echo ""
    echo "============================================================"
}

# Parse arguments
FILTER_SIGNAL=""
TARGET_FILE=""
LATEST=0

while [ $# -gt 0 ]; do
    case "$1" in
        --latest)
            LATEST=1
            shift
            ;;
        --signal)
            FILTER_SIGNAL="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [--latest] [--signal SIGSEGV] [file]"
            echo ""
            echo "Options:"
            echo "  --latest           Analyze the most recent crash dump"
            echo "  --signal SIGNAL    Filter/list by signal (e.g. SIGSEGV)"
            echo "  (no args)          List all crash dumps with summary"
            exit 0
            ;;
        *)
            TARGET_FILE="$1"
            shift
            ;;
    esac
done

if [ -n "$TARGET_FILE" ]; then
    analyze_file "$TARGET_FILE"
elif [ "$LATEST" -eq 1 ]; then
    latest=$(ls -t "$CRASH_DIR"/crash_*.txt 2>/dev/null | head -1)
    if [ -z "$latest" ]; then
        echo "No crash dumps found in $CRASH_DIR"
        exit 1
    fi
    analyze_file "$latest"
elif [ -n "$FILTER_SIGNAL" ]; then
    echo "=== Crash dumps with signal=$FILTER_SIGNAL ==="
    echo ""
    found=0
    for f in "$CRASH_DIR"/crash_*.txt; do
        [ -f "$f" ] || continue
        if grep -q "^Signal: $FILTER_SIGNAL" "$f"; then
            found=$((found + 1))
            analyze_file "$f"
        fi
    done
    if [ "$found" -eq 0 ]; then
        echo "  No crash dumps found with signal=$FILTER_SIGNAL"
    fi
else
    list_dumps
fi
