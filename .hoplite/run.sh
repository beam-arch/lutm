#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec node sim/status/server.mjs
