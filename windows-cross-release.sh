#!/bin/bash
# Cross-compile JavaScriptCore for Windows on a Linux host via
# Dockerfile.windows. Windows targets are always cross-compiled here; the
# target arch comes from WIN_ARCH, not the host.
set -euxo pipefail

export DOCKER_BUILDKIT=1

export WIN_ARCH="${WIN_ARCH:-x64}"
export WEBKIT_RELEASE_TYPE="${WEBKIT_RELEASE_TYPE:-Release}"
export LTO_FLAG="${LTO_FLAG:-}"
export ENABLE_SANITIZERS="${ENABLE_SANITIZERS:-}"

# Codegen floor per arch, matching windows-release.ps1:
#   x64 -> haswell, x64 baseline -> nehalem, arm64 -> armv8-a+crc.
case "$WIN_ARCH" in
    x64)
        WIN_TRIPLE_ARCH="x86_64"
        if [ "${BASELINE:-}" = "true" ] || [ "${BASELINE:-}" = "1" ]; then
            : "${MARCH:=nehalem}"
        else
            : "${MARCH:=haswell}"
        fi
        : "${MARCH_FLAG:="/clang:-march=${MARCH}"}"
        : "${ICU_MARCH_FLAG:="-march=${MARCH}"}"
        ;;
    arm64)
        WIN_TRIPLE_ARCH="aarch64"
        : "${MARCH_FLAG:="/clang:-march=armv8-a+crc"}"
        : "${ICU_MARCH_FLAG:="-march=armv8-a+crc"}"
        ;;
    *) echo "error: WIN_ARCH must be x64 or arm64, got '$WIN_ARCH'" >&2; exit 1 ;;
esac
export WIN_TRIPLE_ARCH MARCH_FLAG ICU_MARCH_FLAG

export CONTAINER_NAME="bun-webkit-windows-cross-${WIN_ARCH}"
if [ "$WEBKIT_RELEASE_TYPE" == "Debug" ]; then
    CONTAINER_NAME="${CONTAINER_NAME}-debug"
fi

temp="${temp:-${TMPDIR:-/tmp}}"
mkdir -p "$temp"
rm -rf "$temp/bun-webkit"

docker buildx build -f Dockerfile.windows -t "$CONTAINER_NAME" \
    --build-arg WIN_ARCH="$WIN_ARCH" \
    --build-arg WIN_TRIPLE_ARCH="$WIN_TRIPLE_ARCH" \
    --build-arg WEBKIT_RELEASE_TYPE="$WEBKIT_RELEASE_TYPE" \
    --build-arg LTO_FLAG="$LTO_FLAG" \
    --build-arg MARCH_FLAG="$MARCH_FLAG" \
    --build-arg ICU_MARCH_FLAG="$ICU_MARCH_FLAG" \
    --build-arg ENABLE_SANITIZERS="$ENABLE_SANITIZERS" \
    --progress=plain \
    --platform=linux/amd64 \
    --target=artifact \
    --output type=local,dest="$temp/bun-webkit" .

echo "Successfully built $CONTAINER_NAME to $temp/bun-webkit"
