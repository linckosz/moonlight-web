#!/bin/bash
# Show the actual file content of StreamView.js from git
git -C "d:/Code/moonlight-web-deepseek" show HEAD:frontend/js/stream/StreamView.js 2>/dev/null || echo "NOT IN GIT"