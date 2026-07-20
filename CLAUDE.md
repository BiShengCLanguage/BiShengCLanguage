# CLAUDE.md — BiSheng C (BSC) compiler repository

BiSheng C is a memory-safe extension of C — ownership, borrowing, traits, generics,
safe zones, nullability checking, async/await — implemented as a fork of
LLVM/Clang 15.0.4 (version set in `llvm/CMakeLists.txt:23`). BSC source files use
`.cbs` (source) and `.hbs` (header); both are registered lit test suffixes
(`clang/test/lit.cfg.py:28`).

## Non-negotiable rules for AI agents

1. **BSC syntax postdates your training data. Never state BSC syntax or semantics
   from memory.** Every syntax/semantics claim must cite one of:
   - `clang/docs/BSC/BiShengCLanguageUserManual.md` — the authoritative manual
     (~9,000 lines, Chinese);
   - a concrete test under `clang/test/BSC/Positive/` (code the compiler accepts);
   - a concrete test under `clang/test/BSC/Negative/` (code the compiler rejects,
     with expected diagnostics).
2. **The compiler is the sole referee.** Any `.cbs`/`.hbs` you write or modify is
   correct only once it compiles (or, for Negative tests, fails with the expected
   diagnostic). Do not report BSC work as done without a compile run.
3. **Behavior changes require tests.** New accepted syntax → a test in the matching
   `clang/test/BSC/Positive/<Feature>/` directory; new or changed diagnostics → a
   test in `clang/test/BSC/Negative/`.
4. **Do not touch upstream LLVM regions.** BSC compiler changes belong in the
   dedicated `BSC/` subdirectories (see code map). Modify shared upstream files
   (`llvm/`, generic clang sources) only when unavoidable, with minimal diffs, so
   upstream merges stay tractable.

## BSC code map

| Path | Contents |
|---|---|
| `clang/docs/BSC/BiShengCLanguageUserManual.md` | User manual, ~9,000 lines — build/install (§1.1), all language features |
| `clang/docs/BSC/bsc-errors.md` | BSC error code reference |
| `clang/docs/BSC/Proposals/` | Design proposals: `ownership.md`, `borrow.md`, `safe-zone.md` |
| `clang/test/BSC/Positive/` | ~690 `.cbs` tests that must compile, grouped by feature: `Ownership/`, `Trait/`, `Generic/`, `SafeZone/`, `NullabilityCheck/`, `OwnedStruct/`, `Coroutine/`, `Method/`, `OperatorOverload/`, `InitAnalysis/`, `InitCheck/`, `Matrix/`, `ParamCheck/`, `Driver/`, `AST/`, `Others/` |
| `clang/test/BSC/Negative/` | ~777 `.cbs` tests that must be rejected |
| `clang/test/BSC/BSCIR/` | IR-level tests |
| `libcbs/` | BSC standard library (`src/`, `lib/`, `test/`, own CMake build) |
| `clang-tools-extra/clang-tidy/bsc/` | BSC-specific clang-tidy checks |
| `clang/lib/{Parse,Sema,AST,Analysis}/BSC/` | BSC feature implementation inside clang |
| `clang/include/clang/{AST,Basic,Driver,Analysis/Analyses}/BSC/` | BSC headers |
| `clang/lib/Headers/bsc_include/` | BSC builtin headers shipped with the compiler |

## Build (from manual §1.1)

```shell
mkdir build && cd build
cmake -G "Ninja" -DLLVM_ENABLE_PROJECTS="clang" -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_USE_LINKER=lld -DBUILD_SHARED_LIBS=OFF -DLLVM_TARGETS_TO_BUILD="X86" \
  -DCMAKE_INSTALL_PREFIX=<install_dir> ../llvm
ninja
ninja install
```

Optional — build the BSC standard library `libcbs` with the freshly built compiler:

```shell
mkdir build_libcbs && cd build_libcbs
cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=<install_dir>/bin/clang \
  -DCMAKE_INSTALL_PREFIX=<install_dir> ../libcbs
ninja stdcbs && ninja install
```

Compile a single BSC file: `<install_dir>/bin/clang file.cbs -o file`
(plain C files can be treated as BSC with `-x bsc`).

## Test

Run the BSC test suite with lit from your build directory:

```shell
build/bin/llvm-lit -sv clang/test/BSC          # whole BSC suite
build/bin/llvm-lit -sv clang/test/BSC/Positive/Ownership   # one feature dir
```

## Writing BSC code — use the skills repo

The companion repository
[BiShengCLanguage/BiShengCSkills](https://github.com/BiShengCLanguage/BiShengCSkills)
packages BSC knowledge as agent skills (ownership, borrowing, safe zones, traits,
generics, C-to-BSC migration, error-code lookup, …) for Claude Code, Cursor,
Windsurf, OpenCode, Copilot and Codex, plus planning agents and an eval harness.
Install it when generating or reviewing BSC code outside this repository — and
prefer its `bsc-design` skill before planning any non-trivial BSC change.
