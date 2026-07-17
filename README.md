<div align="center">

# 毕昇 C · BiSheng C

**A memory-safe superset of C — ownership, borrowing, and concurrency, added incrementally to the code you already have.**

[![User Manual (中文)](https://img.shields.io/badge/User_Manual-中文-a72145)](https://bishengclanguage.github.io/BiShgengCLanguage/)
[![User Manual (English)](https://img.shields.io/badge/User_Manual-English-1f6feb)](https://bishengclanguage.github.io/BiShgengCLanguage/en/)
[![Based on LLVM](https://img.shields.io/badge/based_on-LLVM_15-262d3a?logo=llvm)](https://llvm.org/)
[![Language](https://img.shields.io/badge/language-C-555?logo=c)](#)

[User Manual](https://bishengclanguage.github.io/BiShgengCLanguage/) ·
[English Manual](https://bishengclanguage.github.io/BiShgengCLanguage/en/) ·
[Getting Started](#getting-started) ·
[Features](#why-bisheng-c)

</div>

---

## The sweet spot for AI-generated systems code

AI coding assistants are now writing a large share of systems code — and that has surfaced a
sharp trade-off:

- **AI-generated C compiles and runs, but is riddled with memory bugs.** Models happily emit
  C, yet the output is prone to leaks, use-after-free, double-free, and out-of-bounds access —
  the same unsafe-pointer pitfalls that have plagued C for decades, now produced at scale.
- **AI-generated Rust is safe, but models struggle to produce it.** Rust's safety comes from
  ownership, lifetimes, and the borrow checker — and those same rules give models a low
  first-try success rate. Generations frequently fail to compile and need many correction
  rounds.

**BiSheng C aims for the sweet spot.** It is a *superset of C*, so models generate it as
fluently as ordinary C — but it adds opt-in ownership, borrowing, and non-null guarantees that
let the compiler catch the memory bugs AI-generated C would otherwise ship. You keep C's high
generation success rate while gaining Rust-style memory safety, and you can adopt the safety
features **incrementally** instead of rewriting existing code.

## What is BiSheng C?

In systems programming, C remains the most widely used language — especially in
resource-constrained embedded scenarios. But writing C comes with well-known pain points:
memory-safety bugs from raw pointer use, no native concurrency, and missing high-level
abstractions such as generics.

Languages like Rust tackle these with ownership, lifetimes, a borrow checker, and stackless
coroutines — but as an entirely new language with a steep learning curve, and no story for
the **existing** C code you already maintain.

**BiSheng C takes a different path.** It enhances C in place: stronger memory-safety
guarantees, language-level concurrency, and modern abstractions — all adoptable
**incrementally**, without rewriting your existing codebase. BiSheng C is, in effect, a
**superset of C**.

## Why BiSheng C?

| Feature | What it gives you |
|---|---|
| **Ownership** | Compile-time tracking of `_Owned` pointers prevents memory leaks, use-after-free, and double-free. |
| **Borrowing** | `_Borrow` references with a borrow checker — safe aliasing without giving up ownership. |
| **Non-null pointers** | `_Nonnull` / `_Nullable` make null-safety explicit and checked. |
| **Safe zones** | `_Safe` regions where the compiler enforces the memory-safety rules. |
| **Generics & Traits** | `_Trait`-based polymorphism, generics, and operator overloading. |
| **Concurrency** | Language-level stackless coroutines (`_Async` / `_Await`). |
| **Toolchain** | Source-to-source translation back to plain C, debugging support, and an IDE plugin. |
| **Standard library** | `libcbs`: safe APIs, containers, smart pointers, and a coroutine scheduler. |

> The full story — with examples and rules for every feature — lives in the
> **[User Manual](https://bishengclanguage.github.io/BiShgengCLanguage/)**
> ([English](https://bishengclanguage.github.io/BiShgengCLanguage/en/)).

## Getting Started

### Build the compiler

BiSheng C's compiler is built on LLVM, so the build follows the standard LLVM flow.

```shell
git clone https://gitee.com/bisheng_c_language_dep/llvm-project.git
cd llvm-project
mkdir build && cd build
cmake -G "Ninja" \
  -DLLVM_ENABLE_PROJECTS="clang" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_USE_LINKER=lld \
  -DBUILD_SHARED_LIBS=OFF \
  -DLLVM_TARGETS_TO_BUILD="X86" \
  -DCMAKE_INSTALL_PREFIX=<install_dir> \
  ../llvm
ninja
ninja install
```

Add `<install_dir>/bin` to your `PATH`. Optionally build the standard library `libcbs`:

```shell
cd llvm-project
mkdir build_libcbs && cd build_libcbs
cmake -G "Ninja" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=<install_dir>/bin/clang \
  -DCMAKE_INSTALL_PREFIX=<install_dir> \
  ../libcbs
ninja stdcbs
ninja install
```

### Your first program

BiSheng C source files use the `.cbs` extension. Member functions are one of the simplest
features to start with:

```c
#include <stdio.h>

struct Foo {
    int a;
};

// A member function attached to struct Foo.
int struct Foo::getA(struct Foo* this) {
    return this->a;
}

int main() {
    struct Foo foo = {.a = 1};
    printf("foo.getA() = %d\n", foo.getA());
    return 0;
}
```

Compile and run it with the BiSheng C compiler:

```shell
clang demo.cbs -o demo
./demo
# foo.getA() = 1
```

See **[Getting Started -> Your First BiSheng C Program](https://bishengclanguage.github.io/BiShgengCLanguage/en/chapter-1-getting-started/2-hello-bsc.html)**
for a full walkthrough.

## Documentation

| Resource | Link |
|---|---|
| User Manual (中文) | https://bishengclanguage.github.io/BiShgengCLanguage/ |
| User Manual (English) | https://bishengclanguage.github.io/BiShgengCLanguage/en/ |
| Preview edition | [中文](https://bishengclanguage.github.io/BiShgengCLanguage/preview/) · [English](https://bishengclanguage.github.io/BiShgengCLanguage/en/preview/) |

The preview edition documents upcoming, not-yet-released features.

## Repository layout

This repository is a fork of the [LLVM project](https://llvm.org/); the BiSheng C compiler is
implemented within `clang`. Everything else mirrors upstream LLVM.

- `clang/` — the Clang/BiSheng C front end (where the language features live)
- `libcbs/` — the BiSheng C standard library
- `llvm/`, `lld/`, ... — upstream LLVM components

The original upstream LLVM README is preserved as
[`README-LLVM.md`](README-LLVM.md).

## License

BiSheng C is distributed under the same terms as the LLVM Project —
the Apache License v2.0 with LLVM Exceptions. See the per-component `LICENSE.TXT` files
(e.g. [`llvm/LICENSE.TXT`](llvm/LICENSE.TXT)).
