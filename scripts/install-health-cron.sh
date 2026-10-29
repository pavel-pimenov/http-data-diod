#!/bin/bash

# Install a per-user cron entry that runs ./health-check.sh all on a schedule.
#
# Why cron and not systemd: the operator account has no sudo on this host, so a
# system unit or /etc/cron.d drop-in is not an option. A user crontab entry is
# enough and keeps the watchdog outside the stack it watches: if Docker, the
# containers or vmalert are broken, cron still fires.
#
# Usage:
#   ./scripts/install-health-cron.sh              # install / update (idempotent)
#   ./scripts/install-health-cron.sh --print      # show the crontab line
#   ./scripts/install-health-cron.sh --remove     # remove the entry
#   ./scripts/install-health-cron.sh --status     # installed? + last log lines
#
# Environment:
#   HEALTH_CRON_INTERVAL  minutes between runs (default 15)
#   HEALTH_CRON_LOG       log file (default /tmp/http-data-diod-health.log)
#   HEALTH_CRON_RETRIES   retries passed to health-check.sh (default 2)
#   HEALTH_CRON_MAX_LOG   truncate the log above this size in bytes (default 1 MiB)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

INTERVAL="${HEALTH_CRON_INTERVAL:-15}"
LOG_FILE="${HEALTH_CRON_LOG:-/tmp/http-data-diod-health.log}"
RETRIES="${HEALTH_CRON_RETRIES:-2}"
MAX_LOG="${HEALTH_CRON_MAX_LOG:-1048576}"
LOCK_FILE="${HEALTH_CRON_LOCK:-/tmp/http-data-diod-health.lock}"
MARKER="# http-data-diod-health-check (managed by scripts/install-health-cron.sh)"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

if ! command -v crontab >/dev/null 2>&1; then
    log_error "crontab not found — install cron on this host first"
    exit 1
fi
if ! command -v flock >/dev/null 2>&1; then
    log_error "flock not found (util-linux) — it serialises overlapping runs"
    exit 1
fi
case "$INTERVAL" in
    ''|*[!0-9]*)
        log_error "HEALTH_CRON_INTERVAL must be an integer number of minutes: '$INTERVAL'"
        exit 1
        ;;
esac
if [ "$INTERVAL" -lt 1 ] || [ "$INTERVAL" -gt 1440 ]; then
    log_error "HEALTH_CRON_INTERVAL must be between 1 and 1440 minutes: '$INTERVAL'"
    exit 1
fi

cron_line() {
    # PATH: cron starts with a minimal environment and docker lives in /usr/bin.
    # flock -n: a slow run (docker restart + 10 s waits) must not pile up; a
    # skipped run is fine because the next tick comes in a few minutes.
    # The trailing `if` truncates the log in place once it exceeds MAX_LOG, so
    # the entry needs no external logrotate.
    printf '*/%s * * * * PATH=/usr/local/bin:/usr/bin:/bin cd %s && flock -n %s ./health-check.sh all %s >>%s 2>&1; if [ -f %s ] && [ "$(stat -c%%s %s 2>/dev/null || echo 0)" -gt %s ]; then : > %s; fi' \
        "$INTERVAL" "$PROJECT_ROOT" "$LOCK_FILE" "$RETRIES" "$LOG_FILE" \
        "$LOG_FILE" "$LOG_FILE" "$MAX_LOG" "$LOG_FILE"
    printf '\n'
}

strip_managed() {
    # Drop the marker and the managed cron line, keep everything else intact.
    local current
    current="$(crontab -l 2>/dev/null || true)"
    [ -z "$current" ] && return 0
    printf '%s\n' "$current" | grep -v -F "$MARKER" | grep -v -F "$LOCK_FILE" || true
}

do_print() {
    echo "$MARKER"
    cron_line
}

do_status() {
    if crontab -l 2>/dev/null | grep -qF "$MARKER"; then
        log_info "cron entry installed:"
        crontab -l 2>/dev/null | grep -A1 -F "$MARKER"
    else
        log_warn "cron entry is NOT installed"
    fi
    if [ -f "$LOG_FILE" ]; then
        log_info "last log lines ($LOG_FILE):"
        tail -n 20 "$LOG_FILE"
    else
        log_warn "log file $LOG_FILE does not exist yet"
    fi
}

do_install() {
    local entry
    entry="$(strip_managed)"
    {
        [ -n "$entry" ] && printf '%s\n' "$entry"
        do_print
    } | crontab -
    log_info "installed cron entry (every ${INTERVAL} min):"
    do_print
    log_info "log: $LOG_FILE, lock: $LOCK_FILE"
    log_warn "this is a user crontab entry: it runs only while the user session host is up and cron is running"
}

do_remove() {
    local entry
    entry="$(strip_managed)"
    if [ -n "$entry" ]; then
        printf '%s\n' "$entry" | crontab -
    else
        crontab -r 2>/dev/null || true
    fi
    log_info "removed cron entry (other crontab lines are untouched)"
}

case "${1:---install}" in
    --install|"")
        do_install
        ;;
    --print)
        do_print
        ;;
    --remove)
        do_remove
        ;;
    --status)
        do_status
        ;;
    -h|--help)
        sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        ;;
    *)
        log_error "unknown option: $1 (use --install, --print, --remove, --status)"
        exit 1
        ;;
esac
