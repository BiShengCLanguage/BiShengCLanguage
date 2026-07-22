# Build and Install

Before developing programs with BiSheng C, you first need to build and install the BiSheng C compiler. The BiSheng C compiler is based on the LLVM project, so its build and installation process is the same as LLVM's.

First, clone the BiSheng C compiler project repository:

```shell
$ git clone https://gitcode.com/bisheng_c_language_dep/llvm-project.git
$ cd llvm-project
```

Then, use `cmake` and `ninja` to build the BiSheng C compiler:

```shell
$ mkdir build && cd build
$ cmake -G "Ninja" -DLLVM_ENABLE_PROJECTS="clang" -DCMAKE_BUILD_TYPE=Release -DLLVM_USE_LINKER=lld -DBUILD_SHARED_LIBS=OFF -DLLVM_TARGETS_TO_BUILD="X86" -DCMAKE_INSTALL_PREFIX=<install_dir> ../llvm
$ ninja
```

Finally, use `ninja install` to install the BiSheng C compiler built in the previous step, with the installation directory being `<install_dir>`:

```shell
$ ninja install
```

Optional build: The BiSheng C compiler project also provides the BiSheng C standard library `libcbs`, which supports BiSheng C development.

```shell
$ cd llvm-project
$ mkdir build_libcbs && cd build_libcbs
$ cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=<install_dir>/bin/clang -DCMAKE_INSTALL_PREFIX=<install_dir> ../libcbs
$ ninja stdcbs
$ ninja install
```

At this point, the build and installation of the BiSheng C compiler is complete. You can add `<install_dir>/bin` to the `PATH` environment variable for convenient use later.
