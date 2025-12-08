#!/usr/bin/env python3
import yaml
import subprocess
import os

def save_image(image_name):
    """
    Pull the Docker image and save it as a tar.gz file.
    """
    try:
        # Pull the image
        print(f"Pulling image: {image_name}")
        subprocess.run(['docker', 'pull', image_name], check=True, capture_output=True)

        # Create a safe filename
        filename = image_name.replace('/', '_').replace(':', '_') + '.tar.gz'

        # Save the image and compress it
        print(f"Saving image {image_name} to {filename}")
        with open(filename, 'wb') as f:
            # docker save outputs to stdout, pipe to gzip
            p1 = subprocess.Popen(['docker', 'save', image_name], stdout=subprocess.PIPE)
            p2 = subprocess.Popen(['gzip'], stdin=p1.stdout, stdout=f)
            p1.stdout.close()
            p2.communicate()

        print(f"Successfully saved {image_name} to {filename}")

    except subprocess.CalledProcessError as e:
        print(f"Error processing image {image_name}: {e}")
    except Exception as e:
        print(f"Unexpected error with image {image_name}: {e}")

def main():
    # Load docker-compose.yml
    try:
        with open('docker-compose.yml', 'r') as f:
            compose_data = yaml.safe_load(f)
    except FileNotFoundError:
        print("Error: docker-compose.yml not found in current directory")
        return
    except yaml.YAMLError as e:
        print(f"Error parsing docker-compose.yml: {e}")
        return

    # Extract images from services
    services = compose_data.get('services', {})
    if not services:
        print("No services found in docker-compose.yml")
        return

    for service_name, service_config in services.items():
        if 'image' in service_config:
            image_name = service_config['image']
            save_image(image_name)

if __name__ == "__main__":
    main()
