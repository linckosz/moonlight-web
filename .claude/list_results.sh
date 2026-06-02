#!/bin/bash
echo "=== engineering-manager results ==="
ls -la .claude/results/engineering-manager/ 2>/dev/null || echo "empty"
echo ""
echo "=== all results dirs ==="
find .claude/results -maxdepth 3 -type d 2>/dev/null
echo ""
echo "=== recent .md files in .claude ==="
find .claude -name "*.md" -mtime -7 2>/dev/null | head -20
echo ""
echo "=== recent .log files ==="
find . -name "*.log" -mtime -7 2>/dev/null | head -20
echo ""
echo "=== recent files with 'log' in name ==="
find . -name "*log*" -mtime -7 2>/dev/null | head -20
