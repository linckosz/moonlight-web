#!/bin/bash
echo "=== StreamView.js ==="
cat -n "d:/Code/moonlight-web-deepseek/frontend/js/stream/StreamView.js" 2>&1
echo "=== Directory ==="
ls -la "d:/Code/moonlight-web-deepseek/frontend/js/stream/" 2>&1
echo "=== All frontend JS files ==="
find "d:/Code/moonlight-web-deepseek/frontend/" -name "*.js" -type f 2>&1 | head -30