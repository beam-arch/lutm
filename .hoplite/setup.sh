#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
npm ci --prefix sim/status --no-audit --no-fund
