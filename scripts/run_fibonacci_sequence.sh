#!/bin/bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
exec bash "$root/configurations-paper/run.sh" "$@"
