#!/bin/bash
# Find all references to AppSettings::domain() in the backend
grep -rn "domain()" d:/Code/moonlight-web-deepseek/backend/src/ --include="*.cpp" --include="*.h" | grep -v ".claude" | grep -v "third_party"
echo "---"
# Also find direct references to "domain" key in JSON
grep -rn '"domain"' d:/Code/moonlight-web-deepseek/backend/src/ --include="*.cpp" --include="*.h" | grep -v ".claude" | grep -v "third_party"
