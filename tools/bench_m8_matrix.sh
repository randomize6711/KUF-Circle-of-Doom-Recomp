#!/usr/bin/env bash
# The M8 benchmark matrix (docs/PERFORMANCE.md section 7). Runs one game at a time, sandboxed (tools/bench.py).
#
#   tools/bench_m8_matrix.sh <tag> [cold|warm|all] [preset]
#
#   preset: build/<preset>/bin to run (default linux-amd64-relwithdebinfo); a frozen copy of a build lets the main tree
#   be rebuilt while the matrix runs.
#
#   cold  <tag>-cold: an empty shader/pipeline cache (every sandboxed run's default, a player's first visit): the menu,
#         exploration and fight at 3840x2160, 30 and 120 fps, once. Measures pipeline creation stalls; its caches
#         seed the warm runs.
#   warm  <tag>: the cache of the cold run of the same workload (a player's later sessions):
#         3840x2160 (primary target) at 30/60/120/144 fps, twice; 2560x1440 and 1920x1080 at 30/60/120/144 once
#         (both render the 2560x1440 frame at Render Native; offscreen runs do not exercise the output scaling);
#         the menu at 3840x2160 at 30 and 120, twice.
# About 2.5 hours for all. Results: python3 -I tools/bench.py report --tag <tag> [--compare <other tag>]
set -uo pipefail  # an invalid run is reported by bench.py and the matrix goes on
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
TAG="${1:?tag}"
WHAT="${2:-all}"
PRESET="${3:-linux-amd64-relwithdebinfo}"
B=(python3 -I tools/bench.py run --preset "$PRESET")
EXPLORE=tests/perf/bench_explore.toml
FIGHT=tests/perf/bench_fight.toml
MENU=tests/perf/bench_menu.toml

if [ "$WHAT" = cold ] || [ "$WHAT" = all ]; then
  "${B[@]}" --tag "$TAG-cold" --scenario "$MENU" --res 3840x2160 --fps 30
  "${B[@]}" --tag "$TAG-cold" --scenario "$EXPLORE" --scenario "$FIGHT" --res 3840x2160 --fps 30,120
fi
if [ "$WHAT" = warm ] || [ "$WHAT" = all ]; then
  "${B[@]}" --tag "$TAG" --warm "$TAG-cold" --scenario "$EXPLORE" --scenario "$FIGHT" --res 3840x2160 \
    --fps 30,60,120,144 --reps 2
  "${B[@]}" --tag "$TAG" --warm "$TAG-cold" --scenario "$EXPLORE" --scenario "$FIGHT" --res 2560x1440,1920x1080 \
    --fps 30,60,120,144
  "${B[@]}" --tag "$TAG" --warm "$TAG-cold" --scenario "$MENU" --res 3840x2160 --fps 30,120 --reps 2
fi
