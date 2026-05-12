ls -R d:/Code/moonlight-web-deepseek/backend/src/stream/ 2>/dev/null || echo "stream dir not found"
echo "---"
ls d:/Code/moonlight-web-deepseek/backend/src/ 2>/dev/null || echo "src dir not found"
echo "---"
find d:/Code/moonlight-web-deepseek/backend/src/ -name "*.cpp" -o -name "*.h" | head -50
