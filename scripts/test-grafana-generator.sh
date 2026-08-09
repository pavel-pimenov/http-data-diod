#!/bin/bash
# Test script for Grafana dashboard generator
# Creates a temporary Grafana container and tests the dashboard generation

set -e

echo "=========================================="
echo "Grafana Dashboard Generator Test"
echo "=========================================="

# Start temporary Grafana
echo "Starting temporary Grafana container..."
docker run -d \
    --name grafana-test \
    -p 33000:3000 \
    -e GF_SECURITY_ADMIN_USER=admin \
    -e GF_SECURITY_ADMIN_PASSWORD=admin \
    -e GF_AUTH_ANONYMOUS_ENABLED=true \
    -e GF_AUTH_ANONYMOUS_ORG_ROLE=Admin \
    grafana/grafana:latest

echo "Waiting for Grafana to start..."
sleep 10

# Test Grafana connection
echo "Testing Grafana connection..."
curl -s http://localhost:33000/api/health | python3 -m json.tool || {
    echo "Failed to connect to Grafana"
    docker rm -f grafana-test > /dev/null 2>&1
    exit 1
}

# Add Prometheus datasource
echo "Adding Prometheus datasource..."
curl -s -X POST http://localhost:33000/api/datasources \
    -u admin:admin \
    -H "Content-Type: application/json" \
    -d '{
        "name": "prometheus",
        "type": "prometheus",
        "uid": "prometheus",
        "url": "http://localhost:9090",
        "access": "proxy",
        "isDefault": true
    }' || echo "Note: Prometheus datasource may already exist or Prometheus is not running"

# Run dashboard generator
echo ""
echo "Running dashboard generator..."
export GRAFANA_URL=http://localhost:33000
export GRAFANA_USER=admin
export GRAFANA_PASSWORD=admin

python3 scripts/generate-grafana-dashboards.py

RESULT=$?

# List created dashboards
echo ""
echo "Listing created dashboards..."
curl -s http://localhost:33000/api/search?type=dash-db \
    -u admin:admin | python3 -m json.tool

# Cleanup
echo ""
echo "Cleaning up..."
docker rm -f grafana-test > /dev/null 2>&1

echo ""
echo "=========================================="
if [ $RESULT -eq 0 ]; then
    echo "✅ Dashboard generator test PASSED"
else
    echo "❌ Dashboard generator test FAILED"
fi
echo "=========================================="

exit $RESULT
