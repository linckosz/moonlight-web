#!/bin/bash
# Search for UPnP/IGD/port mapping references in moonlight-qt
find "D:/Code/moonlight-qt/app" -type f \( -name "*.cpp" -o -name "*.h" -o -name "*.hpp" -o -name "*.cmake" -o -name "*.txt" -o -name "*.md" \) | head -50
echo "=== GREP for UPnP ==="
grep -rni "upnp\|igD\|natpmp\|portmap\|port.map\|miniupnp\|portmapper" "D:/Code/moonlight-qt/app" --include="*.cpp" --include="*.h" --include="*.hpp" --include="*.md" --include="*.txt" 2>/dev/null
echo "=== GREP done ==="
echo "=== ExternalDependencies ==="
find "D:/Code/moonlight-qt" -name "*.txt" -o -name "*.cmake" -o -name "*.md" | xargs grep -li "upnp\|portmap\|miniupnp" 2>/dev/null
echo "=== EXTERNAL_CHECK done ==="
