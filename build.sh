#!/bin/bash
set -euo pipefail
shopt -s inherit_errexit

mkdir -p llvm-build
cd llvm-build
cmake ../llvm-project/llvm -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -G Ninja \
    -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_ENABLE_WARNINGS=OFF -DLLVM_APPEND_VC_REV=OFF -DLLVM_TARGETS_TO_BUILD="X86;" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DLLVM_ENABLE_RTTI=ON -DLLVM_ENABLE_EH=ON -DLLVM_ENABLE_ZSTD=OFF
cmake --build . -j 32 -t opt
cmake --build . -j 32 -t llvm-extract

cd ..
mkdir -p alive2-build
cd alive2-build
cmake ../alive2 -GNinja -DCMAKE_PREFIX_PATH=../llvm-build/ -DBUILD_TV=1 -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build . -j 32 -t alive-tv

cd ..
mkdir -p build
cd build
cmake .. -GNinja -DLLVM_DIR=$(pwd)/../llvm-build/lib/cmake/llvm -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build . -j
