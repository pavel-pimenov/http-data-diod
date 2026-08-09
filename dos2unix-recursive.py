#!/usr/bin/env python3
import os

# File extensions to include
SOURCE_EXTS = [
    '.c', '.cpp', '.cxx', '.cc',
    '.h', '.hpp', '.hxx', '.hh', '.json', '.md', '.py', '.sh', '.txt', '.yaml', '.yml', '.inl', '.conf','.supp','.gitignore'
]

def is_source_file(filename):
    _, ext = os.path.splitext(filename)
    return ext.lower() in SOURCE_EXTS

def dos2unix_inplace(filepath):
    try:
        with open(filepath, 'rb') as f:
            content = f.read()
        # Replace CRLF with LF
        new_content = content.replace(b'\r\n', b'\n')
        if new_content != content:
            with open(filepath, 'wb') as f:
                f.write(new_content)
            print(f"Converted: {filepath}")
        else:
            print(f"Already unix: {filepath}")
    except Exception as e:
        print(f"Error processing {filepath}: {e}")

def get_gitignore_extensions(root):
    gitignore_path = os.path.join(root, '.gitignore')
    if not os.path.exists(gitignore_path):
        return set(), set()
    extensions = set()
    ignored_dirs = set()
    try:
        with open(gitignore_path, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                parts = line.split()
                for part in parts:
                    if '*' in part and '.' in part:
                        # like *.log
                        idx = part.find('*.')
                        if idx != -1:
                            ext_part = part[idx+2:]
                            extensions.add('.' + ext_part)
                    elif part.startswith('.') and '/' not in part:
                        # like .log
                        extensions.add(part)
                    elif '/' not in part and '*' not in part and '.' not in part:
                        # assume directory
                        ignored_dirs.add(part)
    except:
        pass
    return extensions, ignored_dirs

def find_missing_text_extensions(root, gitignore_exts, ignored_dirs):
    extensions = set()
    for dirpath, dirs, filenames in os.walk(root, topdown=True):
        # Skip ignored directories
        parts = dirpath[len(root)+1:].split(os.sep) if dirpath != root else []
        if any(part in ignored_dirs for part in parts):
            dirs[:] = []  # prune
            continue
        for fname in filenames:
            _, ext = os.path.splitext(fname)
            if ext:
                extensions.add(ext.lower())

    missing_text = []
    for ext in extensions:
        if ext.lower() in [e.lower() for e in SOURCE_EXTS] or ext.lower() in [e.lower() for e in gitignore_exts]:
            continue
        # Find one file with this extension
        found_text = False
        for dirpath, dirs, filenames in os.walk(root, topdown=True):
            # Skip ignored directories
            parts = dirpath[len(root)+1:].split(os.sep) if dirpath != root else []
            if any(part in ignored_dirs for part in parts):
                dirs[:] = []  # prune
                continue
            for fname in filenames:
                if fname.lower().endswith(ext):
                    filepath = os.path.join(dirpath, fname)
                    try:
                        with open(filepath, 'r', encoding='utf-8') as f:
                            f.read(1024)
                        found_text = True
                    except:
                        pass
                    break
            if found_text:
                missing_text.append(ext)
                break
    return sorted(missing_text)

def main():
    root = os.path.dirname(os.path.abspath(__file__))
    processed, converted = 0, 0
    for dirpath, _, filenames in os.walk(root):
        for fname in filenames:
            if is_source_file(fname):
                processed += 1
                fpath = os.path.join(dirpath, fname)
                # Convert file
                try:
                    with open(fpath, 'rb') as f:
                        content = f.read()
                    new_content = content.replace(b'\r\n', b'\n')
                    if new_content != content:
                        with open(fpath, 'wb') as f:
                            f.write(new_content)
                        converted += 1
                        print(f"Converted: {fpath}")
                except Exception as e:
                    print(f"Error processing {fpath}: {e}")
    print(f"Processed files: {processed}, Converted: {converted}")

    gitignore_exts, ignored_dirs = get_gitignore_extensions(root)
    missing = find_missing_text_extensions(root, gitignore_exts, ignored_dirs)
    if missing:
        print("Missing text extensions:")
        for ext in missing:
            print(f"  {ext}")

if __name__ == "__main__":
    main()
