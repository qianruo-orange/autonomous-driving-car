#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-late
# Commit every working-tree change and push it to GitHub.
#
# Usage: scripts/git_sync.sh "ui: short summary"
#
# Called after each deploy so the board and the repository never drift. The
# deploy itself runs as root (scripts/deploy.sh), but git must run as qianruo
# to reuse ~/.ssh/id_ed25519, hence this separate, user-level script.
set -e
cd "$(dirname "$0")/.."

msg="${1:-chore: sync board changes}"

if [ -z "$(git status --porcelain)" ]; then
    echo "[git] working tree clean, nothing to commit"
else
    git add -A
    git commit -q -m "$msg"
    echo "[git] committed: $msg"
fi

branch=$(git rev-parse --abbrev-ref HEAD)
export GIT_SSH_COMMAND='ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new'
timeout 180 git push origin "$branch" 2>&1 | tail -3
echo "[git] $branch -> origin/$branch at $(git rev-parse --short HEAD)"
