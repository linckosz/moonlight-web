#!/bin/bash
echo "=== Backend src ==="
ls -la d:/Code/moonlight-web-deepseek/backend/src/ 2>/dev/null || echo "NOT FOUND"
echo ""
echo "=== Frontend ==="
find d:/Code/moonlight-web-deepseek/frontend/ -name "*.js" -type f 2>/dev/null | head -40
echo ""
echo "=== Backend all ==="
find d:/Code/moonlight-web-deepseek/backend/ -name "*.cpp" -o -name "*.hpp" -o -name "*.h" 2>/dev/null | head -40
