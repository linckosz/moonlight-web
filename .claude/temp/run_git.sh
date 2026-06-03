#!/bin/bash
cd /d/Code/moonlight-web-deepseek && git log --oneline -10 2>&1
echo "---"
git diff 5f0f588..a52f2cf --name-only 2>&1
