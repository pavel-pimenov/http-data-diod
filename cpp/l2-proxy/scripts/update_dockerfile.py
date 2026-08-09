#!/usr/bin/env python3

import os
import sys
from pathlib import Path

def get_source_files():
    """Get all source files in the project"""
    source_files = []
    for ext in ['.cpp', '.hpp']:
        for file in Path('.').rglob(f'*{ext}'):
            # Skip build directory, test files, and directories
            if ('build' in str(file) or
                'test_' in str(file) or
                'civetweb' in str(file) or
                'nlohmann' in str(file) or
                'prometheus-cpp' in str(file) or
                'sqlite' in str(file) or
                file.is_dir()):
                continue
            source_files.append(str(file))
    return sorted(source_files)

def update_dockerfile():
    """Update Dockerfile with all source files"""
    source_files = get_source_files()

    # Separate .cpp and .hpp files
    cpp_files = [f for f in source_files if f.endswith('.cpp')]
    hpp_files = [f for f in source_files if f.endswith('.hpp')]

    # Read current Dockerfile
    with open('Dockerfile', 'r') as f:
        lines = f.readlines()

    # Find the COPY lines
    new_lines = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.strip().startswith('COPY') and ('.cpp' in line or '.hpp' in line):
            # Skip the current COPY lines
            while i < len(lines) and lines[i].strip().endswith('\\'):
                i += 1
            if i < len(lines):
                i += 1  # Skip the last line of the COPY command

            # Add new COPY lines
            # First line with CMakeLists.txt and .cpp files
            cpp_line = 'COPY CMakeLists.txt ' + ' '.join([os.path.basename(f) for f in cpp_files]) + ' \\'
            new_lines.append(cpp_line + '\n')

            # Second line with .hpp files
            hpp_files_basenames = [os.path.basename(f) for f in hpp_files]
            # Split into multiple lines if too long
            hpp_line = '      ' + ' '.join(hpp_files_basenames[:len(hpp_files_basenames)//2]) + ' \\'
            new_lines.append(hpp_line + '\n')

            hpp_line2 = '      ' + ' '.join(hpp_files_basenames[len(hpp_files_basenames)//2:]) + ' ./'
            new_lines.append(hpp_line2 + '\n')
        else:
            new_lines.append(line)
            i += 1

    # Write back to Dockerfile
    with open('Dockerfile', 'w') as f:
        f.writelines(new_lines)

    print("✅ Dockerfile updated with all source files")

def check_dockerfile():
    """Check if all source files are included in Dockerfile"""
    source_files = get_source_files()
    source_basenames = {os.path.basename(f) for f in source_files}

    # Read Dockerfile and extract file names
    with open('Dockerfile', 'r') as f:
        content = f.read()

    # Extract files from COPY commands
    docker_files = set()
    lines = content.split('\n')
    for line in lines:
        if line.strip().startswith('COPY') and ('.cpp' in line or '.hpp' in line):
            parts = line.split()
            for part in parts:
                if part.endswith(('.cpp', '.hpp')) and part != 'CMakeLists.txt':
                    docker_files.add(part)
                # Also check for files in continuation lines
                elif part.endswith(('.cpp', '.hpp')):
                    docker_files.add(part)

    # Also check the line after the first COPY line
    for i, line in enumerate(lines):
        if line.strip().startswith('COPY') and ('.cpp' in line or '.hpp' in line):
            # Check next line
            if i + 1 < len(lines):
                next_line = lines[i + 1].strip()
                parts = next_line.split()
                for part in parts:
                    if part.endswith(('.cpp', '.hpp')):
                        docker_files.add(part)
            # Check line after next
            if i + 2 < len(lines):
                next_line = lines[i + 2].strip()
                parts = next_line.split()
                for part in parts:
                    if part.endswith(('.cpp', '.hpp')) and part != './':
                        docker_files.add(part)

    missing_files = source_basenames - docker_files

    if not missing_files:
        print("✅ All source files are included in Dockerfile")
        return True
    else:
        print("❌ Missing files in Dockerfile:")
        for file in sorted(missing_files):
            print(f"  - {file}")
        return False

def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--update':
        update_dockerfile()
    else:
        success = check_dockerfile()
        if not success:
            print("\nTo automatically update Dockerfile, run: python3 scripts/update_dockerfile.py --update")
            return 1
    return 0

if __name__ == '__main__':
    sys.exit(main())
