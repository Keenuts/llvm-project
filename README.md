cmake -Hllvm -GNinja -Bbuild -DCMAKE_BUILD_TYPE=Debug -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD="X86;AMDGPU" -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=SPIRV -DLLVM_OPTIMIZED_TABLEGEN=1 -DLLVM_ENABLE_LLD=1 -DLLVM_USE_SPLIT_DWARF=1 -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -DCMAKE_INSTALL_PREFIX=build/install

./build/bin/clang -cc1 -triple spirv-pc-shadermodel6.0-compute -x hlsl -emit-llvm compute.hlsl
./build/bin/llc --mtriple=spirv1.5 compute.ll -o compute.spv




# The LLVM Compiler Infrastructure

Welcome to the LLVM project!

This repository contains the source code for LLVM, a toolkit for the
construction of highly optimized compilers, optimizers, and run-time
environments.

The LLVM project has multiple components. The core of the project is
itself called "LLVM". This contains all of the tools, libraries, and header
files needed to process intermediate representations and convert them into
object files. Tools include an assembler, disassembler, bitcode analyzer, and
bitcode optimizer.

C-like languages use the [Clang](http://clang.llvm.org/) frontend. This
component compiles C, C++, Objective-C, and Objective-C++ code into LLVM bitcode
-- and from there into object files, using LLVM.

Other components include:
the [libc++ C++ standard library](https://libcxx.llvm.org),
the [LLD linker](https://lld.llvm.org), and more.

## Getting the Source Code and Building LLVM

Consult the
[Getting Started with LLVM](https://llvm.org/docs/GettingStarted.html#getting-the-source-code-and-building-llvm)
page for information on building and running LLVM.

For information on how to contribute to the LLVM project, please take a look at
the [Contributing to LLVM](https://llvm.org/docs/Contributing.html) guide.

## Getting in touch

Join the [LLVM Discourse forums](https://discourse.llvm.org/), [Discord
chat](https://discord.gg/xS7Z362), or #llvm IRC channel on
[OFTC](https://oftc.net/).

The LLVM project has adopted a [code of conduct](https://llvm.org/docs/CodeOfConduct.html) for
participants to all modes of communication within the project.
