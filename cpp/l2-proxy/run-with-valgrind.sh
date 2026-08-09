#!/bin/bash

# Script to run the l2-proxy with Valgrind for memory leak detection

# Set default values
MODE=${MODE:-proxy}
LOG_LEVEL=${LOG_LEVEL:-INFO}
VALGRIND_OPTIONS="--leak-check=full --show-leak-kinds=all --track-origins=yes --verbose --log-file=valgrind-output.log --suppressions=valgrind.supp"

# Print usage information
echo "Running l2-proxy with Valgrind"
echo "Mode: $MODE"
echo "Log Level: $LOG_LEVEL"
echo "Valgrind Options: $VALGRIND_OPTIONS"
echo ""

# Run the application with Valgrind
valgrind $VALGRIND_OPTIONS ./l2-proxy

echo ""
echo "Valgrind output saved to valgrind-output.log"
echo "To view the output, run: cat valgrind-output.log"