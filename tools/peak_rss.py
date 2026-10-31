#!/usr/bin/env python3
"""Run a command and fail if its peak resident memory exceeds a limit (test harness guard).

  python3 -I tools/peak_rss.py --max-mb N -- COMMAND [ARGS...]

Runs COMMAND, passes its output through, then prints "peak RSS: M MB (limit N MB)". The exit status is COMMAND's,
or 1 if it succeeded but its largest process (it or any child it waited for) used more than N MB. The peak comes from
the kernel's accounting at exit (wait4 ru_maxrss), so short spikes are counted.

Why: a check that read every capture of a 4K run as float64 took 14 GB and, beside a game run and other checks at
ctest -j6, exhausted the machine's memory (docs/TESTING.md). Checks that read many captures run under this guard.
"""
import os
import subprocess
import sys


def main():
    args = sys.argv[1:]
    if len(args) < 4 or args[0] != "--max-mb" or args[2] != "--":
        sys.exit(__doc__)
    limit = int(args[1])
    proc = subprocess.Popen(args[3:])
    _, status, usage = os.wait4(proc.pid, 0)
    code = os.waitstatus_to_exitcode(status)
    peak = usage.ru_maxrss // 1024
    print(f"peak RSS: {peak} MB (limit {limit} MB)")
    if code == 0 and peak > limit:
        print(f"PEAK RSS: FAIL ({peak} MB over the {limit} MB limit)")
        return 1
    return code


if __name__ == "__main__":
    sys.exit(main())
