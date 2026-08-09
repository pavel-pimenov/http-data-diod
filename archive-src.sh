#!/bin/bash
set -eu

ARCHIVE_NAME="http-data-diod-$(date +%Y%m%d-%H%M%S).tar.gz"

git archive --format=tar.gz \
    --prefix="http-data-diod/" \
    -o "$ARCHIVE_NAME" \
    HEAD

echo "Created: $ARCHIVE_NAME ($(du -h "$ARCHIVE_NAME" | cut -f1))"
