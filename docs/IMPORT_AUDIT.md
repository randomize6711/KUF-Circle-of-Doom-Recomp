# Xbox System-Function Audit (M2)

The game imports 222 symbols: 210 functions and 12 data variables (e.g. `XboxHardwareInfo`,
`KeDebugMonitorData`; the variables are provided by the runtime's export table and were not audited
separately). This audit covers the 210 functions.

**Method.**
- Static: `tools/import_audit_report.py` finds each import's implementation in the ReXGlue sources and
  scans it for incomplete-behaviour markers (SDK stub macros, TODO/STUB/unimplemented, `debug::Break`,
  `assert_always`).
- Dynamic: the `linux-amd64-audit` build interposes a wrapper for every import that counts calls and logs
  the first calls with their arguments.
- Manual: the 19 called functions that matter for initialisation or carry markers were reviewed by hand
  (`config/v1/import_review.toml`).

Data: the 5-minute smoke-test run (`ctest -L smoke`), which boots to the title screen and plays its movies.

## Result

| Status | Count | Meaning |
|---|---|---|
| Fully implemented | 99 | Called. 19 reviewed by hand; the other 80 have no incompleteness markers and behave consistently with the run (no errors, game progresses), but were not reviewed line by line |
| Partially implemented | 4 | Called; works for what the game uses so far, with known gaps: `RtlRaiseException` (only thread naming; other exceptions fail fast through our guard), `NetDll_WSAStartup`, `NetDll_XNetStartup` (offline only), `XMsgStartIORequest` (XGI rich presence accepted as a no-op) |
| Intentionally stubbed | 2 | `VdCallGraphicsNotificationRoutines` (returns 0), `KeEnableFpuExceptions` (no host FP traps) |
| Unused | 105 | Not called during boot and title screen |
| Not yet investigated | 0 | Every exercised import has a located implementation |

No import was replaced with an arbitrary success value by this project. The two stubs are the SDK's,
reviewed and accepted.

## Unused imports that later milestones will exercise

- **Saves (M5/M6):** `XamContentCreateEx`, `XamContentClose`, `XamContentFlush`, `XamContentDelete`,
  `XamContentCreateEnumerator`, `XamContentGetCreator`, `XamContentGetDeviceData`,
  `XamContentGetDeviceState`, `XamShowDeviceSelectorUI`. Content packages must map to the user-data
  directory with atomic writes (ROADMAP M5/M6).
- **Native LAN (M11, interfaces to preserve):** `NetDll_*` sockets (18), the XNet address/QoS functions
  (`XNetGetTitleXnAddr`, `XNetXnAddrToInAddr`, `XNetInAddrToXnAddr`, `XNetQos*`, `XNetDns*`),
  `XamSessionCreateHandle`, `XamSessionRefObjByHandle`, XHV voice (`XamVoice*`).
- **System UI (M4+):** `XamShowSigninUI`, `XamShowMessageBoxUIEx`, `XamShowPlayersUI`, `XamShowFriendsUI`,
  `XamShowGamerCardUIForXUID`, `XamShowMarketplaceUI`, `XamShowPlayerReviewUI`, `XamShowDirtyDiscErrorUI`.
  Headless runs auto-answer these; they need a PC equivalent or a sensible default.

## Offline player profile

The runtime's offline local profile is used: user 0 is signed in locally (sign-in state 1) as "User"
with default profile settings, and users 1–3 are signed out. The game queried all four slots and read
user 0's profile settings five times. The player name becomes configurable in M9; online services are
not used.

## Full table (generated)

| Import | Lib | Status | Calls (audit) | First call (s) | SDK implementation | Note |
|---|---|---|---|---|---|---|
| NetDll_WSAStartup | xam | Partially implemented | 3 | 0.43 | implemented (TODO) `src/kernel/xam/xam_net.cpp` | Winsock startup succeeds; networking stays offline in M2. Interface kept for native LAN (M11). |
| RtlRaiseException | xboxkrnl | Partially implemented | 2 | 0.45 | implemented (TODO, debug::Break) `src/kernel/xboxkrnl/xboxkrnl_debug.cpp` | Runtime handles only SetThreadName (0x406D1388); other codes Break and return. Our RaiseException guard (src/runtime/eh_trace.cpp) passes thread naming through and fails fast on anything else. Observed: 2 calls, both thread naming. |
| NetDll_XNetStartup | xam | Partially implemented | 1 | 0.99 | implemented `src/kernel/xam/xam_net.cpp` | XNet startup succeeds; no Xbox Live. Interface kept for native LAN (M11). |
| XMsgStartIORequest | xam | Partially implemented | 3 | 10.57 | implemented `src/kernel/xam/xam_msg.cpp` | Observed: XGI (app 0xFB) message 0x000B0006 XUserSetContextEx (rich presence) accepted as a successful no-op, correct offline. Other XAM app messages are handled per app (apps/*.cpp); unknown ones not observed. |
| VdCallGraphicsNotificationRoutines | xboxkrnl | Intentionally stubbed | 1 | 0.98 | implemented (TODO) `src/kernel/xboxkrnl/xboxkrnl_video.cpp` | Returns 0; display-mode-change notifications are not emitted. No effect observed. |
| KeEnableFpuExceptions | xboxkrnl | Intentionally stubbed | 32 | 0.98 | implemented (TODO) `src/kernel/xboxkrnl/xboxkrnl_misc.cpp` | No-op: guest FP exception enabling is not mapped to host traps (would turn benign FP conditions into crashes). |
| RtlImageXexHeaderField | xboxkrnl | Fully implemented | 1 | 0.43 | implemented `src/kernel/xboxkrnl/xboxkrnl_rtl.cpp` |  |
| NtAllocateVirtualMemory | xboxkrnl | Fully implemented | 2443 | 0.43 | implemented (assert_always) `src/kernel/xboxkrnl/xboxkrnl_memory.cpp` | Reviewed: assert_always only on unsupported flag combinations; 2,442 calls without error. |
| KeGetCurrentProcessType | xboxkrnl | Fully implemented | 3 | 0.43 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| RtlInitializeCriticalSection | xboxkrnl | Fully implemented | 448 | 0.43 | implemented `src/kernel/xboxkrnl/xboxkrnl_rtl.cpp` |  |
| RtlEnterCriticalSection | xboxkrnl | Fully implemented | 4094293 | 0.43 | implemented `src/kernel/xboxkrnl/xboxkrnl_rtl.cpp` |  |
| RtlLeaveCriticalSection | xboxkrnl | Fully implemented | 4375028 | 0.43 | implemented `src/kernel/xboxkrnl/xboxkrnl_rtl.cpp` |  |
| XexCheckExecutablePrivilege | xboxkrnl | Fully implemented | 1 | 0.43 | implemented `src/kernel/xboxkrnl/xboxkrnl_modules.cpp` |  |
| KeTlsAlloc | xboxkrnl | Fully implemented | 11 | 0.43 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| KeTlsSetValue | xboxkrnl | Fully implemented | 143037 | 0.43 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| KeQuerySystemTime | xboxkrnl | Fully implemented | 1 | 0.43 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| NtFreeVirtualMemory | xboxkrnl | Fully implemented | 2168 | 0.43 | implemented `src/kernel/xboxkrnl/xboxkrnl_memory.cpp` |  |
| NtCreateEvent | xboxkrnl | Fully implemented | 240 | 0.43 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| KeQueryPerformanceFrequency | xboxkrnl | Fully implemented | 28697 | 0.43 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| XamNotifyCreateListener | xam | Fully implemented | 4 | 0.43 | implemented `src/kernel/xam/xam_notify.cpp` |  |
| XamGetSystemVersion | xam | Fully implemented | 10 | 0.43 | implemented `src/kernel/xam/xam_info.cpp` |  |
| KeInitializeSemaphore | xboxkrnl | Fully implemented | 1 | 0.43 | implemented (assert_always) `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` | Reviewed: assert covers invalid limits only. |
| ExRegisterTitleTerminateNotification | xboxkrnl | Fully implemented | 2 | 0.43 | implemented `src/kernel/xboxkrnl/xboxkrnl_modules.cpp` |  |
| ExCreateThread | xboxkrnl | Fully implemented | 35 | 0.43 | implemented (fail-path) `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` | Reviewed: fail-path marker is the error return; 35 threads created (incl. OpenMP workers and Job Threads). |
| ObReferenceObjectByHandle | xboxkrnl | Fully implemented | 42 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_ob.cpp` |  |
| KeSetBasePriorityThread | xboxkrnl | Fully implemented | 3 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| KeResumeThread | xboxkrnl | Fully implemented | 1 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| ObDereferenceObject | xboxkrnl | Fully implemented | 36 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_ob.cpp` |  |
| KeWaitForSingleObject | xboxkrnl | Fully implemented | 335362 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| KeRaiseIrqlToDpcLevel | xboxkrnl | Fully implemented | 9184860 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| KeAcquireSpinLockAtRaisedIrql | xboxkrnl | Fully implemented | 8039942 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| KeReleaseSpinLockFromRaisedIrql | xboxkrnl | Fully implemented | 8039942 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| KfLowerIrql | xboxkrnl | Fully implemented | 8022320 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| XAudioRegisterRenderDriverClient | xboxkrnl | Fully implemented | 1 | 0.44 | implemented (fail-path) `src/kernel/xboxkrnl/xboxkrnl_audio.cpp` | Reviewed: one client registered; render driver frames submitted every ~5.3 ms. |
| KeSetEvent | xboxkrnl | Fully implemented | 615700 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| KeWaitForMultipleObjects | xboxkrnl | Fully implemented | 281499 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| XAudioGetVoiceCategoryVolumeChangeMask | xboxkrnl | Fully implemented | 281499 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_audio.cpp` |  |
| XamUserReadProfileSettings | xam | Fully implemented | 5 | 0.44 | implemented `src/kernel/xam/xam_user.cpp` | Offline profile defaults (SDK UserProfile); 5 reads for user 0. |
| XAudioSubmitRenderDriverFrame | xboxkrnl | Fully implemented | 281498 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_audio.cpp` |  |
| NtWaitForSingleObjectEx | xboxkrnl | Fully implemented | 65757 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| NtCreateTimer | xboxkrnl | Fully implemented | 1 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| NtSetTimerEx | xboxkrnl | Fully implemented | 1 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| NtSetEvent | xboxkrnl | Fully implemented | 35750 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| XNotifyPositionUI | xam | Fully implemented | 1 | 0.44 | implemented `src/kernel/xam/xam_notify.cpp` |  |
| NtCreateSemaphore | xboxkrnl | Fully implemented | 9 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| KeSetAffinityThread | xboxkrnl | Fully implemented | 33 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| NtResumeThread | xboxkrnl | Fully implemented | 55 | 0.44 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| RtlNtStatusToDosError | xboxkrnl | Fully implemented | 4974 | 0.45 | implemented `src/kernel/xboxkrnl/xboxkrnl_error.cpp` |  |
| KeTlsGetValue | xboxkrnl | Fully implemented | 179467 | 0.45 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| MmAllocatePhysicalMemoryEx | xboxkrnl | Fully implemented | 2068 | 0.45 | implemented `src/kernel/xboxkrnl/xboxkrnl_memory.cpp` |  |
| NtWaitForMultipleObjectsEx | xboxkrnl | Fully implemented | 3 | 0.45 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| MmQueryAllocationSize | xboxkrnl | Fully implemented | 745 | 0.45 | implemented `src/kernel/xboxkrnl/xboxkrnl_memory.cpp` |  |
| RtlInitAnsiString | xboxkrnl | Fully implemented | 45 | 0.45 | implemented `src/kernel/xboxkrnl/xboxkrnl_rtl.cpp` |  |
| NtCreateFile | xboxkrnl | Fully implemented | 44 | 0.45 | implemented (fail-path) `src/kernel/xboxkrnl/xboxkrnl_io.cpp` | Reviewed: fail-path marker is the normal not-found return. Game folder is read-only; write opens downgrade to read (Xenia behaviour), rename refused (patch 0001). |
| NtQueryInformationFile | xboxkrnl | Fully implemented | 33 | 0.45 | implemented (TODO, assert_always, stub, unimplemented) `src/kernel/xboxkrnl/xboxkrnl_io_info.cpp` | Reviewed: the unimplemented info classes (XCTD compression) log at error level, none logged; XFileSectorInformation is a debug-logged stub, not hit. |
| NtReadFile | xboxkrnl | Fully implemented | 6018 | 0.45 | implemented (TODO, fail-path) `src/kernel/xboxkrnl/xboxkrnl_io.cpp` | Reviewed: completes synchronously (async TODO) and signals the event/IOSB; 6,013 reads in 5 min without error. |
| NtClose | xboxkrnl | Fully implemented | 207 | 0.45 | implemented `src/kernel/xboxkrnl/xboxkrnl_ob.cpp` |  |
| XNotifyGetNext | xam | Fully implemented | 38933 | 0.45 | implemented `src/kernel/xam/xam_notify.cpp` |  |
| NtSetInformationFile | xboxkrnl | Fully implemented | 1042 | 0.64 | implemented (assert_always) `src/kernel/xboxkrnl/xboxkrnl_io_info.cpp` | Reviewed: assert_always only on unsupported classes; 1,042 calls (file position) without error log. |
| XGetVideoMode | xam | Fully implemented | 6 | 0.98 | implemented (TODO) `src/kernel/xam/xam_video.cpp` | Reviewed: reports 1280x720 widescreen 60 Hz (satisfies the game's PAL-50 refusal check). |
| VdInitializeEngines | xboxkrnl | Fully implemented | 1 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| VdSetGraphicsInterruptCallback | xboxkrnl | Fully implemented | 1 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| ExGetXConfigSetting | xboxkrnl | Fully implemented | 2 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_xconfig.cpp` |  |
| VdSetSystemCommandBufferGpuIdentifierAddress | xboxkrnl | Fully implemented | 2 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| MmGetPhysicalAddress | xboxkrnl | Fully implemented | 587 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_memory.cpp` |  |
| VdInitializeRingBuffer | xboxkrnl | Fully implemented | 1 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| VdEnableRingBufferRPtrWriteBack | xboxkrnl | Fully implemented | 1 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| KiApcNormalRoutineNop | xboxkrnl | Fully implemented | 1 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| VdQueryVideoMode | xboxkrnl | Fully implemented | 2 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| VdQueryVideoFlags | xboxkrnl | Fully implemented | 2 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| VdGetCurrentDisplayGamma | xboxkrnl | Fully implemented | 1 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| KeEnterCriticalRegion | xboxkrnl | Fully implemented | 8937 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| VdRetrainEDRAMWorker | xboxkrnl | Fully implemented | 1 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| VdRetrainEDRAM | xboxkrnl | Fully implemented | 8937 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| KeLeaveCriticalRegion | xboxkrnl | Fully implemented | 8937 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| VdIsHSIOTrainingSucceeded | xboxkrnl | Fully implemented | 1 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| VdGetSystemCommandBuffer | xboxkrnl | Fully implemented | 8936 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| VdSwap | xboxkrnl | Fully implemented | 8936 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| VdSetDisplayMode | xboxkrnl | Fully implemented | 1 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| VdGetCurrentDisplayInformation | xboxkrnl | Fully implemented | 1 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| RtlFillMemoryUlong | xboxkrnl | Fully implemented | 1 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_rtl.cpp` |  |
| VdInitializeScalerCommandBuffer | xboxkrnl | Fully implemented | 1 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| VdPersistDisplay | xboxkrnl | Fully implemented | 1 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| MmFreePhysicalMemory | xboxkrnl | Fully implemented | 406 | 0.98 | implemented `src/kernel/xboxkrnl/xboxkrnl_memory.cpp` |  |
| NtQueryFullAttributesFile | xboxkrnl | Fully implemented | 1 | 0.98 | implemented (assert_always) `src/kernel/xboxkrnl/xboxkrnl_io.cpp` | Reviewed: assert only for root-directory-relative paths (not used). Returns NO_SUCH_FILE for the ShaderDumpxe debug probe, as on hardware. |
| KeTlsFree | xboxkrnl | Fully implemented | 4 | 0.99 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| KfAcquireSpinLock | xboxkrnl | Fully implemented | 26895 | 0.99 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| KfReleaseSpinLock | xboxkrnl | Fully implemented | 26895 | 0.99 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| RtlTryEnterCriticalSection | xboxkrnl | Fully implemented | 280736 | 1.25 | implemented `src/kernel/xboxkrnl/xboxkrnl_rtl.cpp` |  |
| XamInputGetState | xam | Fully implemented | 35748 | 1.32 | implemented `src/kernel/xam/xam_input.cpp` |  |
| XamInputSetState | xam | Fully implemented | 35748 | 1.32 | implemented `src/kernel/xam/xam_input.cpp` |  |
| XamUserGetSigninState | xam | Fully implemented | 10 | 1.32 | implemented `src/kernel/xam/xam_user.cpp` | Offline local profile: user 0 signed in locally (state 1, 'User'), users 1-3 signed out. |
| NtClearEvent | xboxkrnl | Fully implemented | 53622 | 1.32 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| InterlockedPopEntrySList | xboxkrnl | Fully implemented | 142992 | 1.32 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| NtReleaseSemaphore | xboxkrnl | Fully implemented | 17873 | 1.32 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| XGetGameRegion | xam | Fully implemented | 242 | 1.34 | implemented `src/kernel/xam/xam_info.cpp` |  |
| XMACreateContext | xboxkrnl | Fully implemented | 3 | 10.58 | implemented `src/kernel/xboxkrnl/xboxkrnl_audio_xma.cpp` |  |
| KeResetEvent | xboxkrnl | Fully implemented | 20 | 10.60 | implemented (assert_always) `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` | Reviewed: assert covers non-event objects only. |
| MmQueryAddressProtect | xboxkrnl | Fully implemented | 26103 | 10.62 | implemented `src/kernel/xboxkrnl/xboxkrnl_memory.cpp` |  |
| KeDelayExecutionThread | xboxkrnl | Fully implemented | 17 | 10.63 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| NtSuspendThread | xboxkrnl | Fully implemented | 10 | 28.55 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| XMsgInProcessCall | xam | Fully implemented | 2 | 70.58 | implemented `src/kernel/xam/xam_msg.cpp` |  |
| XMsgStartIORequestEx | xam | Fully implemented | 4 | 70.59 | implemented `src/kernel/xam/xam_msg.cpp` |  |
| ExTerminateThread | xboxkrnl | Fully implemented | 16 | 70.65 | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| XMAReleaseContext | xboxkrnl | Fully implemented | 2 | 149.09 | implemented `src/kernel/xboxkrnl/xboxkrnl_audio_xma.cpp` |  |
| DbgBreakPoint | xboxkrnl | Unused | 0 |  | implemented (debug::Break) `src/kernel/xboxkrnl/xboxkrnl_debug.cpp` | M3 (K12): replaced by the executable (`src/runtime/fatal_trap.cpp`): fatal with a crash report, as an unhandled breakpoint is on a retail console |
| DbgPrint | xboxkrnl | Unused | 0 |  | not found `` |  |
| HalReturnToFirmware | xboxkrnl | Unused | 0 |  | implemented (TODO) `src/kernel/xboxkrnl/xboxkrnl_hal.cpp` |  |
| InterlockedFlushSList | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| KeBugCheck | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_debug.cpp` | M3 (K12): replaced by the executable (`src/runtime/fatal_trap.cpp`): fatal with a crash report naming the code and call site |
| KeBugCheckEx | xboxkrnl | Unused | 0 |  | implemented (assert_always, debug::Break) `src/kernel/xboxkrnl/xboxkrnl_debug.cpp` | M3 (K12): replaced by the executable (`src/runtime/fatal_trap.cpp`): fatal with a crash report naming the code, parameters and call site |
| KeLockL2 | xboxkrnl | Unused | 0 |  | implemented (TODO) `src/kernel/xboxkrnl/xboxkrnl_memory.cpp` |  |
| KeReleaseSemaphore | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| KeUnlockL2 | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_memory.cpp` |  |
| MmQueryStatistics | xboxkrnl | Unused | 0 |  | implemented (TODO) `src/kernel/xboxkrnl/xboxkrnl_memory.cpp` |  |
| NetDll_WSACleanup | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_net.cpp` |  |
| NetDll_WSACreateEvent | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_net.cpp` |  |
| NetDll_WSAGetLastError | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_net.cpp` |  |
| NetDll_XNetDnsLookup | xam | Unused | 0 |  | implemented (TODO) `src/kernel/xam/xam_net.cpp` |  |
| NetDll_XNetDnsRelease | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_net.cpp` |  |
| NetDll_XNetGetTitleXnAddr | xam | Unused | 0 |  | implemented (TODO) `src/kernel/xam/xam_net.cpp` |  |
| NetDll_XNetInAddrToXnAddr | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_net.cpp` |  |
| NetDll_XNetQosListen | xam | Unused | 0 |  | implemented (fail-path) `src/kernel/xam/xam_net.cpp` |  |
| NetDll_XNetQosLookup | xam | Unused | 0 |  | SDK stub `src/kernel/xam/xam_net.cpp` |  |
| NetDll_XNetQosRelease | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_net.cpp` |  |
| NetDll_XNetXnAddrToInAddr | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_net.cpp` |  |
| NetDll_XNetXnAddrToMachineId | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_net.cpp` |  |
| NetDll___WSAFDIsSet | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_net.cpp` |  |
| NetDll_accept | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_net.cpp` |  |
| NetDll_bind | xam | Unused | 0 |  | implemented (fail-path) `src/kernel/xam/xam_net.cpp` |  |
| NetDll_closesocket | xam | Unused | 0 |  | implemented (TODO) `src/kernel/xam/xam_net.cpp` |  |
| NetDll_connect | xam | Unused | 0 |  | implemented (fail-path) `src/kernel/xam/xam_net.cpp` |  |
| NetDll_inet_addr | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_net.cpp` |  |
| NetDll_ioctlsocket | xam | Unused | 0 |  | implemented (TODO, fail-path) `src/kernel/xam/xam_net.cpp` |  |
| NetDll_listen | xam | Unused | 0 |  | implemented (fail-path) `src/kernel/xam/xam_net.cpp` |  |
| NetDll_recv | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_net.cpp` |  |
| NetDll_recvfrom | xam | Unused | 0 |  | implemented (TODO) `src/kernel/xam/xam_net.cpp` |  |
| NetDll_select | xam | Unused | 0 |  | implemented (TODO) `src/kernel/xam/xam_net.cpp` |  |
| NetDll_send | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_net.cpp` |  |
| NetDll_sendto | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_net.cpp` |  |
| NetDll_setsockopt | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_net.cpp` |  |
| NetDll_socket | xam | Unused | 0 |  | implemented (fail-path) `src/kernel/xam/xam_net.cpp` |  |
| NtCancelTimer | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_threading.cpp` |  |
| NtDuplicateObject | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_ob.cpp` |  |
| NtFlushBuffersFile | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_io.cpp` |  |
| NtOpenFile | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_io.cpp` |  |
| NtQueryDirectoryFile | xboxkrnl | Unused | 0 |  | implemented (fail-path) `src/kernel/xboxkrnl/xboxkrnl_io.cpp` |  |
| NtQueryVirtualMemory | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_memory.cpp` |  |
| NtQueryVolumeInformationFile | xboxkrnl | Unused | 0 |  | implemented (TODO, assert_always, stub) `src/kernel/xboxkrnl/xboxkrnl_io_info.cpp` |  |
| NtReadFileScatter | xboxkrnl | Unused | 0 |  | implemented (TODO, fail-path) `src/kernel/xboxkrnl/xboxkrnl_io.cpp` |  |
| NtWriteFile | xboxkrnl | Unused | 0 |  | implemented (TODO, fail-path) `src/kernel/xboxkrnl/xboxkrnl_io.cpp` |  |
| ObCreateSymbolicLink | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_ob.cpp` |  |
| ObDeleteSymbolicLink | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_ob.cpp` |  |
| RtlCaptureContext | xboxkrnl | Unused | 0 |  | implemented (TODO, stub) `src/kernel/xboxkrnl/xboxkrnl_rtl.cpp` |  |
| RtlCompareMemoryUlong | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_rtl.cpp` |  |
| RtlMultiByteToUnicodeN | xboxkrnl | Unused | 0 |  | implemented (TODO) `src/kernel/xboxkrnl/xboxkrnl_rtl.cpp` |  |
| RtlTimeFieldsToTime | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_rtl.cpp` |  |
| RtlTimeToTimeFields | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_rtl.cpp` |  |
| RtlUnicodeToMultiByteN | xboxkrnl | Unused | 0 |  | implemented (TODO) `src/kernel/xboxkrnl/xboxkrnl_rtl.cpp` |  |
| RtlUnwind | xboxkrnl | Unused | 0 |  | implemented (TODO, stub) `src/kernel/xboxkrnl/xboxkrnl_rtl.cpp` |  |
| VdEnableDisableClockGating | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| VdShutdownEngines | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_video.cpp` |  |
| XAudioGetVoiceCategoryVolume | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_audio.cpp` |  |
| XAudioUnregisterRenderDriverClient | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_audio.cpp` |  |
| XGetAVPack | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_info.cpp` |  |
| XGetLanguage | xam | Unused | 0 |  | implemented (TODO) `src/kernel/xam/xam_info.cpp` |  |
| XMsgCancelIORequest | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_msg.cpp` |  |
| XamAlloc | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_info.cpp` |  |
| XamContentClose | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_content.cpp` |  |
| XamContentCreateEnumerator | xam | Unused | 0 |  | implemented (TODO, fail-path) `src/kernel/xam/xam_content.cpp` |  |
| XamContentCreateEx | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_content.cpp` |  |
| XamContentDelete | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_content.cpp` |  |
| XamContentFlush | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_content.cpp` |  |
| XamContentGetCreator | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_content.cpp` |  |
| XamContentGetDeviceData | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_content_device.cpp` |  |
| XamContentGetDeviceState | xam | Unused | 0 |  | implemented (fail-path) `src/kernel/xam/xam_content_device.cpp` |  |
| XamEnumerate | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_enum.cpp` |  |
| XamFree | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_info.cpp` |  |
| XamGetExecutionId | xam | Unused | 0 |  | implemented (fail-path) `src/kernel/xam/xam_info.cpp` |  |
| XamInputGetCapabilities | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_input.cpp` |  |
| XamLoaderLaunchTitle | xam | Unused | 0 |  | implemented (assert_always) `src/kernel/xam/xam_info.cpp` |  |
| XamLoaderTerminateTitle | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_info.cpp` |  |
| XamSessionCreateHandle | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_user.cpp` |  |
| XamSessionRefObjByHandle | xam | Unused | 0 |  | implemented (TODO) `src/kernel/xam/xam_user.cpp` |  |
| XamShowDeviceSelectorUI | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_ui.cpp` |  |
| XamShowDirtyDiscErrorUI | xam | Unused | 0 |  | implemented (TODO, unimplemented) `src/kernel/xam/xam_ui.cpp` |  |
| XamShowFriendsUI | xam | Unused | 0 |  | SDK stub `src/kernel/xam/xam_ui.cpp` |  |
| XamShowGamerCardUIForXUID | xam | Unused | 0 |  | SDK stub `src/kernel/xam/xam_ui.cpp` |  |
| XamShowMarketplaceUI | xam | Unused | 0 |  | SDK stub `src/kernel/xam/xam_ui.cpp` |  |
| XamShowMessageBoxUIEx | xam | Unused | 0 |  | implemented (TODO, stub) `src/kernel/xam/xam_ui.cpp` |  |
| XamShowPlayerReviewUI | xam | Unused | 0 |  | SDK stub `src/kernel/xam/xam_ui.cpp` |  |
| XamShowPlayersUI | xam | Unused | 0 |  | SDK stub `src/kernel/xam/xam_ui.cpp` |  |
| XamShowSigninUI | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_user.cpp` |  |
| XamUserAreUsersFriends | xam | Unused | 0 |  | implemented (assert_always, fail-path) `src/kernel/xam/xam_user.cpp` |  |
| XamUserCheckPrivilege | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_user.cpp` |  |
| XamUserCreateStatsEnumerator | xam | Unused | 0 |  | SDK stub `src/kernel/xam/xam_user.cpp` |  |
| XamUserGetName | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_user.cpp` |  |
| XamUserGetXUID | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_user.cpp` |  |
| XamVoiceClose | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_voice.cpp` |  |
| XamVoiceCreate | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_voice.cpp` |  |
| XamVoiceHeadsetPresent | xam | Unused | 0 |  | implemented `src/kernel/xam/xam_voice.cpp` |  |
| XamVoiceSubmitPacket | xam | Unused | 0 |  | SDK stub `src/kernel/xam/xam_voice.cpp` |  |
| XexGetModuleHandle | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_modules.cpp` |  |
| XexGetProcedureAddress | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_modules.cpp` |  |
| XexLoadImage | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_modules.cpp` |  |
| XexUnloadImage | xboxkrnl | Unused | 0 |  | implemented `src/kernel/xboxkrnl/xboxkrnl_modules.cpp` |  |
| __C_specific_handler | xboxkrnl | Unused | 0 |  | implemented (TODO, stub) `src/kernel/xboxkrnl/xboxkrnl_rtl.cpp` |  |
| _vsnprintf | xboxkrnl | Unused | 0 |  | not found `` |  |
| sprintf | xboxkrnl | Unused | 0 |  | not found `` |  |
| vsprintf | xboxkrnl | Unused | 0 |  | not found `` |  |
