#!/usr/bin/env bash
#
# Moonlight-Web — unified TNR (non-regression) gate.
#
# Runs both test suites with coverage and aggregates the result, exiting
# non-zero if either fails its gate. Intended for local pre-PR checks and CI.
#
#   Frontend : Vitest + v8 coverage   (70% on the in-scope JS modules)
#   Backend  : Qt Test + OpenCppCoverage (70% on the in-scope C++ units)
#
# Usage:  bash scripts/run-tests.sh
#
# The backend suite needs Windows + MSVC + Qt (+ OpenCppCoverage for the %).
# On other platforms it is skipped with a notice (frontend still gates).

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

fe_rc=0
be_rc=0

echo "════════════════════════════════════════"
echo " Frontend TNR (Vitest + coverage)"
echo "════════════════════════════════════════"
( cd "$ROOT/frontend" && npm run coverage ) || fe_rc=$?

echo
echo "════════════════════════════════════════"
echo " Backend TNR (Qt Test + OpenCppCoverage)"
echo "════════════════════════════════════════"
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*)
        bat="$(cygpath -w "$ROOT/backend/tests/run_coverage.bat")"
        cmd //c "$bat"
        be_rc=$?
        ;;
    *)
        echo "[skip] Backend tests require Windows + MSVC + Qt — skipped on $(uname -s)."
        ;;
esac

echo
echo "════════════════════════════════════════"
echo " TNR summary"
echo "════════════════════════════════════════"
[ "$fe_rc" -eq 0 ] && echo "  Frontend : ✅ PASS" || echo "  Frontend : ❌ FAIL (exit $fe_rc)"
[ "$be_rc" -eq 0 ] && echo "  Backend  : ✅ PASS" || echo "  Backend  : ❌ FAIL (exit $be_rc)"

[ "$fe_rc" -eq 0 ] && [ "$be_rc" -eq 0 ] && exit 0
exit 1
