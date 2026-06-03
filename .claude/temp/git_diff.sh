#!/bin/sh
cd /d/Code/moonlight-web-deepseek
echo "=== Recent commits ==="
git log --oneline -10
echo ""
echo "=== Changed files in Fix UDP ==="
git diff --stat 5f0f588..a52f2cf
echo ""
echo "=== Full diff (frontend) ==="
git diff --no-color 5f0f588..a52f2cf -- frontend/
echo ""
echo "=== Full diff (backend) ==="
git diff --no-color 5f0f588..a52f2cf -- backend/
