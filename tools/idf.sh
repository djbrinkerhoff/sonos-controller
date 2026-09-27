#!/bin/bash
set -euo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
export IDF_TOOLS_PATH="$project_root/.tools/espressif"
if [[ -z "${IDF_PYTHON_ENV_PATH:-}" ]]; then
  for candidate in "$IDF_TOOLS_PATH"/python_env/idf5.5_py*_env; do
    if [[ -x "$candidate/bin/python" ]]; then export IDF_PYTHON_ENV_PATH="$candidate"; fi
  done
fi
if [[ -z "${IDF_PYTHON_ENV_PATH:-}" ]]; then
  echo "Run bash tools/bootstrap.sh first." >&2
  exit 1
fi
export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
source "$project_root/.tools/esp-idf/export.sh" >/dev/null
cd "$project_root/firmware"
exec idf.py "$@"
