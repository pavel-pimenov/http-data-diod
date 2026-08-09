#!/usr/bin/env python3

import re
import os
import sys
from pathlib import Path

def get_source_files():
    """Get all source files in the project"""
    source_files = []
    for ext in ['.cpp', '.hpp']:
        for file in Path('.').rglob(f'*{ext}'):
            # Skip build directory and test files
            if 'build' in str(file) or 'test_' in str(file):
                continue
            source_files.append(str(file))
    return sorted(source_files)

def get_dockerfile_files():
    """Extract files mentioned in Dockerfile COPY commands"""
    docker_files = set()

    with open('Dockerfile', 'r') as f:
        content = f.read()

    # Find all COPY lines
    copy_lines = re.findall(r'COPY\s+(.*?\\\s*)*\s*\.', content, re.DOTALL)

    # Extract file names from COPY lines
    for line in content.split('\n'):
        if line.strip().startswith('COPY') and not line.strip().endswith('\\'):
            # Simple COPY line
            parts = line.split()
            for part in parts[1:-1]:  # Skip COPY and .
                if part != '\\':
                    docker_files.add(part)
        elif line.strip().startswith('COPY') and line.strip().endswith('\\'):
            # Multi-line COPY
            continue

    # Handle multi-line COPY commands
    lines = content.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if line.startswith('COPY') and line.endswith('\\'):
            # Collect all files until we find a line that doesn't end with \
            files = []
            while i < len(lines) and lines[i].strip().endswith('\\'):
                line_content = lines[i].strip().rstrip('\\').strip()
                if line_content != 'COPY':
                    files.extend(line_content.split())
                i += 1
            # Add the last line
            if i < len(lines):
                line_content = lines[i].strip()
                if line_content != '.':
                    files.extend(line_content.split())
            # Add all files to docker_files
            for file in files:
                if file not in ['COPY', '.', '\\']:
                    docker_files.add(file)
        i += 1

    return docker_files

def main():
    print("Checking if all source files are included in Dockerfile...")

    source_files = get_source_files()
    docker_files = get_dockerfile_files()

    # Get base names for comparison
    source_basenames = {os.path.basename(f) for f in source_files if 'civetweb' not in f and 'nlohmann' not in f and 'prometheus-cpp' not in f}
    docker_basenames = {os.path.basename(f) for f in docker_files if f != '.'}

    missing_files = source_basenames - docker_basenames

    if not missing_files:
        print("✅ All source files are included in Dockerfile")
        return 0
    else:
        print("❌ Missing files in Dockerfile:")
        for file in sorted(missing_files):
            print(f"  - {file}")
        print("\nPlease add these files to the Dockerfile COPY command")
        return 1

if __name__ == '__main__':
    sys.exit(main())
