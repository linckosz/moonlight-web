#!/bin/bash
echo "=== backend-dev ==="
ls -la .claude/results/backend-dev/ 2>/dev/null || echo "no backend-dev results dir"
echo "=== engineering-manager ==="
ls -la .claude/results/engineering-manager/ 2>/dev/null || echo "no engineering-manager results dir"
echo "=== root results ==="
ls -la .claude/results/ 2>/dev/null || echo "no results dir"
