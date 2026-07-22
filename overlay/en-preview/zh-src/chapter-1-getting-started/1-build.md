# 构建与安装

在使用毕昇C语言开发程序之前，需要先构建并安装毕昇C语言的编译器。毕昇C语言的编译器基于LLVM项目，因此其构建与安装过程与LLVM相同。

首先，克隆毕昇C编译器项目仓库：

```shell
$ git clone https://gitcode.com/bisheng_c_language_dep/llvm-project.git
$ cd llvm-project
```

然后，使用`cmake`和`ninja`构建毕昇C编译器：

```shell
$ mkdir build && cd build
$ cmake -G "Ninja" -DLLVM_ENABLE_PROJECTS="clang" -DCMAKE_BUILD_TYPE=Release -DLLVM_USE_LINKER=lld -DBUILD_SHARED_LIBS=OFF -DLLVM_TARGETS_TO_BUILD="X86" -DCMAKE_INSTALL_PREFIX=<install_dir> ../llvm
$ ninja
```

最后，使用`ninja install`安装上一步构建的毕昇C编译器，安装目录为 `<install_dir>`：

```shell
$ ninja install
```

可选构建: 毕昇C编译器项目中还提供了毕昇C标准库`libcbs`，用于支持毕昇C语言的开发。

```shell
$ cd llvm-project
$ mkdir build_libcbs && cd build_libcbs
$ cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=<install_dir>/bin/clang -DCMAKE_INSTALL_PREFIX=<install_dir> ../libcbs
$ ninja stdcbs
$ ninja install
```

至此，毕昇C编译器的构建与安装就完成了。可以将 `<install_dir>/bin` 添加到环境变量 `PATH` 中，方便后续使用。
