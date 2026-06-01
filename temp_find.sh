#!/bin/bash
PROJ="/d/Code/moonlight-web-deepseek"
echo "=== backend/src/input/ ==="
ls "$PROJ/backend/src/input/" 2>/dev/null || echo "NOT FOUND"
echo ""
echo "=== backend/src/ streaming/ ==="
ls "$PROJ/backend/src/streaming/" 2>/dev/null || echo "NOT FOUND"
echo ""
echo "=== frontend js files ==="
find "$PROJ/frontend" -name "*.js" 2>/dev/null | head -40
echo ""
echo "=== find files with mouse/mouse in name ==="
find "$PROJ/backend" -iname "*mouse*" -o -iname "*input*" 2>/dev/null
echo ""
echo "=== find files with StreamView ==="
find "$PROJ/frontend" -iname "*stream*" -o -iname "*view*" 2>/dev/null
