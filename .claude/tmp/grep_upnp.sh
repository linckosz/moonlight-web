grep -rni "upnp\|igD\|natpmp\|portmap\|port.map\|miniupnp\|portmapper" "D:/Code/moonlight-qt/app" --include="*.cpp" --include="*.h" --include="*.hpp" 2>/dev/null
echo "EXIT:$?"
