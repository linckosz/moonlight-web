# Find relevant source files
echo "=== Input files ==="
find /d/Code/moonlight-web-deepseek/backend/src/input/ -type f 2>/dev/null

echo "=== Streaming files ==="
find /d/Code/moonlight-web-deepseek/backend/src/streaming/ -type f 2>/dev/null

echo "=== Network files ==="
find /d/Code/moonlight-web-deepseek/backend/src/network/ -type f 2>/dev/null

echo "=== Frontend JS ==="
find /d/Code/moonlight-web-deepseek/frontend/ -name "*.js" -type f 2>/dev/null

echo "=== Frontend UI ==="
find /d/Code/moonlight-web-deepseek/frontend/ -name "*.html" -type f 2>/dev/null

echo "=== Backend src subdirs ==="
find /d/Code/moonlight-web-deepseek/backend/src/ -maxdepth 1 -type d 2>/dev/null