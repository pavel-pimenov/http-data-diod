#!/bin/sh
# Runs the payload as the unprivileged 'app' user (uid 10001). Every entry in
# the runtime images goes through this script so the main process never runs
# as root, while host-mounted writable dirs (/root/logs, /memory-logs,
# /profiles, /crash-dumps) can still be re-owned on each start (their uid is
# the host's, visible in the container without CAP_CHOWN only after a chown
# done by root at boot).
set -eu

if [ "$(id -u)" = "0" ]; then
  chown -R app:app /root /memory-logs /profiles /crash-dumps 2>/dev/null || true
  exec setpriv --reuid 10001 --regid 10001 --clear-groups "$@"
fi

exec "$@"