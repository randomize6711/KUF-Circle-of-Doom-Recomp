# Recompilation (M1)

Input: `default.xex` v1 (SHA-256 `3ec406cc…29ff`, title 424507D1, media 7092B5EC), staged read-only to
`build/input/v1/` by `tools/stage_input.sh`. Toolchain: ReXGlue SDK `c94f5eb` (v0.10.0 + 2 commits),
Clang 23.1.3.

## Result

| Measure | Value |
|---|---|
| Codegen time | ~15 s for the whole image |
| Generated functions | 28,908 (`.pdata` lists 22,650) |
| Instructions emitted | 2,365,019 of 2,380,430 non-zero code words |
| Unimplemented instructions (`REX_UNIMPLEMENTED`) | **0** |
| Unknown VMX128 forms (`__builtin_debugtrap`) | **0** |
| Unresolved calls/branches in codegen log | **0** |
| Reconstructed switch tables | 689 |
| Indirect calls (`bctrl` / tail `bctr`) | 18,832 |
| `twi`/`tw` traps emitted | 8,050 (assertion traps; runtime decides behaviour) |
| Mnemonic cross-check vs LLVM disassembly | **0 mismatches** (`tools/audit_coverage.py`) |
| Generated C++ | 330 files, 166 MB; compiles with Clang in ~2.5 min at `-j8` (≈260 MB peak per TU) |

### Code words not emitted (15,411)

| Kind | Words | Notes |
|---|---|---|
| Jump/address tables inside `.text` | 14,285 | Switch tables: data, read by the generated `switch` |
| Import thunk block `0x82A1D364–0x82A1E084` | 840 | Resolved as imports, not code |
| Lone unreachable `blr` | 67 | Compiler padding after tail jumps |
| Other (38 runs) | 219 | SEH/C++ EH funclets and continuations (see below) and dead code |

### Determinism
A fresh codegen is bit-identical across runs and across the RelWithDebInfo and Release builds of the CLI.
This was checked by regenerating into separate directories and diffing them: only the stamp and dependency
files differ. Incremental runs reuse the previous function-to-file assignment (`codegen.partition.json`)
to limit rebuilds, so their file layout can differ from a fresh run while the function code is the same.

## Overrides needed

### Tail-jump-only leaf functions (`config/v1/functions.toml`, 5 entries)
Leaf functions with no `.pdata` record that are only reached by an unconditional `b` were never
claimed. Each was checked in the disassembly; the boundaries are documented inline.

### Pointer-only functions (`config/v1/indirect_functions.toml`, 103 entries, generated)
Small virtual methods, dispatch-table entries and member thunks referenced only through pointers.
They have no `.pdata` and are never `bl` targets, and the game has almost no RTTI, so ReXGlue's vtable
scanner cannot find them. Without a function-table entry an indirect call aborts at runtime. This was
observed in the first sandboxed runs (`0x82A08D40` on the audio thread, `0x821F89E8` on the main thread).

`tools/indirect_targets.py` finds them from:
- 32-bit code pointers stored in `.rdata`/`.data` (83 functions), and
- code addresses built with `lis` + `addi`/`ori` (20 functions; e.g. the `lwz/lwz/mtctr/bctr` thunk run at
  `0x821F89C8–0x821F8A18`).

Each candidate's extent comes from walking its control flow. Gap-fill fragments it branches into are
absorbed with an explicit `end` (e.g. `0x82A08D40` ends at `0x82A08D5C`, absorbing the three-instruction
error-return fragment). Candidates are rejected if they run into the body of a real function:
- 30 SEH `__except`/`__finally` funclets referenced from the scope tables at `0x8211BE68–0x8211C5E8`,
- 3 C++ catch continuation addresses (resume points returned in `r3` by catch funclets),
- 583 addresses that point at switch-table data.

`tools/codegen_iterate.sh` reruns codegen + audit + candidate search until the list stops changing (2 passes).

## What is NOT covered yet
- **Exception handling.** The game links SEH (`__C_specific_handler`, `RtlUnwind`) and uses C++ `catch`.
  ReXGlue's `generate_exception_handlers` is off. Funclets are only reached when something throws or
  faults, which has not been observed yet. Tracked in KNOWN_ISSUES.
- `setjmp`/`longjmp` were not configured in M1. **Fixed in M2:** `setjmp` = `0x82A184F0`, `longjmp` =
  `0x828008C0` (`config/v1/codegen.toml`). All 8 `setjmp` and 22 `longjmp` call sites (19 `bl` + 3 tail
  `b`) use ReXGlue's native implementation. Without it, the XGraphics shader compiler's error recovery
  corrupted the stack (KNOWN_ISSUES K2).
- The `.embsec_` sections (8, ≈0.25 MB) recompile cleanly and are covered by `.pdata`. Their origin is
  presumed to be XDK library code but has not been confirmed.
