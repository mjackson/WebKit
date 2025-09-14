#!/bin/bash

set -euxo pipefail

export DOCKER_BUILDKIT=1

export BUILDKIT_ARCH=$(uname -m)
export ARCH=${BUILDKIT_ARCH}
export LTO_FLAG="${LTO_FLAG:-""}"
if [ "$BUILDKIT_ARCH" == "amd64" ]; then
    export BUILDKIT_ARCH="amd64"
    export ARCH=x64
fi

if [ "$BUILDKIT_ARCH" == "x86_64" ]; then
    export BUILDKIT_ARCH="amd64"
    export ARCH=x64
fi

if [ "$BUILDKIT_ARCH" == "arm64" ]; then
    export BUILDKIT_ARCH="arm64"
    export ARCH=aarch64
fi

if [ "$BUILDKIT_ARCH" == "aarch64" ]; then
    export BUILDKIT_ARCH="arm64"
    export ARCH=aarch64
fi

if [ "$BUILDKIT_ARCH" == "armv7l" ]; then
    echo "Unsupported platform: $BUILDKIT_ARCH"
    exit 1
fi

export WEBKIT_RELEASE_TYPE=${WEBKIT_RELEASE_TYPE:-"Release"}

export CONTAINER_NAME=bun-webkit-linux-$BUILDKIT_ARCH

if [ "$WEBKIT_RELEASE_TYPE" == "relwithdebuginfo" ]; then
    CONTAINER_NAME=bun-webkit-linux-$BUILDKIT_ARCH-dbg
fi

# Set default CFLAGS based on build type - musl doesn't have the same assertion concerns as glibc
# but we add NDEBUG for consistency and potential performance benefits
if [ "$WEBKIT_RELEASE_TYPE" = "Release" -o "$WEBKIT_RELEASE_TYPE" = "MinSizeRel" -o "$WEBKIT_RELEASE_TYPE" = "RelWithDebInfo" ]; then
    export DEFAULT_CFLAGS="${DEFAULT_CFLAGS:--DNDEBUG -mno-omit-leaf-frame-pointer -g -fno-omit-frame-pointer -ffunction-sections -fdata-sections -faddrsig -fno-unwind-tables -fno-asynchronous-unwind-tables -DU_STATIC_IMPLEMENTATION=1}"
else
    export DEFAULT_CFLAGS="${DEFAULT_CFLAGS:--mno-omit-leaf-frame-pointer -g -fno-omit-frame-pointer -ffunction-sections -fdata-sections -faddrsig -fno-unwind-tables -fno-asynchronous-unwind-tables -DU_STATIC_IMPLEMENTATION=1}"
fi

mkdir -p $temp
rm -rf $temp/bun-webkit

docker buildx build -f Dockerfile.musl -t $CONTAINER_NAME --build-arg LTO_FLAG="$LTO_FLAG" --build-arg WEBKIT_RELEASE_TYPE=$WEBKIT_RELEASE_TYPE --build-arg DEFAULT_CFLAGS="$DEFAULT_CFLAGS" --progress=plain --platform=linux/$BUILDKIT_ARCH --target=artifact --output type=local,dest=$temp/bun-webkit .
