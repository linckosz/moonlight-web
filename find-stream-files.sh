#!/bin/bash
find d:/Code/moonlight-web-deepseek/frontend -name "StreamView*" -type f 2>/dev/null
find d:/Code/moonlight-web-deepseek/frontend -name "stream*" -type f 2>/dev/null
echo "---"
ls -la d:/Code/moonlight-web-deepseek/frontend/js/stream/ 2>/dev/null || echo "stream dir not found"
ls -la d:/Code/moonlight-web-deepseek/frontend/js/ 2>/dev/null || echo "js dir not found"