#!/bin/bash
# Generates the Secure Boot V2 RSA-3072 OTA signing key, once. The key is
# gitignored and must be backed up elsewhere; without it no further OTA image
# can be signed and updates fall back to USB.
set -euo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
key="$project_root/firmware/keys/ota_signing_key.pem"
if [[ -f "$key" ]]; then
  echo "Signing key already exists: $key (not overwriting)"
  exit 0
fi
export IDF_TOOLS_PATH="$project_root/.tools/espressif"
for candidate in "$IDF_TOOLS_PATH"/python_env/idf5.5_py*_env; do
  if [[ -x "$candidate/bin/python" ]]; then python_env="$candidate"; fi
done
if [[ -z "${python_env:-}" ]]; then
  echo "Run bash tools/bootstrap.sh first." >&2
  exit 1
fi
mkdir -p "$(dirname "$key")"
"$python_env/bin/python" -m espsecure generate_signing_key --version 2 --scheme rsa3072 "$key"
chmod 600 "$key"
echo "Created $key - back it up somewhere safe; losing it means OTA signing stops."
