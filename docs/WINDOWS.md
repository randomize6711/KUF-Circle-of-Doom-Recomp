# Windows Compatibility (M2 investigation)

**Status: not verified.** No Windows build has been produced or run. Nothing here claims Windows support.

## What was tried (2026-10-20)

The ReXGlue SDK's supported Windows toolchain is **clang-cl with the MSVC CRT and the Windows SDK**
(presets `win-amd64-*`, Visual Studio 2022 with the Clang components). Neither the MSVC CRT nor the
Windows SDK is installed on the development machine. Getting them on Linux (e.g. with `xwin`) means
downloading Microsoft-licensed files, which needs the user's approval, so that was not done.

As a no-download probe, the project was cross-configured with **Clang targeting MinGW-w64**
(`cmake/mingw-w64-x86_64.cmake`, using the installed `mingw-w64-gcc` headers and libraries with `lld`).
Configure succeeds. The build stops on toolchain mismatches, not on project code:

| Error | Count | Cause |
|---|---|---|
| FFmpeg `crtversion.h` not found | 151 | FFmpeg's MSVC configuration expects the MSVC CRT |
| `ObjBase.h` / `Windows.h` / `WinSock2.h` not found | ~32 | The SDK includes Windows headers in MSVC's mixed case; MinGW installs them lowercase on a case-sensitive filesystem |
| XInput `FARPROC` → `void*` | 6 | MSVC accepts the conversion, Clang-on-MinGW rejects it |
| `_mm_shuffle_epi8` needs SSSE3 | ~12 | Probe configuration only: the presets pass `-march=x86-64-v2`, the probe did not |

**Conclusion:** MinGW is not a practical target without broad SDK changes. The supported clang-cl + MSVC
route is the way to go, and it needs either a Windows machine or the user's approval to install
Microsoft's CRT/SDK for cross-compilation.

## Project code readiness

- `src/runtime/crash_report.cpp`: Windows branches (`ReadProcessMemory`, `_exit`) exist but are
  untested. Host backtraces are Linux-only for now.
- `src/runtime/telemetry.cpp`: CPU usage is measured on Linux only and reports 0 elsewhere.
- Import audit (`KUFCOD_IMPORT_AUDIT`) is Linux-only by design (ELF symbol interposition); configure fails
  if it's enabled on other platforms.
- Tests: the fork-based crash-report test is built only on Unix. The VFS test uses a portable scratch
  directory.
- Tooling: `tools/run_sandboxed.sh` and the smoke test depend on bubblewrap (Linux). Windows needs its
  own runner (e.g. a separate test account and directory ACLs) and a smoke-test variant.

## Next steps (proposed)
1. User decision: Windows test machine/VM, or approval for an `xwin`-based clang-cl cross toolchain.
2. Add a Windows CI job (GitHub Actions `windows-latest`, clang-cl) once a remote exists. No remote
   exists yet.
3. Port the runner and smoke test, then run the 5-minute smoke test natively on Windows.
