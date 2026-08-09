#!/bin/bash

# Try to get git SHA, fallback to unknown if git is not available
if command -v git >/dev/null 2>&1; then
    SHA=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
else
    SHA="unknown"
fi

echo "#ifndef L2_PROXY_VERSION_H"
echo "#define L2_PROXY_VERSION_H"
echo "#define VERSION \"1.0.4-$SHA\""
echo "#endif"
