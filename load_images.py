#!/usr/bin/env python3
import subprocess
import glob
import os

def load_image(tar_file):
    """
    Load a Docker image from a tar.gz file.
    """
    try:
        print(f"Loading image from {tar_file}...")
        with open(tar_file, 'rb') as f:
            subprocess.run(['docker', 'load'], stdin=f, check=True, capture_output=True)
        print(f"Successfully loaded {tar_file}")
    except subprocess.CalledProcessError as e:
        print(f"Error loading {tar_file}: {e}")
    except Exception as e:
        print(f"Unexpected error with {tar_file}: {e}")

def main():
    # Find all .tar.gz files in the current directory
    tar_files = glob.glob('*.tar.gz')

    if not tar_files:
        print("No .tar.gz files found in the current directory")
        return

    print("Loading Docker images from tar.gz files...")

    # Load each image
    for tar_file in tar_files:
        load_image(tar_file)

    print("All images loaded successfully")

    # Optional: Start docker-compose
    try:
        response = input("Do you want to start docker-compose now? (y/n): ").strip().lower()
        if response == 'y':
            print("Starting docker-compose...")
            subprocess.run(['docker-compose', 'up', '-d'], check=True)
            print("Docker-compose started")
        else:
            print("Skipping docker-compose start")
    except KeyboardInterrupt:
        print("\nSkipping docker-compose start")
    except subprocess.CalledProcessError as e:
        print(f"Error starting docker-compose: {e}")

if __name__ == "__main__":
    main()
