#!/bin/bash
# Test script for Grafana dashboard generator
# Creates a temporary Grafana container and tests the dashboard generation

set -e

echo "=========================================="
echo "Grafana Dashboard Generator Test"
echo "=========================================="

# Start temporary Grafana
echo "Starting temporary Grafana container..."
docker rm -f grafana-test > /dev/null 2>&1 || true
docker run -d \
    --name grafana-test \
    -p 33000:3000 \
    -e GF_SECURITY_ADMIN_USER=admin \
    -e GF_SECURITY_ADMIN_PASSWORD=admin \
    -e GF_AUTH_ANONYMOUS_ENABLED=true \
    -e GF_AUTH_ANONYMOUS_ORG_ROLE=Admin \
    grafana/grafana:latest

echo "Waiting for Grafana to start..."
for i in $(seq 1 30); do
    if curl -sf http://localhost:33000/api/health >/dev/null 2>&1; then
        echo "Grafana is ready (after $((i * 2))s)"
        break
    fi
    if [ "$i" -eq 30 ]; then
        echo "Grafana did not become ready in time"
        docker rm -f grafana-test > /dev/null 2>&1
        exit 1
    fi
    sleep 2
done

# Test Grafana connection
echo "Testing Grafana connection..."
curl -s http://localhost:33000/api/health | python3 -m json.tool || {
    echo "Failed to connect to Grafana"
    docker rm -f grafana-test > /dev/null 2>&1
    exit 1
}

# Run dashboard generator (создаст и datasource через API)
echo ""
echo "Running dashboard generator..."
export GRAFANA_URL=http://localhost:33000
export GRAFANA_USER=admin
export GRAFANA_PASSWORD=admin

python3 scripts/generate-grafana-dashboards.py --datasource-url http://localhost:9090

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
