#!/usr/bin/env bash

set -euxo pipefail

LLVM_VERSION=19

matrix_cpp_flags="-D_LIBCXX_ENABLE_ASSERTIONS=1"
matrix_cpu=native
matrix_label=bun-webkit-macos-arm64-debug
matrix_brew_prefix=/opt/homebrew/opt
matrix_package_json_arch="arm64"
matrix_CMAKE_BUILD_TYPE=Debug

matrix_ENABLE_MALLOC_HEAP_BREAKDOWN=''
matrix_ENABLE_SANITIZERS=''
LDFLAGS=''

export ICU_INCLUDE_DIRS="${matrix_brew_prefix}/icu4c/include"
export LDFLAGS="${LDFLAGS} "
export CC="${matrix_brew_prefix}/llvm@${LLVM_VERSION}/bin/clang"
export CXX="${matrix_brew_prefix}/llvm@${LLVM_VERSION}/bin/clang++"
export RANLIB="${matrix_brew_prefix}/llvm@${LLVM_VERSION}/bin/llvm-ranlib"
export AR="${matrix_brew_prefix}/llvm@${LLVM_VERSION}/bin/llvm-ar"
export CMAKE_C_COMPILER="${matrix_brew_prefix}/llvm@${LLVM_VERSION}/bin/clang"
export CMAKE_CXX_COMPILER="${matrix_brew_prefix}/llvm@${LLVM_VERSION}/bin/clang++"
export CMAKE_C_FLAGS=" -fno-exceptions ${matrix_cpp_flags} -fvisibility=hidden -fvisibility-inlines-hidden -O3 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -faddrsig "
export CMAKE_CXX_FLAGS=" -fno-exceptions ${matrix_cpp_flags} -fvisibility=hidden -fvisibility-inlines-hidden -O3 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -faddrsig -fno-c++-static-destructors "
export CMAKE_OSX_DEPLOYMENT_TARGET="13.0"
export CMAKE_BUILD_TYPE="${matrix_CMAKE_BUILD_TYPE}"
export PACKAGE_JSON_ARCH="${matrix_package_json_arch}"
export PACKAGE_JSON_LABEL="${matrix_label}"
export ENABLE_MALLOC_HEAP_BREAKDOWN="${matrix_ENABLE_MALLOC_HEAP_BREAKDOWN}"
export ENABLE_SANITIZERS="${matrix_ENABLE_SANITIZERS}"

export GITHUB_REPOSITORY="oven-sh/WebKit"
bash mac-release.bash
