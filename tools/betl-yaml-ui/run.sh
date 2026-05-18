#!/usr/bin/env bash
# tools/betl-yaml-ui/run.sh — launch the pipeline YAML viewer/editor.
#
# Usage:
#   tools/betl-yaml-ui/run.sh [server.py args...]
#
# First invocation creates .venv/ here and installs fastapi + uvicorn.
# Subsequent runs reuse it. Pass any extra args through to server.py
# (e.g. --root some/dir --port 9000).

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
venv="$here/.venv"

if [[ ! -x "$venv/bin/python" ]]; then
    echo "[run] creating venv at $venv" >&2
    python3 -m venv "$venv"
    "$venv/bin/pip" install --quiet --upgrade pip
    "$venv/bin/pip" install --quiet fastapi uvicorn
fi

exec "$venv/bin/python" "$here/server.py" "$@"
