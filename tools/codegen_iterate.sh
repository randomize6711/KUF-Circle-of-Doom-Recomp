#!/usr/bin/env bash
# Regenerate recompiled code for v1 and audit it until no new indirect-only functions are found.
# Usage: tools/codegen_iterate.sh   (run from the repo root; needs build/input/v1 and build/analysis/v1)
set -euo pipefail
cd "$(dirname "$0")/.."
REXGLUE=${REXGLUE:-build/linux-amd64-relwithdebinfo/bin/rexgluerd}
A=build/analysis/v1
for pass in 1 2 3 4 5; do
  "$REXGLUE" codegen kuf_cod_manifest.toml > build/logs/codegen.log 2>&1 || { tail -30 build/logs/codegen.log; exit 1; }
  python3 -I tools/audit_coverage.py generated/default $A/flat.dis $A/image.bin --report $A/coverage.txt >/dev/null
  before=$(grep -c '^0x' config/v1/indirect_functions.toml 2>/dev/null || echo 0)
  python3 -I tools/indirect_targets.py generated/default $A/image.bin $A/coverage.txt $A/flat.dis config/v1/indirect_functions.toml
  after=$(grep -c '^0x' config/v1/indirect_functions.toml)
  echo "pass $pass: indirect-only functions $before -> $after"
  [ "$before" = "$after" ] && break
done
python3 -I tools/audit_coverage.py generated/default $A/flat.dis $A/image.bin --report $A/coverage.txt
