#!/usr/bin/env bash
# bumpver's pre_commit_hook (pyproject.toml): the new version written, uv.lock follows it (nind's
# own entry: bumpver's patterns can't tell it from another package's) and goes in the same commit.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
uv lock 2>&1         # bumpver logs a hook's stderr as errors
git add uv.lock
