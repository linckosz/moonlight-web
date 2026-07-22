#!/bin/bash
set -euo pipefail

# Only relevant in Claude Code on the web, where the global git config
# is reset to the Claude identity on every fresh session/reclone.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

# Override the commit author identity so commits made in this environment
# appear under Bruno's GitHub account (and show his avatar) instead of Claude's.
git config --local user.name "Bruno Martin"
git config --local user.email "brunoocto@gmail.com"
