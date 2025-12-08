#!/bin/bash

# Script to load Docker images from tar.gz files and start docker-compose

set -e  # Exit on any error

echo "Loading Docker images from tar.gz files..."

# Find all .tar.gz files in the current directory
tar_files=$(ls *.tar.gz 2>/dev/null || true)

if [ -z "$tar_files" ]; then
    echo "No .tar.gz files found in the current directory"
    exit 1
fi

# Load each image
for file in $tar_files; do
    echo "Loading image from $file..."
    docker load < "$file"
    echo "Successfully loaded $file"
done

echo "All images loaded successfully"

# Optional: Start docker-compose
read -p "Do you want to start docker-compose now? (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Starting docker-compose..."
    docker-compose up -d
    echo "Docker-compose started"
else
    echo "Skipping docker-compose start"
fi
