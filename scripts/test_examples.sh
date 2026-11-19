#!/bin/bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
exec bash "$root/scripts/run_all_configurations.sh" "$@" "$root/examples"
