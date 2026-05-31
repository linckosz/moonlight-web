#!/bin/bash
cd d:/Code/moonlight-web-deepseek
echo "=== GIT LOG (all) ==="
git log --oneline --all -100
echo ""
echo "=== GIT LOG (verbose, last 20) ==="
git log --oneline -20
echo ""
echo "=== TOP-LEVEL STRUCTURE ==="
ls -la
echo ""
echo "=== BACKEND SRC ==="
find backend/src -name "*.h" -o -name "*.cpp" -o -name "*.pro" | sort
echo ""
echo "=== FRONTEND JS ==="
find frontend/js -name "*.js" | sort
echo ""
echo "=== FRONTEND FILES ==="
find frontend -type f | sort
echo ""
echo "=== DOCS ==="
ls docs/
echo ""
echo "=== RESULTS DIR ==="
find .claude/results -name "*.md" 2>/dev/null | sort | head -50
echo ""
echo "=== MEMORY FILES ==="
find .claude/agent-memory -name "*.md" 2>/dev/null | sort | head -50
