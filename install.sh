#!/bin/sh
# Run as your normal desktop user, never with sudo.
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$repo_dir/scripts/install.py" "$@"
