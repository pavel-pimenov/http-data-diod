#!/usr/bin/env python3
"""
Grafana Dashboard Generator for HTTP Proxy
Generates and updates all dashboards via Grafana API

Usage:
    python3 scripts/generate-grafana-dashboards.py [--config CONFIG_FILE]

Options:
    --config CONFIG_FILE    Path to JSON/YAML config file with Grafana settings

Interactive Mode:
    If no environment variables or config file are provided, the script will
    prompt for authentication interactively.

Environment Variables (optional, will be overridden by --config):
    GRAFANA_URL - Grafana URL (default: http://localhost:3000)
    GRAFANA_USER - Grafana username (default: admin)
    GRAFANA_PASSWORD - Grafana password (default: admin)
    GRAFANA_API_KEY - Grafana API key (alternative to user/password)
"""

import os
import sys
import json
import argparse
import requests
from typing import Dict, List, Any, Optional
import logging

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# ============================================================================
# Configuration Management
# ============================================================================

def load_config_file(config_path: str) -> Dict[str, Any]:
    """Load configuration from JSON or YAML file"""
    if not os.path.exists(config_path):
        logger.error(f"Config file not found: {config_path}")
        sys.exit(1)
    
    try:
        with open(config_path, 'r') as f:
            if config_path.endswith('.json'):
                return json.load(f)
            elif config_path.endswith('.yaml') or config_path.endswith('.yml'):
                try:
                    import yaml
                    return yaml.safe_load(f)
                except ImportError:
                    logger.error("PyYAML not installed. Install with: pip install pyyaml")
                    sys.exit(1)
            else:
                # Try JSON first, then YAML
                try:
                    return json.load(f)
                except json.JSONDecodeError:
                    try:
                        import yaml
                        f.seek(0)
                        return yaml.safe_load(f)
                    except ImportError:
                        logger.error("Cannot determine config format. Use .json or install PyYAML for .yaml")
                        sys.exit(1)
    except Exception as e:
        logger.error(f"Failed to load config file: {e}")
        sys.exit(1)


def get_interactive_auth() -> Dict[str, str]:
    """Prompt user for Grafana authentication interactively"""
    print("\n" + "=" * 60)
    print("Grafana Authentication")
    print("=" * 60)
    
    # Get Grafana URL
    url = input("\nGrafana URL [http://localhost:3000]: ").strip()
    if not url:
        url = 'http://localhost:3000'
    
    # Ask for auth method
    print("\nAuthentication method:")
    print("1. API Key (recommended for production)")
    print("2. Username/Password")
    choice = input("Choose [1]: ").strip()
    
    if choice == '2':
        # Username/Password auth
        user = input("Username [admin]: ").strip()
        if not user:
            user = 'admin'
        password = input("Password [admin]: ").strip()
        if not password:
            password = 'admin'
        return {
            'url': url,
            'user': user,
            'password': password,
            'api_key': ''
        }
    else:
        # API Key auth
        api_key = input("API Key: ").strip()
        return {
            'url': url,
            'user': '',
            'password': '',
            'api_key': api_key
        }


def load_configuration(config_file: Optional[str] = None) -> Dict[str, str]:
    """Load configuration from file, environment variables, or interactive prompt"""
    
    # Priority 1: Config file
    if config_file:
        logger.info(f"Loading configuration from file: {config_file}")
        config = load_config_file(config_file)
        return {
            'url': config.get('grafana_url', config.get('GRAFANA_URL', 'http://localhost:3000')),
            'user': config.get('grafana_user', config.get('GRAFANA_USER', 'admin')),
            'password': config.get('grafana_password', config.get('GRAFANA_PASSWORD', 'admin')),
            'api_key': config.get('grafana_api_key', config.get('GRAFANA_API_KEY', ''))
        }
    
    # Priority 2: Environment variables
    env_url = os.getenv('GRAFANA_URL')
    env_user = os.getenv('GRAFANA_USER')
    env_password = os.getenv('GRAFANA_PASSWORD')
    env_api_key = os.getenv('GRAFANA_API_KEY')
    
    if env_url or env_user or env_password or env_api_key:
        logger.info("Using configuration from environment variables")
        return {
            'url': env_url or 'http://localhost:3000',
            'user': env_user or 'admin',
            'password': env_password or 'admin',
            'api_key': env_api_key or ''
        }
    
    # Priority 3: Interactive prompt
    logger.info("No configuration provided. Starting interactive mode...")
    return get_interactive_auth()


# ============================================================================
# Dashboard Defaults
# ============================================================================

PROMETHEUS_DATASOURCE = 'prometheus'
PROMETHEUS_UID = 'prometheus'

# Dashboard refresh intervals
DEFAULT_REFRESH = '10s'

# ============================================================================
# Dashboard: Distributed Tracing
# ============================================================================

import os as _os

def load_dashboard_from_json(json_path: str) -> Dict:
    """Load dashboard definition from JSON file"""
    if not _os.path.exists(json_path):
        logger.error(f"Dashboard JSON file not found: {json_path}")
        return None
    try:
        with open(json_path, 'r') as f:
            data = json.load(f)
        # Validate it's a dashboard
        if "dashboard" not in data:
            logger.error(f"Invalid dashboard JSON: missing 'dashboard' key")
            return None
        return data
    except Exception as e:
        logger.error(f"Failed to load dashboard from {json_path}: {e}")
        return None


def create_nats_dashboard() -> Dict:
    """Create NATS Messaging dashboard"""
    dashboard = create_dashboard_base(
        title="NATS-сервер",
        uid="nats-dashboard",
        tags=["nats", "messaging"]
    )

    panels = dashboard["dashboard"]["panels"]
    y = 0

    # gnatsd_* метрики от nats-exporter несут label server_id; app-метрики
    # (l2_proxy_nats_*) — нет. В обоих случаях фильтруем по label vm, как
    # и в остальных дашбордах.
    ts_custom = {"lineWidth": 2, "fillOpacity": 10, "gradientMode": "none"}
    green_only = {"mode": "absolute", "steps": [{"color": "green", "value": None}]}

    # Row 1: Overview
    panels.append(create_row_panel("Обзор NATS-сервера", 1, y))
    y += 1

    # Panel 2: Connections
    panels.append(create_stat_panel(
        title="Подключения",
        id=2,
        x=0, y=y, w=6, h=4,
        unit="short",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 50},
                {"color": "red", "value": 100}
            ]
        },
        targets=[
            {"expr": 'gnatsd_varz_connections{server_id=~".+", vm=~"${vm:regex}"}', "refId": "A"}
        ]
    ))

    # Panel 3: Subscriptions
    panels.append(create_stat_panel(
        title="Подписки",
        id=3,
        x=6, y=y, w=6, h=4,
        unit="short",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 500},
                {"color": "red", "value": 1000}
            ]
        },
        targets=[
            {"expr": 'gnatsd_subsz_num_subscriptions{server_id=~".+", vm=~"${vm:regex}"}', "refId": "A"}
        ]
    ))

    # Panel 4: Total subscriptions
    panels.append(create_stat_panel(
        title="Всего подписок",
        id=4,
        x=12, y=y, w=6, h=4,
        unit="short",
        thresholds=green_only,
        targets=[
            {"expr": 'gnatsd_subsz_total{server_id=~".+", vm=~"${vm:regex}"}', "refId": "A"}
        ]
    ))

    # Panel 5: CPU cores
    panels.append(create_stat_panel(
        title="Ядра CPU",
        id=5,
        x=18, y=y, w=6, h=4,
        unit="short",
        thresholds=green_only,
        targets=[
            {"expr": 'gnatsd_varz_cores{server_id=~".+", vm=~"${vm:regex}"}', "refId": "A"}
        ]
    ))
    y += 4

    # Row 2: Connection statistics
    panels.append(create_row_panel("Статистика подключений", 6, y))
    y += 1

    # Panel 7: Connections over time
    panels.append(create_timeseries_panel(
        title="Подключения во времени",
        id=7,
        x=0, y=y, w=12, h=8,
        unit="short",
        custom=ts_custom,
        targets=[
            {"expr": 'gnatsd_varz_connections{server_id=~".+", vm=~"${vm:regex}"}', "legendFormat": "Подключения", "refId": "A"}
        ]
    ))

    # Panel 8: Connection limit
    panels.append(create_timeseries_panel(
        title="Лимит подключений",
        id=8,
        x=12, y=y, w=12, h=8,
        unit="short",
        custom=ts_custom,
        targets=[
            {"expr": 'gnatsd_connz_limit{server_id=~".+", vm=~"${vm:regex}"}', "legendFormat": "Макс. подключения", "refId": "A"}
        ]
    ))
    y += 8

    # Row 3: Traffic statistics
    panels.append(create_row_panel("Статистика трафика", 9, y))
    y += 1

    # Panel 10: Bytes in/out
    panels.append(create_timeseries_panel(
        title="Байты входящие/исходящие",
        id=10,
        x=0, y=y, w=12, h=8,
        unit="bytes",
        custom=ts_custom,
        targets=[
            {"expr": 'gnatsd_connz_in_bytes{server_id=~".+", vm=~"${vm:regex}"}', "legendFormat": "Вх", "refId": "A"},
            {"expr": 'gnatsd_connz_out_bytes{server_id=~".+", vm=~"${vm:regex}"}', "legendFormat": "Исх", "refId": "B"}
        ]
    ))

    # Panel 11: Messages in/out
    panels.append(create_timeseries_panel(
        title="Сообщения входящие/исходящие",
        id=11,
        x=12, y=y, w=12, h=8,
        unit="short",
        custom=ts_custom,
        targets=[
            {"expr": 'gnatsd_connz_in_msgs{server_id=~".+", vm=~"${vm:regex}"}', "legendFormat": "Вх", "refId": "A"},
            {"expr": 'gnatsd_connz_out_msgs{server_id=~".+", vm=~"${vm:regex}"}', "legendFormat": "Исх", "refId": "B"}
        ]
    ))
    y += 8

    # Row 4: Subscription statistics
    panels.append(create_row_panel("Статистика подписок", 12, y))
    y += 1

    # Panel 13: Subscriptions
    panels.append(create_timeseries_panel(
        title="Подписки",
        id=13,
        x=0, y=y, w=12, h=8,
        unit="short",
        custom=ts_custom,
        targets=[
            {"expr": 'gnatsd_subsz_num_subscriptions{server_id=~".+", vm=~"${vm:regex}"}', "legendFormat": "Активные", "refId": "A"},
            {"expr": 'gnatsd_subsz_total{server_id=~".+", vm=~"${vm:regex}"}', "legendFormat": "Всего", "refId": "B"}
        ]
    ))

    # Panel 14: Subscription cache
    panels.append(create_timeseries_panel(
        title="Кэш подписок",
        id=14,
        x=12, y=y, w=12, h=8,
        unit="short",
        custom=ts_custom,
        targets=[
            {"expr": 'gnatsd_subsz_num_cache{server_id=~".+", vm=~"${vm:regex}"}', "legendFormat": "Размер кэша", "refId": "A"},
            {"expr": 'gnatsd_subsz_cache_hit_rate{server_id=~".+", vm=~"${vm:regex}"}', "legendFormat": "Доля попаданий", "refId": "B"}
        ]
    ))
    y += 8

    # Row 5: NATS application metrics
    panels.append(create_row_panel("Метрики приложения NATS", 15, y))
    y += 1

    # Panel 16: NATS request rate
    panels.append(create_stat_panel(
        title="Скорость NATS-запросов",
        id=16,
        x=0, y=y, w=6, h=4,
        unit="reqps",
        thresholds=green_only,
        targets=[
            {"expr": 'sum(rate(l2_proxy_nats_requests_total{vm=~"${vm:regex}"}[1m]))', "legendFormat": "запросов/с", "refId": "A"}
        ]
    ))

    # Panel 17: NATS error rate
    panels.append(create_stat_panel(
        title="Скорость NATS-ошибок",
        id=17,
        x=6, y=y, w=6, h=4,
        unit="percentunit",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 0.01},
                {"color": "red", "value": 0.05}
            ]
        },
        targets=[
            {"expr": 'sum(rate(l2_proxy_nats_errors_total{vm=~"${vm:regex}"}[5m])) / sum(rate(l2_proxy_nats_requests_total{vm=~"${vm:regex}"}[5m]))', "legendFormat": "Ошибок %", "refId": "A"}
        ]
    ))

    # Panel 19: Connection events (id 18 удалён ранее как мёртвый)
    panels.append(create_stat_panel(
        title="События подключения",
        id=19,
        x=18, y=y, w=6, h=4,
        unit="cps",
        thresholds=green_only,
        targets=[
            {"expr": 'sum(rate(l2_proxy_nats_connection_creates_total{vm=~"${vm:regex}"}[1m]))', "legendFormat": "созданий/с", "refId": "A"}
        ]
    ))
    y += 4

    # Panel 20: NATS request duration
    panels.append(create_timeseries_panel(
        title="NATS длительность запроса",
        id=20,
        x=0, y=y, w=12, h=8,
        unit="s",
        custom=ts_custom,
        targets=[
            {"expr": 'histogram_quantile(0.50, sum(rate(l2_proxy_nats_request_duration_seconds_bucket{vm=~"${vm:regex}"}[5m])) by (le))', "legendFormat": "p50", "refId": "A"},
            {"expr": 'histogram_quantile(0.95, sum(rate(l2_proxy_nats_request_duration_seconds_bucket{vm=~"${vm:regex}"}[5m])) by (le))', "legendFormat": "p95", "refId": "B"},
            {"expr": 'histogram_quantile(0.99, sum(rate(l2_proxy_nats_request_duration_seconds_bucket{vm=~"${vm:regex}"}[5m])) by (le))', "legendFormat": "p99", "refId": "C"}
        ]
    ))

    # Panel 21: NATS requests and errors
    panels.append(create_timeseries_panel(
        title="NATS запросы и ошибки",
        id=21,
        x=12, y=y, w=12, h=8,
        unit="reqps",
        custom=ts_custom,
        targets=[
            {"expr": 'sum(rate(l2_proxy_nats_requests_total{vm=~"${vm:regex}"}[1m]))', "legendFormat": "Запросы/с", "refId": "A"},
            {"expr": 'sum(rate(l2_proxy_nats_errors_total{vm=~"${vm:regex}"}[1m]))', "legendFormat": "Ошибки/с", "refId": "B"}
        ]
    ))
    y += 8

    # Panel 22: NATS connection events over time
    panels.append(create_timeseries_panel(
        title="NATS события подключения во времени",
        id=22,
        x=0, y=y, w=12, h=8,
        unit="cps",
        custom=ts_custom,
        targets=[
            {"expr": 'sum(rate(l2_proxy_nats_connection_creates_total{vm=~"${vm:regex}"}[1m]))', "legendFormat": "Создания/с", "refId": "A"},
            {"expr": 'sum(rate(l2_proxy_nats_connection_errors_total{vm=~"${vm:regex}"}[1m]))', "legendFormat": "Ошибки/с", "refId": "B"}
        ]
    ))

    return dashboard


def create_nginx_dashboard() -> Dict:
    """Load NGINX dashboard from JSON file"""
    json_path = _os.path.join(_os.path.dirname(__file__), 'grafana-dashboards', 'grafana-nginx.json')
    dashboard = load_dashboard_from_json(json_path)
    if dashboard is None:
        dashboard = create_dashboard_base(
            title="NGINX Метрики",
            uid="nginx-metrics",
            tags=["nginx", "web"]
        )
    return dashboard


def create_tracing_dashboard() -> Dict:
    """Create Distributed Tracing dashboard"""
    dashboard = create_dashboard_base(
        title="Распределённая трассировка",
        uid="l2-distributed-tracing",
        tags=["l2-proxy", "tracing", "jaeger"]
    )
    
    panels = dashboard["dashboard"]["panels"]
    y = 0
    
    # Row 1: Tracing Overview
    panels.append(create_row_panel("Обзор трассировки", 1, y))
    y += 1
    
    # Panel 2: Spans Sent vs Failed
    panels.append(create_timeseries_panel(
        title="Спанов отправлено vs ошибок",
        id=2,
        x=0, y=y, w=12, h=8,
        unit="short",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "red", "value": 10}
            ]
        },
        targets=[
            {"expr": "rate(l2_tracing_spans_sent_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Отправлено спанов/с", "refId": "A"},
            {"expr": "rate(l2_tracing_spans_failed_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Ошибки спанов/с", "refId": "B"}
        ]
    ))
    
    # Panel 3: Tracing Queue Size
    panels.append(create_timeseries_panel(
        title="Размер очереди трассировки",
        id=3,
        x=12, y=y, w=12, h=8,
        unit="short",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 5000},
                {"color": "red", "value": 8000}
            ]
        },
        targets=[
            {"expr": "l2_tracing_queue_size{vm=~\"${vm:regex}\"}", "legendFormat": "Размер очереди", "refId": "A"}
        ]
    ))
    y += 8
    
    # Panel 4: Spans Failed Rate
    panels.append(create_timeseries_panel(
        title="Скорость ошибок спанов",
        id=4,
        x=0, y=y, w=12, h=8,
        unit="short",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 10},
                {"color": "red", "value": 50}
            ]
        },
        targets=[
            {"expr": "rate(l2_tracing_spans_failed_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Ошибки спанов/с", "refId": "A"}
        ]
    ))
    
    # Panel 5: Last Send Duration
    panels.append(create_timeseries_panel(
        title="Длительность последней отправки",
        id=5,
        x=12, y=y, w=12, h=8,
        unit="s",
        targets=[
            {"expr": "l2_tracing_last_send_duration_seconds{vm=~\"${vm:regex}\"}", "legendFormat": "Длительность последней отправки", "refId": "A"}
        ]
    ))
    y += 8
    
    # Row 2: Tracing Latency
    panels.append(create_row_panel("Задержки трассировки", 10, y))
    y += 1
    
    # Panel 11: Span Send Latency
    panels.append(create_timeseries_panel(
        title="Задержка отправки спанов",
        id=11,
        x=0, y=y, w=12, h=8,
        unit="s",
        targets=[
            {"expr": "histogram_quantile(0.50, rate(l2_tracing_send_latency_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "Задержка отправки p50", "refId": "A"},
            {"expr": "histogram_quantile(0.95, rate(l2_tracing_send_latency_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "Задержка отправки p95", "refId": "B"},
            {"expr": "histogram_quantile(0.99, rate(l2_tracing_send_latency_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "Задержка отправки p99", "refId": "C"}
        ]
    ))
    
    # Panel 12: Queue Time
    panels.append(create_timeseries_panel(
        title="Время спана в очереди",
        id=12,
        x=12, y=y, w=12, h=8,
        unit="s",
        targets=[
            {"expr": "histogram_quantile(0.50, rate(l2_tracing_queue_time_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "Время в очереди p50", "refId": "A"},
            {"expr": "histogram_quantile(0.95, rate(l2_tracing_queue_time_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "Время в очереди p95", "refId": "B"},
            {"expr": "histogram_quantile(0.99, rate(l2_tracing_queue_time_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "Время в очереди p99", "refId": "C"}
        ]
    ))
    y += 8
    
    return dashboard

# ============================================================================
# Dashboard Definitions
# ============================================================================

def create_dashboard_base(title: str, uid: str, tags: List[str]) -> Dict:
    """Create base dashboard structure"""
    return {
        "dashboard": {
            "id": None,
            "uid": uid,
            "title": title,
            "tags": tags,
            "timezone": "browser",
            "schemaVersion": 38,
            "version": 0,
            "refresh": DEFAULT_REFRESH,
            "fiscalYearStartMonth": 0,
            "graphTooltip": 0,
            "links": [],
            "liveNow": False,
            "panels": [],
            "templating": {
                "list": [
                    {
                        "name": "vm",
                        "label": "Виртуальная машина",
                        "description": "Имя узла, на котором развёрнут стек. Label 'vm' добавляет vmagent при скрейпе из env VM_NAME (по умолчанию — hostname узла, см. rebuild-and-run.sh). На всех досках метрики показываются только одной ВМ.",
                        "type": "query",
                        "datasource": {
                            "type": "prometheus",
                            "uid": PROMETHEUS_UID
                        },
                        "definition": "label_values(up, vm)",
                        "refresh": 2,
                        "regex": "",
                        "sort": 1,
                        "multi": False,
                        "includeAll": False,
                        "current": {},
                        "hide": 0,
                        "options": [],
                        "query": {
                            "query": "label_values(up, vm)",
                            "refId": "PrometheusVariableQueryEditor-VariableQuery"
                        }
                    }
                ]
            },
            "time": {
                "from": "now-1h",
                "to": "now"
            },
            "timepicker": {},
            "annotations": {
                "list": [
                    {
                        "builtIn": 1,
                        "datasource": {
                            "type": "grafana",
                            "uid": "-- Grafana --"
                        },
                        "enable": True,
                        "hide": True,
                        "iconColor": "rgba(0, 211, 255, 1)",
                        "name": "Annotations & Alerts",
                        "type": "dashboard"
                    }
                ]
            },
            "editable": True,
            "style": "dark"
        },
        "overwrite": True
    }


def create_row_panel(title: str, id: int, y: int) -> Dict:
    """Create a row panel for grouping"""
    return {
        "id": id,
        "gridPos": {"h": 1, "w": 24, "x": 0, "y": y},
        "type": "row",
        "title": title,
        "collapsed": False,
        "panels": []
    }


def create_timeseries_panel(
    title: str,
    id: int,
    x: int, y: int, w: int, h: int,
    targets: List[Dict],
    unit: str = "short",
    thresholds: Optional[List[Dict]] = None,
    custom: Optional[Dict] = None
) -> Dict:
    """Create a time series panel"""
    
    if thresholds is None:
        thresholds = {
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None}
            ]
        }
    
    field_defaults = {
        "color": {"mode": "palette-classic"},
        "mappings": [],
        "thresholds": thresholds,
        "unit": unit
    }
    if custom is not None:
        field_defaults["custom"] = custom
    
    return {
        "id": id,
        "type": "timeseries",
        "title": title,
        "gridPos": {"h": h, "w": w, "x": x, "y": y},
        "datasource": {
            "type": "prometheus",
            "uid": PROMETHEUS_UID
        },
        "fieldConfig": {
            "defaults": field_defaults,
            "overrides": []
        },
        "options": {
            "legend": {
                "displayMode": "list",
                "placement": "bottom",
                "showLegend": True
            },
            "tooltip": {
                "mode": "single",
                "sort": "none"
            }
        },
        "targets": targets
    }


def create_stat_panel(
    title: str,
    id: int,
    x: int, y: int, w: int, h: int,
    targets: List[Dict],
    unit: str = "short",
    color_mode: str = "value",
    thresholds: Optional[List[Dict]] = None
) -> Dict:
    """Create a stat panel"""
    if thresholds is None:
        thresholds = {
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "red", "value": 80}
            ]
        }
        field_color_mode = color_mode
    else:
        field_color_mode = "thresholds"
    return {
        "id": id,
        "type": "stat",
        "title": title,
        "gridPos": {"h": h, "w": w, "x": x, "y": y},
        "datasource": {
            "type": "prometheus",
            "uid": PROMETHEUS_UID
        },
        "fieldConfig": {
            "defaults": {
                "color": {"mode": field_color_mode},
                "mappings": [],
                "thresholds": thresholds,
                "unit": unit
            },
            "overrides": []
        },
        "options": {
            "colorMode": color_mode,
            "graphMode": "area",
            "justifyMode": "auto",
            "orientation": "auto",
            "reduceOptions": {
                "calcs": ["lastNotNull"],
                "fields": "",
                "values": False
            },
            "textMode": "auto"
        },
        "targets": targets
    }


def create_bargauge_panel(
    title: str,
    id: int,
    x: int, y: int, w: int, h: int,
    targets: List[Dict],
    unit: str = "short",
    thresholds: Optional[List[Dict]] = None,
    display_mode: str = "gradient",
    orientation: str = "auto"
) -> Dict:
    """Create a bar gauge panel (e.g. hot clients colored by thresholds)"""
    if thresholds is None:
        thresholds = {
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None}
            ]
        }
    return {
        "id": id,
        "type": "bargauge",
        "title": title,
        "gridPos": {"h": h, "w": w, "x": x, "y": y},
        "datasource": {
            "type": "prometheus",
            "uid": PROMETHEUS_UID
        },
        "fieldConfig": {
            "defaults": {
                "color": {"mode": "thresholds"},
                "mappings": [],
                "thresholds": thresholds,
                "unit": unit
            },
            "overrides": []
        },
        "options": {
            "displayMode": display_mode,
            "legend": {
                "displayMode": "list",
                "placement": "bottom",
                "showLegend": True
            },
            "orientation": orientation,
            "reduceOptions": {
                "calcs": ["lastNotNull"],
                "fields": "",
                "values": False
            },
            "showUnfilled": True,
            "text": {"valueSize": 24}
        },
        "targets": targets
    }



# ============================================================================
# Dashboard: SLO Tracking
# ============================================================================

def create_slo_dashboard() -> Dict:
    """Create SLO (Service Level Objective) dashboard"""
    dashboard = create_dashboard_base(
        title="SLO (уровень обслуживания)",
        uid="l2-slo-tracking",
        tags=["l2-proxy", "slo", "reliability", "error-budget"]
    )

    panels = dashboard["dashboard"]["panels"]
    y = 0

    # Row 1: SLO Overview
    panels.append(create_row_panel("Обзор SLO", 1, y))
    y += 1

    # Panel 2: Availability SLO (99.9%)
    panels.append(create_stat_panel(
        title="Доступность SLO (99.9%)",
        id=2,
        x=0, y=y, w=6, h=5,
        unit="percentunit",
        targets=[
            {"expr": "1 - (sum(rate(l2_proxy_client_request_errors_total{vm=~\"${vm:regex}\"}[1h])) / sum(rate(l2_proxy_client_requests_total{vm=~\"${vm:regex}\"}[1h])))", "legendFormat": "Доступность 1ч", "refId": "A"}
        ],
        color_mode="value"
    ))

    # Panel 3: Latency P95 SLO (< 50ms)
    panels.append(create_stat_panel(
        title="Задержка P95 SLO (< 50мс)",
        id=3,
        x=6, y=y, w=6, h=5,
        unit="s",
        targets=[
            {"expr": "histogram_quantile(0.95, rate(l2_proxy_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[30m]))", "legendFormat": "Задержка P95 30м", "refId": "A"}
        ],
        color_mode="value"
    ))

    # Panel 4: Latency P99 SLO (< 100ms)
    panels.append(create_stat_panel(
        title="Задержка P99 SLO (< 100мс)",
        id=4,
        x=12, y=y, w=6, h=5,
        unit="s",
        targets=[
            {"expr": "histogram_quantile(0.99, rate(l2_proxy_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "Задержка P99 5м", "refId": "A"}
        ],
        color_mode="value"
    ))

    # Panel 5: Error Rate SLO (< 1%)
    panels.append(create_stat_panel(
        title="Доля ошибок SLO (< 1%)",
        id=5,
        x=18, y=y, w=6, h=5,
        unit="percentunit",
        targets=[
            {"expr": "sum(rate(l2_proxy_client_request_errors_total{vm=~\"${vm:regex}\"}[5m])) / sum(rate(l2_proxy_client_requests_total{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "Доля ошибок 5м", "refId": "A"}
        ],
        color_mode="value"
    ))
    y += 8

    # Row 2: Error Budget
    panels.append(create_row_panel("Бюджет ошибок", 10, y))
    y += 1

    # Panel 11: Error Budget Remaining (Availability)
    panels.append(create_timeseries_panel(
        title="Оставшийся бюджет ошибок — доступность",
        id=11,
        x=0, y=y, w=12, h=8,
        unit="percentunit",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 0.5},
                {"color": "red", "value": 0.1}
            ]
        },
        targets=[
            {"expr": "(0.001 - (1 - (1 - (sum(rate(l2_proxy_client_request_errors_total{vm=~\"${vm:regex}\"}[24h])) / sum(rate(l2_proxy_client_requests_total{vm=~\"${vm:regex}\"}[24h])))))) / 0.001", "legendFormat": "Оставшийся бюджет ошибок", "refId": "A"},
            {"expr": "0", "legendFormat": "Бюджет исчерпан", "refId": "B"}
        ]
    ))

    # Panel 12: Error Budget Burn Rate
    panels.append(create_timeseries_panel(
        title="Скорость сгорания бюджета ошибок",
        id=12,
        x=12, y=y, w=12, h=8,
        unit="short",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 5},
                {"color": "red", "value": 20}
            ]
        },
        targets=[
            {"expr": "(sum(rate(l2_proxy_client_request_errors_total{vm=~\"${vm:regex}\"}[5m])) / sum(rate(l2_proxy_client_requests_total{vm=~\"${vm:regex}\"}[5m]))) / 0.001", "legendFormat": "Сгорание бюджета 5м", "refId": "A"},
            {"expr": "(sum(rate(l2_proxy_client_request_errors_total{vm=~\"${vm:regex}\"}[30m])) / sum(rate(l2_proxy_client_requests_total{vm=~\"${vm:regex}\"}[30m]))) / 0.001", "legendFormat": "Сгорание бюджета 30м", "refId": "B"}
        ]
    ))
    y += 8

    # Row 3: Availability Over Time
    panels.append(create_row_panel("Доступность во времени", 20, y))
    y += 1

    # Panel 21: Availability % (1h, 6h, 24h)
    panels.append(create_timeseries_panel(
        title="Доступность % во времени",
        id=21,
        x=0, y=y, w=24, h=8,
        unit="percentunit",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "red", "value": None},
                {"color": "yellow", "value": 0.999},
                {"color": "green", "value": 0.9999}
            ]
        },
        targets=[
            {"expr": "1 - (sum(rate(l2_proxy_client_request_errors_total{vm=~\"${vm:regex}\"}[1h])) / sum(rate(l2_proxy_client_requests_total{vm=~\"${vm:regex}\"}[1h])))", "legendFormat": "Доступность 1ч", "refId": "A"},
            {"expr": "1 - (sum(rate(l2_proxy_client_request_errors_total{vm=~\"${vm:regex}\"}[6h])) / sum(rate(l2_proxy_client_requests_total{vm=~\"${vm:regex}\"}[6h])))", "legendFormat": "Доступность 6ч", "refId": "B"},
            {"expr": "1 - (sum(rate(l2_proxy_client_request_errors_total{vm=~\"${vm:regex}\"}[24h])) / sum(rate(l2_proxy_client_requests_total{vm=~\"${vm:regex}\"}[24h])))", "legendFormat": "Доступность 24ч", "refId": "C"}
        ]
    ))
    y += 8

    # Row 4: Latency Over Time
    panels.append(create_row_panel("Задержки во времени", 30, y))
    y += 1

    # Panel 31: P50, P95, P99 Latency
    panels.append(create_timeseries_panel(
        title="Перцентили задержки",
        id=31,
        x=0, y=y, w=12, h=8,
        unit="s",
        targets=[
            {"expr": "histogram_quantile(0.50, rate(l2_proxy_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "Задержка P50", "refId": "A"},
            {"expr": "histogram_quantile(0.95, rate(l2_proxy_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "Задержка P95", "refId": "B"},
            {"expr": "histogram_quantile(0.99, rate(l2_proxy_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "Задержка P99", "refId": "C"}
        ]
    ))

    # Panel 32: Requests per Second
    panels.append(create_timeseries_panel(
        title="Запросы в секунду",
        id=32,
        x=12, y=y, w=12, h=8,
        unit="reqps",
        targets=[
            {"expr": "sum(rate(l2_proxy_client_requests_total{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "Всего запросов/с", "refId": "A"},
            {"expr": "sum(rate(l2_proxy_client_request_errors_total{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "Ошибок/с", "refId": "B"}
        ]
    ))
    y += 8

    # Row 5: SLO Compliance
    panels.append(create_row_panel("Соответствие SLO", 40, y))
    y += 1

    # Panel 41: SLO Compliance - Availability
    panels.append(create_timeseries_panel(
        title="Соответствие SLO — доступность",
        id=41,
        x=0, y=y, w=12, h=6,
        unit="percentunit",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "red", "value": None},
                {"color": "green", "value": 0.999}
            ]
        },
        targets=[
            {"expr": "1 - (sum(rate(l2_proxy_client_request_errors_total{vm=~\"${vm:regex}\"}[1h])) / sum(rate(l2_proxy_client_requests_total{vm=~\"${vm:regex}\"}[1h])))", "legendFormat": "Доступность", "refId": "A"},
            {"expr": "0.999", "legendFormat": "Цель SLO (99.9%)", "refId": "B"}
        ]
    ))

    # Panel 42: SLO Compliance - Latency P95
    panels.append(create_timeseries_panel(
        title="Соответствие SLO — задержка P95",
        id=42,
        x=12, y=y, w=12, h=6,
        unit="s",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "red", "value": 0.05}
            ]
        },
        targets=[
            {"expr": "histogram_quantile(0.95, rate(l2_proxy_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[30m]))", "legendFormat": "Задержка P95", "refId": "A"},
            {"expr": "0.05", "legendFormat": "Цель SLO (50мс)", "refId": "B"}
        ]
    ))

    # Panel 43: SLO Compliance - Error Rate
    panels.append(create_timeseries_panel(
        title="Соответствие SLO — доля ошибок",
        id=43,
        x=0, y=y, w=12, h=6,
        unit="percentunit",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "red", "value": 0.01}
            ]
        },
        targets=[
            {"expr": "sum(rate(l2_proxy_client_request_errors_total{vm=~\"${vm:regex}\"}[5m])) / sum(rate(l2_proxy_client_requests_total{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "Доля ошибок", "refId": "A"},
            {"expr": "0.01", "legendFormat": "Цель SLO (1%)", "refId": "B"}
        ]
    ))

    # Panel 44: SLO Compliance - Latency P99
    panels.append(create_timeseries_panel(
        title="Соответствие SLO — задержка P99",
        id=44,
        x=12, y=y, w=12, h=6,
        unit="s",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "red", "value": 0.1}
            ]
        },
        targets=[
            {"expr": "histogram_quantile(0.99, rate(l2_proxy_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "Задержка P99", "refId": "A"},
            {"expr": "0.1", "legendFormat": "Цель SLO (100мс)", "refId": "B"}
        ]
    ))
    y += 6

    return dashboard


# ============================================================================
# Grafana API Client
# ============================================================================

class GrafanaAPI:
    """Grafana API client for dashboard management"""

    def __init__(self, base_url: str, api_key: str = "", user: str = "", password: str = ""):
        self.base_url = base_url.rstrip('/')
        self.session = requests.Session()

        if api_key:
            self.session.headers['Authorization'] = f'Bearer {api_key}'
        else:
            self.session.auth = (user, password)

    def test_connection(self) -> bool:
        """Test Grafana connection"""
        try:
            response = self.session.get(f'{self.base_url}/api/health')
            response.raise_for_status()
            data = response.json()
            logger.info(f"Grafana connection successful: {data.get('commit', 'unknown')}")
            return True
        except Exception as e:
            logger.error(f"Failed to connect to Grafana: {e}")
            return False

    def get_dashboard(self, uid: str) -> Optional[Dict]:
        """Get dashboard by UID"""
        try:
            response = self.session.get(f'{self.base_url}/api/dashboards/uid/{uid}')
            if response.status_code == 404:
                return None
            response.raise_for_status()
            return response.json()
        except Exception as e:
            logger.error(f"Failed to get dashboard {uid}: {e}")
            return None

    def save_dashboard(self, dashboard: Dict) -> bool:
        """Save/create dashboard"""
        try:
            # Overwrite ignores the stored version and avoids HTTP 412
            # "version-mismatch" when the static JSON export (grafana-nginx.json)
            # is older than the dashboard already in Grafana.
            dashboard['overwrite'] = True
            response = self.session.post(
                f'{self.base_url}/api/dashboards/db',
                json=dashboard
            )
            response.raise_for_status()
            result = response.json()
            logger.info(f"Dashboard saved: {result.get('uid')} - {result.get('url', '')}")
            return True
        except Exception as e:
            logger.error(f"Failed to save dashboard: {e}")
            if hasattr(e, 'response') and e.response is not None:
                logger.error(f"Response: {e.response.text}")
            return False

    def delete_dashboard(self, uid: str) -> bool:
        """Delete dashboard by UID"""
        try:
            response = self.session.delete(f'{self.base_url}/api/dashboards/uid/{uid}')
            response.raise_for_status()
            logger.info(f"Dashboard deleted: {uid}")
            return True
        except Exception as e:
            logger.error(f"Failed to delete dashboard {uid}: {e}")
            return False


# ============================================================================
# Prometheus API Client for Metric Discovery
# ============================================================================

class PrometheusAPI:
    """Prometheus API client for metric discovery"""

    def __init__(self, base_url: str):
        self.base_url = base_url.rstrip('/')
        self.session = requests.Session()

    def test_connection(self) -> bool:
        """Test Prometheus connection"""
        try:
            response = self.session.get(f'{self.base_url}/-/healthy')
            response.raise_for_status()
            logger.info("Prometheus connection successful")
            return True
        except Exception as e:
            logger.warning(f"Failed to connect to Prometheus: {e}")
            return False

    def get_all_metrics(self) -> List[str]:
        """Get all available metric names from Prometheus"""
        try:
            response = self.session.get(f'{self.base_url}/api/v1/label/__name__/values')
            response.raise_for_status()
            data = response.json()
            metrics = data.get('data', [])
            return [m for m in metrics if m.startswith('l2_')]
        except Exception as e:
            logger.error(f"Failed to get metrics from Prometheus: {e}")
            return []

    def get_metric_samples(self, metric_name: str, limit: int = 5) -> List[Dict]:
        """Get sample data for a specific metric"""
        try:
            response = self.session.get(
                f'{self.base_url}/api/v1/query',
                params={'query': metric_name, 'limit': limit}
            )
            response.raise_for_status()
            data = response.json()
            return data.get('data', {}).get('result', [])
        except Exception as e:
            logger.error(f"Failed to get samples for {metric_name}: {e}")
            return []


# ============================================================================
# Dashboard Correction and Creation Logic
# ============================================================================

def discover_new_metrics(prometheus_api: PrometheusAPI, known_metrics: List[str]) -> List[str]:
    """Discover new metrics that are not yet in dashboards"""
    all_metrics = prometheus_api.get_all_metrics()
    new_metrics = [m for m in all_metrics if m not in known_metrics]
    return new_metrics


def categorize_metric(metric_name: str) -> str:
    """Categorize a metric by its prefix (proxy, worker, tracing, etc.)"""
    if metric_name.startswith('l2_proxy_'):
        return 'proxy'
    elif metric_name.startswith('l2_worker_'):
        return 'worker'
    elif metric_name.startswith('l2_tracing_'):
        return 'tracing'
    elif metric_name.startswith('l2_endpoint_'):
        return 'endpoint'
    elif metric_name.startswith('l2_http_'):
        return 'connection_pool'
    elif metric_name.startswith('l2_rate_limiter_'):
        return 'resilience'
    else:
        return 'unknown'


def get_existing_dashboard_metrics(dashboard: Dict) -> List[str]:
    """Extract all metric names from a dashboard's panels"""
    metrics = []
    for panel in dashboard.get('dashboard', {}).get('panels', []):
        for target in panel.get('targets', []):
            expr = target.get('expr', '')
            # Extract metric names from PromQL expressions
            import re
            # Match metric names (alphanumeric and underscore)
            found = re.findall(r'\b(l2_[a-z_]+)\b', expr)
            metrics.extend(found)
    return list(set(metrics))


def create_metric_panel_for_new_metric(metric_name: str, panel_id: int, x: int, y: int) -> Optional[Dict]:
    """Create a time series panel for a newly discovered metric"""
    category = categorize_metric(metric_name)
    
    # Determine unit based on metric name
    unit = "short"
    if 'bytes' in metric_name:
        unit = "bytes"
    elif 'duration' in metric_name or 'latency' in metric_name or 'time' in metric_name:
        unit = "s"
    elif '_total' in metric_name or 'rate' in metric_name:
        unit = "reqps"
    
    # Create legend format
    legend_format = metric_name.replace('l2_proxy_', '').replace('l2_worker_', '').replace('l2_tracing_', '')
    
    # Check if it's a histogram metric
    if '_bucket{' in metric_name or 'histogram_quantile' in metric_name:
        # Create percentile panels
        targets = [
            {"expr": f"histogram_quantile(0.50, rate({metric_name}_bucket{{vm=~\"${{vm:regex}}\"}}[5m]))", "legendFormat": f"{legend_format} p50", "refId": "A"},
            {"expr": f"histogram_quantile(0.95, rate({metric_name}_bucket{{vm=~\"${{vm:regex}}\"}}[5m]))", "legendFormat": f"{legend_format} p95", "refId": "B"},
            {"expr": f"histogram_quantile(0.99, rate({metric_name}_bucket{{vm=~\"${{vm:regex}}\"}}[5m]))", "legendFormat": f"{legend_format} p99", "refId": "C"}
        ]
    else:
        # Simple rate or gauge
        if '_total' in metric_name:
            expr = f"rate({metric_name}{{vm=~\"${{vm:regex}}\"}}[1m])"
        else:
            expr = f"{metric_name}{{vm=~\"${{vm:regex}}\"}}"
        
        targets = [
            {"expr": expr, "legendFormat": legend_format, "refId": "A"}
        ]
    
    return create_timeseries_panel(
        title=metric_name.replace('_', ' ').title(),
        id=panel_id,
        x=x, y=y, w=12, h=8,
        unit=unit,
        targets=targets
    )


def correct_dashboard_panels(api: GrafanaAPI, dashboard_func, dashboard_uid: str) -> bool:
    """Correct an existing dashboard by comparing with generated version with detailed diagnostics"""
    try:
        import re
        
        # Get existing dashboard
        existing = api.get_dashboard(dashboard_uid)
        if not existing:
            logger.info(f"Dashboard {dashboard_uid} does not exist, will create new")
            return api.save_dashboard(dashboard_func())
        
        # Get new dashboard definition
        new_dashboard = dashboard_func()
        
        existing_panels = existing.get('dashboard', {}).get('panels', [])
        new_panels = new_dashboard.get('dashboard', {}).get('panels', [])
        
        # Extract metrics from both versions
        existing_metrics = set(get_existing_dashboard_metrics(existing))
        new_metrics = set(get_existing_dashboard_metrics(new_dashboard))
        
        # Calculate differences
        added_metrics = new_metrics - existing_metrics
        removed_metrics = existing_metrics - new_metrics
        common_metrics = existing_metrics & new_metrics
        
        # Count panels by type
        existing_rows = sum(1 for p in existing_panels if p.get('type') == 'row')
        new_rows = sum(1 for p in new_panels if p.get('type') == 'row')
        existing_visual_panels = len(existing_panels) - existing_rows
        new_visual_panels = len(new_panels) - new_rows
        
        # Print detailed diagnostics
        print(f"\n{'='*60}")
        print(f"Dashboard: {new_dashboard['dashboard']['title']}")
        print(f"UID: {dashboard_uid}")
        print(f"{'='*60}")
        
        # Panel changes
        if len(existing_panels) != len(new_panels):
            print(f"\n📊 Panel Changes:")
            print(f"   Total panels: {len(existing_panels)} -> {len(new_panels)} ({len(new_panels) - len(existing_panels):+d})")
            print(f"   Row panels: {existing_rows} -> {new_rows}")
            print(f"   Visualization panels: {existing_visual_panels} -> {new_visual_panels}")
            
            # Find added/removed panels by title
            existing_titles = set(p.get('title', '') for p in existing_panels if p.get('type') != 'row')
            new_titles = set(p.get('title', '') for p in new_panels if p.get('type') != 'row')
            added_panels = new_titles - existing_titles
            removed_panels = existing_titles - new_titles
            
            if added_panels:
                print(f"\n   ✨ Added panels:")
                for title in sorted(added_panels):
                    print(f"      + {title}")
            
            if removed_panels:
                print(f"\n   🗑️  Removed panels:")
                for title in sorted(removed_panels):
                    print(f"      - {title}")
        
        # Metric changes
        if added_metrics or removed_metrics:
            print(f"\n📈 Metric Changes:")
            
            if added_metrics:
                print(f"\n   ✨ New metrics ({len(added_metrics)}):")
                for metric in sorted(added_metrics):
                    category = categorize_metric(metric)
                    print(f"      + {metric} ({category})")
            
            if removed_metrics:
                print(f"\n   🗑️  Removed metrics ({len(removed_metrics)}):")
                for metric in sorted(removed_metrics):
                    print(f"      - {metric}")
            
            if common_metrics:
                print(f"\n   ⚡ Unchanged metrics: {len(common_metrics)}")
        else:
            print(f"\n✅ Dashboard is up to date")
            print(f"   Panels: {len(new_panels)} ({new_rows} rows + {new_visual_panels} visualizations)")
            print(f"   Metrics: {len(new_metrics)} total")
        
        print(f"{'='*60}\n")
        
        # Determine if update is needed
        needs_update = (len(existing_panels) != len(new_panels)) or (existing_metrics != new_metrics)
        
        if needs_update:
            if api.save_dashboard(new_dashboard):
                print(f"✅ Dashboard updated successfully")
                return True
            else:
                print(f"❌ Failed to update dashboard")
                return False
        else:
            return True
            
    except Exception as e:
        logger.error(f"Failed to correct dashboard {dashboard_uid}: {e}")
        return False


def discover_and_create_dashboards(api: GrafanaAPI, prometheus_api: PrometheusAPI) -> List[str]:
    """Discover new metrics and create dashboards for them if needed"""
    created_dashboards = []
    
    # Get all metrics from Prometheus
    all_metrics = prometheus_api.get_all_metrics()
    if not all_metrics:
        logger.warning("No metrics found in Prometheus")
        return created_dashboards
    
    logger.info(f"Found {len(all_metrics)} L2 metrics in Prometheus")
    
    # Group metrics by category
    metrics_by_category = {}
    for metric in all_metrics:
        category = categorize_metric(metric)
        if category not in metrics_by_category:
            metrics_by_category[category] = []
        metrics_by_category[category].append(metric)
    
    # Check for new categories that don't have dashboards yet
    existing_dashboards = []  # We'll get this from Grafana
    try:
        response = api.session.get(f'{api.base_url}/api/search?type=dash-db')
        response.raise_for_status()
        existing_dashboards = [d['uid'] for d in response.json()]
    except Exception as e:
        logger.error(f"Failed to get existing dashboards: {e}")
    
    # Known dashboard UIDs
    known_uids = {
        'l2-distributed-tracing'
    }
    
    # Check for new metric categories
    for category, metrics in metrics_by_category.items():
        # This is a simplified check - in reality you'd want more sophisticated logic
        pass
    
    return created_dashboards


# ============================================================================
# Dashboard: L2 Proxy (traffic, NATS, connection pool, rate limiting)
# ============================================================================

def create_proxy_dashboard() -> Dict:
    """Create L2 Proxy dashboard covering all proxy-mode metrics"""
    dashboard = create_dashboard_base(
        title="L2 Прокси",
        uid="l2-proxy",
        tags=["l2-proxy", "nats", "rate-limiting", "http-pool"]
    )
    
    panels = dashboard["dashboard"]["panels"]
    y = 0
    
    # Row 0: Hot clients & per-client distribution (X-DataHub-Client-Id) — на
    # самом верху, чтобы хот-клиенты были видны без прокрутки.
    panels.append(create_row_panel("Хот-клиенты (X-DataHub-Client-Id)", 5, y))
    y += 1

    # Panel 71: Hot clients bar gauge — top client-ids by request rate,
    # normalized to the hottest client (1.0). The hotter the client, the redder
    # the bar: green < 0.5, yellow >= 0.5, red >= 0.9. The load test emulates
    # hot clients via --hot-clients/--hot-share (hot-client-N ids).
    panels.append(create_bargauge_panel(
        title="Хот-клиенты (нормированная нагрузка client-id)",
        id=71,
        x=0, y=y, w=24, h=6,
        unit="percentunit",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 0.5},
                {"color": "red", "value": 0.9}
            ]
        },
        targets=[
            {"expr": "topk(10, rate(l2_proxy_per_client_id_requests_total{vm=~\"${vm:regex}\",client_id!=\"unknown\"}[5m]) / clamp_min(scalar(max(rate(l2_proxy_per_client_id_requests_total{vm=~\"${vm:regex}\",client_id!=\"unknown\"}[5m]))), 1e-6))", "legendFormat": "{{client_id}}", "refId": "A"}
        ]
    ))
    y += 6

    # Panel 67: Top client-ids by request rate (X-DataHub-Client-Id header —
    # различает клиентов, работающих из-под одного IP)
    panels.append(create_timeseries_panel(
        title="Топ client-id по запросам",
        id=67,
        x=0, y=y, w=12, h=8,
        unit="reqps",
        targets=[
            {"expr": "topk(10, rate(l2_proxy_per_client_id_requests_total{vm=~\"${vm:regex}\",client_id!=\"unknown\"}[5m]))", "legendFormat": "{{client_id}}", "refId": "A"}
        ]
    ))

    # Panel 68: Top client-ids by rejections
    panels.append(create_timeseries_panel(
        title="Топ client-id по отказам",
        id=68,
        x=12, y=y, w=12, h=8,
        unit="reqps",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 1},
                {"color": "red", "value": 10}
            ]
        },
        targets=[
            {"expr": "topk(10, rate(l2_proxy_per_client_id_rejected_total{vm=~\"${vm:regex}\",client_id!=\"unknown\"}[5m]))", "legendFormat": "{{client_id}}", "refId": "A"}
        ]
    ))
    y += 8

    # Panel 69: Top client-ids by p95 latency (histogram per client_id)
    panels.append(create_timeseries_panel(
        title="Топ client-id по задержке p95",
        id=69,
        x=0, y=y, w=12, h=8,
        unit="s",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 0.5},
                {"color": "red", "value": 1.0}
            ]
        },
        targets=[
            {"expr": "topk(10, histogram_quantile(0.95, sum(rate(l2_proxy_per_client_id_latency_seconds_bucket{vm=~\"${vm:regex}\",client_id!=\"unknown\"}[5m])) by (le, client_id)))", "legendFormat": "{{client_id}}", "refId": "A"}
        ]
    ))

    # Panel 70: Top client-ids by rejection ratio (rejected / total requests)
    panels.append(create_timeseries_panel(
        title="Доля отказов client-id",
        id=70,
        x=12, y=y, w=12, h=8,
        unit="percentunit",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 0.05},
                {"color": "red", "value": 0.2}
            ]
        },
        targets=[
            {"expr": "topk(10, rate(l2_proxy_per_client_id_rejected_total{vm=~\"${vm:regex}\",client_id!=\"unknown\"}[5m]) / clamp_min(rate(l2_proxy_per_client_id_requests_total{vm=~\"${vm:regex}\",client_id!=\"unknown\"}[5m]), 0.0001))", "legendFormat": "{{client_id}}", "refId": "A"}
        ]
    ))
    y += 8

    # Panel 72: Top client-ids by duplicate POST bodies detected (same body
    # hash seen again within the detector TTL window — retry storms per client).
    panels.append(create_timeseries_panel(
        title="Топ client-id по дублям POST-тел",
        id=72,
        x=0, y=y, w=24, h=8,
        unit="reqps",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 1},
                {"color": "red", "value": 10}
            ]
        },
        targets=[
            {"expr": "topk(10, rate(l2_proxy_per_client_id_duplicate_requests_total{vm=~\"${vm:regex}\",client_id!=\"unknown\"}[5m]))", "legendFormat": "{{client_id}}", "refId": "A"}
        ]
    ))
    y += 8

    # Row 1: Traffic
    panels.append(create_row_panel("Трафик", 1, y))
    y += 1
    
    # Panel 2: Client Requests
    panels.append(create_timeseries_panel(
        title="Запросы клиентов",
        id=2,
        x=0, y=y, w=8, h=8,
        unit="reqps",
        targets=[
            {"expr": "rate(l2_proxy_client_requests_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Запросы/с", "refId": "A"}
        ]
    ))
    
    # Panel 3: Client Request Errors
    panels.append(create_timeseries_panel(
        title="Ошибки запросов клиентов",
        id=3,
        x=8, y=y, w=8, h=8,
        unit="reqps",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 1},
                {"color": "red", "value": 10}
            ]
        },
        targets=[
            {"expr": "rate(l2_proxy_client_request_errors_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Ошибки/с", "refId": "A"}
        ]
    ))
    
    # Panel 4: Duplicate Requests
    panels.append(create_timeseries_panel(
        title="Дублирующиеся запросы",
        id=4,
        x=16, y=y, w=8, h=8,
        unit="reqps",
        targets=[
            {"expr": "rate(l2_proxy_duplicate_requests_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "NATS re-send/с", "refId": "A"},
            {"expr": "rate(l2_proxy_duplicate_posts_detected_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Дубл. POST-тела/с", "refId": "B"}
        ]
    ))
    y += 8
    
    # Row 2: Bytes
    panels.append(create_row_panel("Байты", 10, y))
    y += 1
    
    # Panel 11: Bytes Received
    panels.append(create_timeseries_panel(
        title="Получено байт",
        id=11,
        x=0, y=y, w=12, h=8,
        unit="Bps",
        targets=[
            {"expr": "rate(l2_proxy_bytes_received_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Получено Б/с", "refId": "A"}
        ]
    ))
    
    # Panel 12: Bytes Sent
    panels.append(create_timeseries_panel(
        title="Отправлено байт",
        id=12,
        x=12, y=y, w=12, h=8,
        unit="Bps",
        targets=[
            {"expr": "rate(l2_proxy_bytes_sent_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Отправлено Б/с", "refId": "A"}
        ]
    ))
    y += 8
    
    # Row 3: Latency & Sizes
    panels.append(create_row_panel("Задержки и размеры", 20, y))
    y += 1
    
    # Panel 21: Request Duration
    panels.append(create_timeseries_panel(
        title="Длительность запроса",
        id=21,
        x=0, y=y, w=12, h=8,
        unit="s",
        targets=[
            {"expr": "histogram_quantile(0.50, rate(l2_proxy_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p50", "refId": "A"},
            {"expr": "histogram_quantile(0.95, rate(l2_proxy_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p95", "refId": "B"},
            {"expr": "histogram_quantile(0.99, rate(l2_proxy_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p99", "refId": "C"}
        ]
    ))
    
    # Panel 22: Request Size
    panels.append(create_timeseries_panel(
        title="Размер запроса",
        id=22,
        x=12, y=y, w=6, h=8,
        unit="bytes",
        targets=[
            {"expr": "histogram_quantile(0.50, rate(l2_proxy_request_size_bytes_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p50", "refId": "A"},
            {"expr": "histogram_quantile(0.95, rate(l2_proxy_request_size_bytes_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p95", "refId": "B"},
            {"expr": "histogram_quantile(0.99, rate(l2_proxy_request_size_bytes_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p99", "refId": "C"}
        ]
    ))
    
    # Panel 23: Response Size
    panels.append(create_timeseries_panel(
        title="Размер ответа",
        id=23,
        x=18, y=y, w=6, h=8,
        unit="bytes",
        targets=[
            {"expr": "histogram_quantile(0.50, rate(l2_proxy_response_size_bytes_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p50", "refId": "A"},
            {"expr": "histogram_quantile(0.95, rate(l2_proxy_response_size_bytes_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p95", "refId": "B"},
            {"expr": "histogram_quantile(0.99, rate(l2_proxy_response_size_bytes_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p99", "refId": "C"}
        ]
    ))
    y += 8
    
    # Row 5: NATS
    panels.append(create_row_panel("NATS", 40, y))
    y += 1
    
    # Panel 41: NATS Requests
    panels.append(create_timeseries_panel(
        title="NATS запросы",
        id=41,
        x=0, y=y, w=8, h=8,
        unit="reqps",
        targets=[
            {"expr": "rate(l2_proxy_nats_requests_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Запросы/с", "refId": "A"}
        ]
    ))
    
    # Panel 42: NATS Errors
    panels.append(create_timeseries_panel(
        title="NATS ошибки",
        id=42,
        x=8, y=y, w=8, h=8,
        unit="reqps",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 1},
                {"color": "red", "value": 10}
            ]
        },
        targets=[
            {"expr": "rate(l2_proxy_nats_errors_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Ошибки/с", "refId": "A"}
        ]
    ))
    
    # Panel 43: NATS Connection Events
    panels.append(create_timeseries_panel(
        title="NATS события подключения",
        id=43,
        x=16, y=y, w=8, h=8,
        unit="reqps",
        targets=[
            {"expr": "rate(l2_proxy_nats_connection_creates_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Создания/с", "refId": "A"},
            {"expr": "rate(l2_proxy_nats_connection_errors_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Ошибки подкл./с", "refId": "B"}
        ]
    ))
    y += 8
    
    # Panel 44: NATS Request Duration
    panels.append(create_timeseries_panel(
        title="NATS длительность запроса",
        id=44,
        x=0, y=y, w=24, h=8,
        unit="s",
        targets=[
            {"expr": "histogram_quantile(0.50, rate(l2_proxy_nats_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p50", "refId": "A"},
            {"expr": "histogram_quantile(0.95, rate(l2_proxy_nats_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p95", "refId": "B"},
            {"expr": "histogram_quantile(0.99, rate(l2_proxy_nats_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p99", "refId": "C"}
        ]
    ))
    y += 8
    
    # Row 6: Connection Pool
    panels.append(create_row_panel("Пул соединений", 50, y))
    y += 1
    
    # Panel 51: Pool Active/Available
    panels.append(create_timeseries_panel(
        title="Пул активных/доступных",
        id=51,
        x=0, y=y, w=12, h=8,
        unit="short",
        targets=[
            {"expr": "l2_http_pool_active_clients{vm=~\"${vm:regex}\"}", "legendFormat": "Активные", "refId": "A"},
            {"expr": "l2_http_pool_available_clients{vm=~\"${vm:regex}\"}", "legendFormat": "Доступные", "refId": "B"}
        ]
    ))
    
    # Panel 52: Pool Acquisitions/Releases
    panels.append(create_timeseries_panel(
        title="Пул получения/возврата",
        id=52,
        x=12, y=y, w=6, h=8,
        unit="reqps",
        targets=[
            {"expr": "rate(l2_http_pool_client_acquisitions_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Получения/с", "refId": "A"},
            {"expr": "rate(l2_http_pool_client_releases_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Возвраты/с", "refId": "B"}
        ]
    ))
    
    # Panel 53: Pool Stale Evictions
    panels.append(create_timeseries_panel(
        title="Вытеснение устаревших из пула",
        id=53,
        x=18, y=y, w=6, h=8,
        unit="reqps",
        targets=[
            {"expr": "rate(l2_http_pool_stale_evictions_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Вытеснения/с", "refId": "A"}
        ]
    ))
    y += 8
    
    # Row 7: Rate Limiting
    panels.append(create_row_panel("Ограничение частоты", 60, y))
    y += 1
    
    # Panel 61: Global Rate Limiter Tokens
    panels.append(create_timeseries_panel(
        title="Токены глобального лимитера",
        id=61,
        x=0, y=y, w=6, h=8,
        unit="short",
        targets=[
            {"expr": "l2_rate_limiter_tokens{vm=~\"${vm:regex}\"}", "legendFormat": "Токены", "refId": "A"}
        ]
    ))
    
    # Panel 62: Global Rejected
    panels.append(create_timeseries_panel(
        title="Отклонено глобальным",
        id=62,
        x=6, y=y, w=6, h=8,
        unit="reqps",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 1},
                {"color": "red", "value": 10}
            ]
        },
        targets=[
            {"expr": "rate(l2_rate_limiter_rejected_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Отклонено/с", "refId": "A"}
        ]
    ))
    
    # Panel 63: Per-IP Rejected
    panels.append(create_timeseries_panel(
        title="Отклонено per-IP",
        id=63,
        x=12, y=y, w=6, h=8,
        unit="reqps",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 1},
                {"color": "red", "value": 10}
            ]
        },
        targets=[
            {"expr": "rate(l2_per_ip_rate_limiter_rejected_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Отклонено/с", "refId": "A"}
        ]
    ))
    
    # Panel 64: Per-IP Tracked IPs
    panels.append(create_timeseries_panel(
        title="Per-IP отслеживаемые IP",
        id=64,
        x=18, y=y, w=6, h=8,
        unit="short",
        targets=[
            {"expr": "l2_proxy_per_ip_rate_limiter_ips_tracked{vm=~\"${vm:regex}\"}", "legendFormat": "Отслеживаемые IP", "refId": "A"}
        ]
    ))
    y += 8

    # Panel 65: Top IPs by request rate (кто больше всего нагружает)
    panels.append(create_timeseries_panel(
        title="Топ IP по запросам",
        id=65,
        x=0, y=y, w=12, h=8,
        unit="reqps",
        targets=[
            {"expr": "topk(10, rate(l2_proxy_per_ip_requests_total{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "{{ip}}", "refId": "A"}
        ]
    ))

    # Panel 66: Top IPs by rejections
    panels.append(create_timeseries_panel(
        title="Топ IP по отказам",
        id=66,
        x=12, y=y, w=12, h=8,
        unit="reqps",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 1},
                {"color": "red", "value": 10}
            ]
        },
        targets=[
            {"expr": "topk(10, rate(l2_proxy_per_ip_rejected_total{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "{{ip}}", "refId": "A"}
        ]
    ))
    y += 8

    return dashboard


# ============================================================================
# Dashboard: L2 Worker (processing pipeline)
# ============================================================================

def create_worker_dashboard() -> Dict:
    """Create L2 Worker dashboard covering all worker-mode metrics"""
    dashboard = create_dashboard_base(
        title="L2 Воркер",
        uid="l2-worker",
        tags=["l2-proxy", "worker", "circuit-breaker"]
    )
    
    panels = dashboard["dashboard"]["panels"]
    y = 0
    
    # Row 1: Traffic
    panels.append(create_row_panel("Трафик", 1, y))
    y += 1
    
    # Panel 2: Requests Processed
    panels.append(create_timeseries_panel(
        title="Обработано запросов",
        id=2,
        x=0, y=y, w=8, h=8,
        unit="reqps",
        targets=[
            {"expr": "rate(l2_worker_requests_processed_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Запросы/с", "refId": "A"}
        ]
    ))
    
    # Panel 3: L2 Calls
    panels.append(create_timeseries_panel(
        title="L2 вызовы",
        id=3,
        x=8, y=y, w=8, h=8,
        unit="reqps",
        targets=[
            {"expr": "rate(l2_worker_l2_calls_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Вызовы/с", "refId": "A"}
        ]
    ))
    
    # Panel 4: L2 Errors
    panels.append(create_timeseries_panel(
        title="L2 ошибки",
        id=4,
        x=16, y=y, w=8, h=8,
        unit="reqps",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 1},
                {"color": "red", "value": 10}
            ]
        },
        targets=[
            {"expr": "rate(l2_worker_l2_errors_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Ошибки/с", "refId": "A"}
        ]
    ))
    y += 8
    
    # Row 2: Bytes
    panels.append(create_row_panel("Байты", 10, y))
    y += 1
    
    # Panel 11: Bytes Received
    panels.append(create_timeseries_panel(
        title="Получено байт",
        id=11,
        x=0, y=y, w=12, h=8,
        unit="Bps",
        targets=[
            {"expr": "rate(l2_worker_bytes_received_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Получено Б/с", "refId": "A"}
        ]
    ))
    
    # Panel 12: Bytes Sent
    panels.append(create_timeseries_panel(
        title="Отправлено байт",
        id=12,
        x=12, y=y, w=12, h=8,
        unit="Bps",
        targets=[
            {"expr": "rate(l2_worker_bytes_sent_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Отправлено Б/с", "refId": "A"}
        ]
    ))
    y += 8
    
    # Row 3: Latency
    panels.append(create_row_panel("Задержки", 20, y))
    y += 1
    
    # Panel 21: Request Duration
    panels.append(create_timeseries_panel(
        title="Длительность запроса",
        id=21,
        x=0, y=y, w=12, h=8,
        unit="s",
        targets=[
            {"expr": "histogram_quantile(0.50, rate(l2_worker_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p50", "refId": "A"},
            {"expr": "histogram_quantile(0.95, rate(l2_worker_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p95", "refId": "B"},
            {"expr": "histogram_quantile(0.99, rate(l2_worker_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p99", "refId": "C"}
        ]
    ))
    
    # Panel 22: L2 Call Duration
    panels.append(create_timeseries_panel(
        title="L2 длительность вызова",
        id=22,
        x=12, y=y, w=12, h=8,
        unit="s",
        targets=[
            {"expr": "histogram_quantile(0.50, rate(l2_worker_l2_call_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p50", "refId": "A"},
            {"expr": "histogram_quantile(0.95, rate(l2_worker_l2_call_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p95", "refId": "B"},
            {"expr": "histogram_quantile(0.99, rate(l2_worker_l2_call_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p99", "refId": "C"}
        ]
    ))
    y += 8
    
    # Row 4: Response Size
    panels.append(create_row_panel("Размер ответа", 30, y))
    y += 1
    
    # Panel 31: L2 Response Size
    panels.append(create_timeseries_panel(
        title="L2 размер ответа",
        id=31,
        x=0, y=y, w=24, h=8,
        unit="bytes",
        targets=[
            {"expr": "histogram_quantile(0.50, rate(l2_worker_l2_response_size_bytes_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p50", "refId": "A"},
            {"expr": "histogram_quantile(0.95, rate(l2_worker_l2_response_size_bytes_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p95", "refId": "B"},
            {"expr": "histogram_quantile(0.99, rate(l2_worker_l2_response_size_bytes_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p99", "refId": "C"}
        ]
    ))
    y += 8
    
    # Row 5: Errors & Resilience
    panels.append(create_row_panel("Ошибки и надёжность", 40, y))
    y += 1
    
    # Panel 41: Processing JSON Errors
    panels.append(create_timeseries_panel(
        title="Ошибки обработки JSON",
        id=41,
        x=0, y=y, w=8, h=8,
        unit="reqps",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 1},
                {"color": "red", "value": 10}
            ]
        },
        targets=[
            {"expr": "rate(l2_worker_processing_json_errors_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Ошибки JSON/с", "refId": "A"}
        ]
    ))
    
    # Panel 42: Processing Validation Errors
    panels.append(create_timeseries_panel(
        title="Ошибки валидации",
        id=42,
        x=8, y=y, w=8, h=8,
        unit="reqps",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 1},
                {"color": "red", "value": 10}
            ]
        },
        targets=[
            {"expr": "rate(l2_worker_processing_validation_errors_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Ошибки валидации/с", "refId": "A"}
        ]
    ))
    
    # Panel 43: Circuit Breaker State
    panels.append(create_timeseries_panel(
        title="Состояние Circuit Breaker",
        id=43,
        x=16, y=y, w=8, h=8,
        unit="short",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 1},
                {"color": "red", "value": 2}
            ]
        },
        targets=[
            {"expr": "l2_worker_circuit_breaker_state{vm=~\"${vm:regex}\"}", "legendFormat": "0=закрыт 1=открыт 2=полуоткрыт", "refId": "A"}
        ]
    ))
    y += 8
    
    # Row 6: Deduplication
    panels.append(create_row_panel("Дедупликация", 50, y))
    y += 1
    
    # Panel 44: Duplicate Requests Served from Cache
    panels.append(create_timeseries_panel(
        title="Дубликаты (из кэша)",
        id=44,
        x=0, y=y, w=24, h=8,
        unit="reqps",
        targets=[
            {"expr": "rate(l2_worker_duplicate_requests_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Дубликаты/с", "refId": "A"}
        ]
    ))
    
    return dashboard


# ============================================================================
# Dashboard: L2 Server (HTTP backend)
# ============================================================================

def create_server_dashboard() -> Dict:
    """Create L2 Server dashboard covering all server-mode metrics"""
    dashboard = create_dashboard_base(
        title="L2 Сервер",
        uid="l2-server",
        tags=["l2-proxy", "server", "http"]
    )
    
    panels = dashboard["dashboard"]["panels"]
    y = 0
    
    # Row 1: Traffic
    panels.append(create_row_panel("Трафик", 1, y))
    y += 1
    
    # Panel 2: Requests
    panels.append(create_timeseries_panel(
        title="Запросы",
        id=2,
        x=0, y=y, w=12, h=8,
        unit="reqps",
        targets=[
            {"expr": "rate(l2_server_requests_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Запросы/с", "refId": "A"}
        ]
    ))
    
    # Panel 3: Request Errors
    panels.append(create_timeseries_panel(
        title="Ошибки запросов",
        id=3,
        x=12, y=y, w=12, h=8,
        unit="reqps",
        thresholds={
            "mode": "absolute",
            "steps": [
                {"color": "green", "value": None},
                {"color": "yellow", "value": 1},
                {"color": "red", "value": 10}
            ]
        },
        targets=[
            {"expr": "rate(l2_server_request_errors_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Ошибки/с", "refId": "A"}
        ]
    ))
    y += 8
    
    # Row 2: Bytes
    panels.append(create_row_panel("Байты", 10, y))
    y += 1
    
    # Panel 11: Bytes Received
    panels.append(create_timeseries_panel(
        title="Получено байт",
        id=11,
        x=0, y=y, w=12, h=8,
        unit="Bps",
        targets=[
            {"expr": "rate(l2_server_bytes_received_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Получено Б/с", "refId": "A"}
        ]
    ))
    
    # Panel 12: Bytes Sent
    panels.append(create_timeseries_panel(
        title="Отправлено байт",
        id=12,
        x=12, y=y, w=12, h=8,
        unit="Bps",
        targets=[
            {"expr": "rate(l2_server_bytes_sent_total{vm=~\"${vm:regex}\"}[1m])", "legendFormat": "Отправлено Б/с", "refId": "A"}
        ]
    ))
    y += 8
    
    # Row 3: Latency
    panels.append(create_row_panel("Задержки", 20, y))
    y += 1
    
    # Panel 21: Request Duration
    panels.append(create_timeseries_panel(
        title="Длительность запроса",
        id=21,
        x=0, y=y, w=24, h=8,
        unit="s",
        targets=[
            {"expr": "histogram_quantile(0.50, rate(l2_server_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p50", "refId": "A"},
            {"expr": "histogram_quantile(0.95, rate(l2_server_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p95", "refId": "B"},
            {"expr": "histogram_quantile(0.99, rate(l2_server_request_duration_seconds_bucket{vm=~\"${vm:regex}\"}[5m]))", "legendFormat": "p99", "refId": "C"}
        ]
    ))
    
    return dashboard


# ============================================================================
# Main
# ============================================================================

def main():
    """Main function"""
    # Parse command line arguments
    parser = argparse.ArgumentParser(
        description='Grafana Dashboard Generator for HTTP Proxy',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Interactive mode
  python3 scripts/generate-grafana-dashboards.py
  
  # With config file
  python3 scripts/generate-grafana-dashboards.py --config grafana-config.json
  
  # With environment variables
  GRAFANA_API_KEY=xxx python3 scripts/generate-grafana-dashboards.py
        """
    )
    parser.add_argument(
        '--config',
        type=str,
        help='Path to JSON/YAML config file with Grafana settings'
    )
    parser.add_argument(
        '--prometheus-url',
        type=str,
        help='Prometheus URL for metric discovery (default: http://localhost:9090)'
    )
    parser.add_argument(
        '--discover-metrics',
        action='store_true',
        help='Enable metric discovery from Prometheus to create/update dashboards'
    )
    parser.add_argument(
        '--correct-dashboards',
        action='store_true',
        help='Correct existing dashboards if they differ from generated versions'
    )
    
    args = parser.parse_args()
    
    logger.info("=" * 60)
    logger.info("Grafana Dashboard Generator")
    logger.info("=" * 60)
    
    # Load configuration
    config = load_configuration(args.config)
    logger.info(f"Grafana URL: {config['url']}")
    
    # Initialize Grafana API client
    if config['api_key']:
        api = GrafanaAPI(config['url'], api_key=config['api_key'])
    else:
        api = GrafanaAPI(config['url'], user=config['user'], password=config['password'])

    # Test Grafana connection
    if not api.test_connection():
        logger.error("Cannot connect to Grafana. Exiting.")
        return 1
    
    # Define standard dashboards to create/update
    dashboard_definitions = [
        (create_tracing_dashboard, 'l2-distributed-tracing'),
        (create_proxy_dashboard, 'l2-proxy'),
        (create_worker_dashboard, 'l2-worker'),
        (create_server_dashboard, 'l2-server'),
        (create_slo_dashboard, 'l2-slo-tracking'),
        (create_nats_dashboard, 'nats-dashboard'),
        (create_nginx_dashboard, 'nginx-metrics')
    ]
    
    # Metric discovery and dashboard creation (if enabled)
    if args.discover_metrics:
        prometheus_url = args.prometheus_url or 'http://localhost:9090'
        prometheus_api = PrometheusAPI(prometheus_url)
        
        if prometheus_api.test_connection():
            logger.info("Starting metric discovery...")
            discover_and_create_dashboards(api, prometheus_api)
        else:
            logger.warning("Cannot connect to Prometheus, skipping metric discovery")
    
    # Correct existing dashboards (if enabled)
    if args.correct_dashboards:
        logger.info("Checking for dashboard corrections...")
        success_count = 0
        for dashboard_func, uid in dashboard_definitions:
            if correct_dashboard_panels(api, dashboard_func, uid):
                success_count += 1
        
        logger.info("=" * 60)
        logger.info(f"Dashboard correction complete: {success_count}/{len(dashboard_definitions)} successful")
        logger.info("=" * 60)
        return 0 if success_count == len(dashboard_definitions) else 1

    # Create/update standard dashboards (without correction mode)
    success_count = 0
    for dashboard_func, uid in dashboard_definitions:
        dashboard = dashboard_func()
        title = dashboard["dashboard"]["title"]

        logger.info(f"Processing dashboard: {title} ({uid})")

        # Save dashboard
        if api.save_dashboard(dashboard):
            success_count += 1

    logger.info("=" * 60)
    logger.info(f"Dashboard generation complete: {success_count}/{len(dashboard_definitions)} successful")
    logger.info("=" * 60)

    return 0 if success_count == len(dashboard_definitions) else 1


if __name__ == "__main__":
    exit(main())
