#!/bin/bash
echo "=== Looking for keyboard input files in moonlight-qt ==="
find D:/Code/moonlight-qt -type f \( -name "*.cpp" -o -name "*.h" \) | grep -iE "input|keyboard|Sdl" | head -30

echo ""
echo "=== Looking for SS_KBE_FLAG in moonlight-common-c ==="
find D:/Code/moonlight-qt/moonlight-common-c -type f \( -name "*.h" -o -name "*.c" \) | head -50

echo ""
echo "=== Grep SS_KBE_FLAG ==="
grep -rn "SS_KBE_FLAG" D:/Code/moonlight-qt/ --include="*.h" --include="*.cpp" --include="*.c" 2>/dev/null | head -30

echo ""
echo "=== Grep NON_NORMALIZED ==="
grep -rn "NON_NORMALIZED" D:/Code/moonlight-qt/ --include="*.h" --include="*.cpp" --include="*.c" 2>/dev/null | head -30

echo ""
echo "=== Moonlight-web backend keyboard files ==="
find D:/Code/moonlight-web-deepseek/backend -type f \( -name "*.cpp" -o -name "*.h" \) | grep -iE "input|keyboard|relay|signaling" | head -30

echo ""
echo "=== Moonlight-web frontend keyboard files ==="
find D:/Code/moonlight-web-deepseek/frontend -type f -name "*.js" | grep -iE "input|keyboard|stream|view" | head -30
