#!/bin/bash
echo "=== Recent commits ==="
git -C "d:/Code/moonlight-web-deepseek" log --oneline -10
echo "=== Files in latest commit ==="
git -C "d:/Code/moonlight-web-deepseek" show --name-only --oneline HEAD
echo "=== Diff of latest commit ==="
git -C "d:/Code/moonlight-web-deepseek" diff HEAD~1..HEAD -- "frontend/" 2>/dev/null | head -200
echo "=== Find StreamView ==="
find "d:/Code/moonlight-web-deepseek/frontend/" -name "*Stream*" -o -name "*stream*" 2>/dev/null