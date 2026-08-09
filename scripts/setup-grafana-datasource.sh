#!/bin/bash
# Setup Prometheus datasource in Grafana
# This script configures Prometheus as a data source for Grafana dashboards

set -e

GRAFANA_URL="${GRAFANA_URL:-http://localhost:3000}"
GRAFANA_USER="${GRAFANA_USER:-admin}"
GRAFANA_PASSWORD="${GRAFANA_PASSWORD:-admin}"
PROMETHEUS_URL="${PROMETHEUS_URL:-http://host.docker.internal:9090}"

echo "=========================================="
echo "Grafana Datasource Setup"
echo "=========================================="
echo "Grafana URL: $GRAFANA_URL"
echo "Prometheus URL: $PROMETHEUS_URL"
echo ""

# Wait for Grafana to be ready
echo "Waiting for Grafana to be ready..."
for i in $(seq 1 30); do
    if curl -s --connect-timeout 2 --max-time 5 "$GRAFANA_URL/api/health" > /dev/null 2>&1; then
        echo "✅ Grafana is ready!"
        break
    fi
    if [ $i -eq 30 ]; then
        echo "❌ Grafana did not start within 30 seconds"
        exit 1
    fi
    sleep 1
done

# Check if datasource already exists
echo "Checking if Prometheus datasource exists..."
EXISTING=$(curl -s -u "$GRAFANA_USER:$GRAFANA_PASSWORD" \
    "$GRAFANA_URL/api/datasources/name/prometheus" 2>/dev/null)

if echo "$EXISTING" | grep -q '"id"'; then
    echo "✅ Prometheus datasource already exists"
    exit 0
fi

# Create Prometheus datasource
echo "Creating Prometheus datasource..."
RESPONSE=$(curl -s -X POST -u "$GRAFANA_USER:$GRAFANA_PASSWORD" \
    -H "Content-Type: application/json" \
    -d "{
        \"name\": \"prometheus\",
        \"type\": \"prometheus\",
        \"uid\": \"prometheus\",
        \"url\": \"$PROMETHEUS_URL\",
        \"access\": \"proxy\",
        \"isDefault\": true,
        \"jsonData\": {
            \"httpMethod\": \"POST\",
            \"timeInterval\": \"10s\"
        }
    }" \
    "$GRAFANA_URL/api/datasources" 2>/dev/null)

if echo "$RESPONSE" | grep -q '"datasource"'; then
    echo "✅ Prometheus datasource created successfully!"
    echo "   ID: $(echo "$RESPONSE" | grep -o '"id":[0-9]*' | cut -d: -f2)"
else
    echo "⚠️  Datasource creation response: $RESPONSE"
    echo "   (This may be okay if datasource already exists)"
fi

echo ""
echo "=========================================="
echo "✅ Datasource setup complete!"
echo "=========================================="
