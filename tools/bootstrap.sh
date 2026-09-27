#!/bin/bash
set -euo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$project_root"
mkdir -p .tools
export IDF_TOOLS_PATH="$project_root/.tools/espressif"
export PIP_CACHE_DIR="$project_root/.tools/pip-cache"
if [[ ! -d .tools/esp-idf ]]; then
  git clone --depth 1 --branch v5.5.3 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git .tools/esp-idf
fi
if [[ "$(git -C .tools/esp-idf rev-parse HEAD)" != "2c211b236707889e8400c4dc5644dd5c4ee071e0" ]]; then
  echo "Expected ESP-IDF v5.5.3; existing toolchain was left unchanged." >&2
  exit 1
fi
.tools/esp-idf/install.sh esp32p4
for candidate in "$IDF_TOOLS_PATH"/python_env/idf5.5_py*_env; do
  if [[ -x "$candidate/bin/python" ]]; then
    "$candidate/bin/python" -m pip install 'cmake<4' ninja
  fi
done
echo "Ready: bash tools/idf.sh build"
