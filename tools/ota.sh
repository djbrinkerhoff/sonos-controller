#!/bin/bash
# Builds the firmware and uploads it to a Tab5's /ota endpoint, e.g.
#   bash tools/ota.sh 192.168.68.60
set -euo pipefail
if [[ $# -ne 1 ]]; then
  echo "usage: $0 <tab5-ip>" >&2
  exit 1
fi
project_root="$(cd "$(dirname "$0")/.." && pwd)"
bash "$project_root/tools/idf.sh" build
image="$project_root/firmware/build/sonos_controller.bin"
echo "Uploading $(stat -f%z "$image") bytes to $1 ..."
curl -sS --fail -X POST --data-binary "@$image" "http://$1:3400/ota"
echo
