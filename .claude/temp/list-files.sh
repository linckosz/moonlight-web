#!/bin/bash
echo "=== Backend server/ ==="
ls -la d:/Code/moonlight-web-deepseek/backend/src/server/
echo "=== Frontend js/ ==="
ls -la d:/Code/moonlight-web-deepseek/frontend/js/
echo "=== Frontend js/streaming/ ==="
ls -la d:/Code/moonlight-web-deepseek/frontend/js/streaming/ 2>/dev/null || echo "(no streaming dir)"
echo "=== Frontend js/app/ ==="
ls -la d:/Code/moonlight-web-deepseek/frontend/js/app/ 2>/dev/null || echo "(no app dir)"
echo "=== Backend src/ ==="
ls -la d:/Code/moonlight-web-deepseek/backend/src/