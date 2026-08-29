#!/bin/bash
# cleanup.sh — зачистка артефактов http-data-diod (build/logs/profiles/crash)
# Использование:
#   ./cleanup.sh              # dry-run (показать что удалится)
#   ./cleanup.sh --apply      # реально чистит
#   ./cleanup.sh --all        # + docker prune (build cache)
#   ./cleanup.sh --help
set -euo pipefail

DRY_RUN=true
DO_DOCKER=false
if [[ "${1:-}" == "--apply" || "${1:-}" == "--all" ]]; then
  DRY_RUN=false
fi
if [[ "${1:-}" == "--all" ]]; then
  DO_DOCKER=true
fi
if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  echo "Usage: $0 [--dry-run] [--apply] [--all]"
  echo "  --dry-run  показать что удалится (default)"
  echo "  --apply    реально чистит build/logs/profiles/crash"
  echo "  --all      + docker builder prune (until=24h)"
  exit 0
fi
if [[ "${1:-}" == "" ]]; then
  echo "dry-run mode — покажем что удалится, для применения: $0 --apply"
fi

run() {
  if $DRY_RUN; then
    echo "[dry-run] $*"
  else
    echo "[run] $*"
    eval "$@"
  fi
}

echo "=== http-data-diod cleanup ==="
echo "dry-run=$DRY_RUN all=$DO_DOCKER"
echo ""

# 1. CMake build артефакты (пересоберутся в контейнере, ccache сохранится)
echo "--- build dirs ---"
for d in cpp/l2-proxy/build cpp/l2-proxy/build-asan cpp/l2-proxy/build-lint cpp/l2-proxy/build-pvs cpp/l2-proxy/build-asan.log cpp/l2-proxy/build/build cpp/l2-proxy/build_test cpp/l2-proxy/build_tests; do
  if [ -e "$d" ]; then
    du -sh "$d" 2>/dev/null | awk '{print "  "$1" "$2}'
    if [ -w "$d" ] || [ -w "$(dirname "$d")" ]; then
      run "rm -rf \"$d\""
    else
      echo "  (need sudo for $d)"
      if $DRY_RUN; then
        echo "[dry-run] sudo rm -rf \"$d\""
      else
        sudo -n rm -rf "$d" 2>/dev/null || echo "  skip $d (sudo required, run: sudo rm -rf $d)"
      fi
    fi
  fi
done
# __pycache__ и pyc (игнорится, но на диске)
for d in $(find . -type d -name "__pycache__" 2>/dev/null | head -n 20); do
  echo "  __pycache__ $d"
  run "rm -rf \"$d\""
done
for f in $(find . -name "*.pyc" -o -name "*.pyo" 2>/dev/null | head -n 20); do
  echo "  pyc $f"
  run "rm -f \"$f\""
done

# 2. Логи — gzip вместо удаления (сохраняем для анализа)
echo ""
echo "--- logs ---"
if [ -d "logs/load-6h" ]; then
  for f in logs/load-6h/*.log; do
    [ -e "$f" ] || continue
    if [[ "$f" == *.gz ]]; then continue; fi
    sz=$(du -h "$f" 2>/dev/null | cut -f1)
    echo "  $sz $f -> $f.gz"
    if $DRY_RUN; then
      echo "[dry-run] gzip -k \"$f\""
    else
      gzip -k "$f" 2>/dev/null || true
      # оставляем оригинал для tail, но можно раскомментировать для удаления:
      # rm -f "$f"
    fi
  done
fi
for f in logs/*.log logs/l2-proxy.log; do
  [ -e "$f" ] || continue
  if [[ "$f" == *.gz ]]; then continue; fi
  sz=$(du -h "$f" 2>/dev/null | cut -f1)
  echo "  $sz $f (keep, gzip -k)"
  run "gzip -kf \"$f\" 2>/dev/null || true"
done

# 3. profiles — архивируем и чистим старые профайлы
echo ""
echo "--- profiles ---"
if [ -d "profiles" ] && [ "$(ls -A profiles 2>/dev/null)" ]; then
  du -sh profiles 2>/dev/null || true
  if $DRY_RUN; then
    echo "[dry-run] tar czf profiles-20260828.tgz profiles/*.prof profiles/*.hprof 2>/dev/null; rm -f profiles/*.prof profiles/*.hprof"
  else
    if ls profiles/*.prof profiles/*.hprof 1>/dev/null 2>&1; then
      tar czf "profiles-$(date +%Y%m%d).tgz" profiles/*.prof profiles/*.hprof 2>/dev/null || true
      echo "  archived -> profiles-$(date +%Y%m%d).tgz"
      rm -f profiles/*.prof profiles/*.hprof 2>/dev/null || true
    fi
  fi
else
  echo "  (empty)"
fi

# 4. crash-dumps — удаляем старые >30 дней, свежие оставляем
echo ""
echo "--- crash-dumps ---"
if [ -d "crash-dumps" ]; then
  ls -lh crash-dumps 2>&1 | head -n 20
  if $DRY_RUN; then
    echo "[dry-run] find crash-dumps -type f -mtime +30 -delete"
  else
    find crash-dumps -type f -mtime +30 -delete 2>/dev/null || true
    # также удаляем 2026-08-11 старые SIGABRT (уже пофикшены Jaeger UAF)
    rm -f crash-dumps/crash_20260811_* 2>/dev/null || true
    echo "  cleaned old dumps"
  fi
fi

# 5. Пустые служебные папки (не монтируются)
echo ""
echo "--- empty dirs ---"
for d in certs_nats docker-memory-analysis; do
  if [ -d "$d" ] && [ -z "$(ls -A "$d" 2>/dev/null)" ]; then
    echo "  empty $d"
    run "rmdir \"$d\" 2>/dev/null || true"
  elif [ -d "$d" ]; then
    echo "  keep $d (not empty)"
  fi
done

# 6. Docker build cache (только --all)
echo ""
echo "--- docker ---"
if $DO_DOCKER; then
  run "docker builder prune --filter \"until=24h\" -f 2>/dev/null || true"
  run "docker image prune -f 2>/dev/null || true"
else
  echo "  skip (use --all to prune)"
  docker system df 2>/dev/null | head -n 10 || true
fi

echo ""
if $DRY_RUN; then
  echo "dry-run done — для применения: $0 --apply"
else
  echo "cleanup done"
fi
