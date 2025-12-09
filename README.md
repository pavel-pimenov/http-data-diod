# http-redis-proxy

sudo apt install libhiredis-dev libmicrohttpd-dev libjsoncpp-dev libcurl4-openssl-dev python3-pip 
sudo apt-get install apache2-utils

# sudo apt-get install  opentelemetry-cpp-dev (only ubuntu 25.10)

Perfect! The docker-compose.yml has been updated successfully:

- Replaced Redis with Valkey (using `valkey/valkey:alpine` image)

- Added a custom network configuration with a specific subnet (`172.25.0.0/16`) to avoid Docker's address pool exhaustion error

- All containers are now running:

  - Valkey (Redis-compatible database) on port 6379
  - l2-proxy on port 8888
  - l2-server on port 3000
  - l2-worker (running in the background)

The services are properly interconnected and should work as intended for the HTTP-Redis proxy system.
