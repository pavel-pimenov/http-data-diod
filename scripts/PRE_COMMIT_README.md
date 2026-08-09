# Git Pre-commit Test Script

## Overview

This pre-commit hook automatically runs health checks and message consistency tests before allowing commits. This ensures that only working code is committed to the repository.

## Installation

The hook is automatically installed when you clone the repository. To manually install:

```bash
# Make sure scripts are executable
chmod +x scripts/pre-commit.sh
chmod +x .git/hooks/pre-commit
```

## Usage

### Automatic (via git hook)

When you run `git commit`, the pre-commit hook will automatically:
1. Check if containers are running
2. Run health checks on all services
3. Run message consistency test (1 iteration, 1 concurrent)
4. Allow or block the commit based on test results

```bash
git commit -m "your message"
```

### Manual (direct script)

You can also run the pre-commit script manually:

```bash
# Run tests and commit if passed
./scripts/pre-commit.sh "your commit message"

# Just run tests without committing
./scripts/pre-commit.sh
```

### Git Alias (optional)

Add a convenient git alias:

```bash
git config alias.safe-commit '!./scripts/pre-commit.sh'

# Usage:
git safe-commit "your commit message"
```

## What Gets Tested

1. **Container Status** - Checks if Docker containers are running
2. **Health Checks** - Verifies all services respond:
   - l2-proxy (port 8888, /health)
   - l2-worker (port 19091, /metrics)
   - l2-server (port 8088, /health)
   - nginx (port 7777)
   - jaeger (port 16686)

3. **Message Consistency** - Sends a test message through the proxy and verifies it's received correctly

## Skipping Tests (Not Recommended)

In emergency situations, you can skip the pre-commit hook:

```bash
git commit --no-verify -m "emergency fix"
```

⚠️ **Warning**: Skipping tests may result in broken code being committed!

## Troubleshooting

### Tests Fail Randomly

1. Check if services are healthy:
   ```bash
   ./health-check.sh all
   ```

2. Rebuild and restart services:
   ```bash
   ./rebuild-and-run.sh
   ```

3. Check service logs:
   ```bash
   docker compose logs -f
   ```

### Containers Not Running

The pre-commit script will try to start containers automatically. If this fails:

```bash
# Manual start
./rebuild-and-run.sh

# Then try commit again
git commit -m "your message"
```

### Hook Not Running

Make sure the hook is executable:

```bash
chmod +x .git/hooks/pre-commit
chmod +x scripts/pre-commit.sh
```

## Configuration

Edit `scripts/pre-commit.sh` to customize:

- `RETRY_DELAY` - Time between health check retries
- `MAX_RETRIES` - Number of health check attempts
- Test parameters for message_counter.py

## Files

- `scripts/pre-commit.sh` - Main pre-commit script
- `.git/hooks/pre-commit` - Git hook that calls the script
- `health-check.sh` - Health check utility (called by pre-commit)
- `message_counter.py` - Message consistency test (called by pre-commit)
