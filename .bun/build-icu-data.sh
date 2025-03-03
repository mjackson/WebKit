#!/bin/bash
# Script to extract ICU data files for Linux (x86_64 and arm64)

set -euo pipefail

# Expected parameters
ICU_VERSION="$1"         # ICU version (e.g., 75.1)
ICU_ARCHIVE_PATH="$2"    # Path to the ICU archive file
OUTPUT_DIR="$3"          # Directory to output ICU files 
BUILD_DIR="${4:-/tmp/icu-build}"  # Optional build directory

# Check platform - only support Linux
OS=$(uname -s)
ARCH=$(uname -m)
if [ "$OS" != "Linux" ]; then
    echo "ERROR: This script only supports Linux platforms"
    exit 1
fi

if [ "$ARCH" != "x86_64" ] && [ "$ARCH" != "aarch64" ]; then
    echo "ERROR: This script only supports x86_64 and arm64 architectures"
    exit 1
fi

# Extract version components
ICU_VERSION_MAJOR=$(echo $ICU_VERSION | cut -d. -f1)
ICU_DATA_NAME="icudt${ICU_VERSION_MAJOR}l"

# Setup directories
mkdir -p "${BUILD_DIR}"
mkdir -p "${OUTPUT_DIR}/share/icu"
mkdir -p "${OUTPUT_DIR}/lib"
mkdir -p "${OUTPUT_DIR}/include/unicode"

# Extract ICU archive
echo "Extracting ICU archive..."
cd "${BUILD_DIR}"
tar -xf "${ICU_ARCHIVE_PATH}" --strip-components=1

# Build ICU
echo "Configuring and building ICU..."
cd source
./configure --enable-static --enable-shared \
  --disable-layoutex --disable-layout \
  --with-data-packaging=files \
  --disable-samples --disable-debug --disable-tests

make -j$(nproc)

# Find the ICU data file
echo "Looking for ICU data file..."
DATA_FILE=$(find "${BUILD_DIR}" -name "${ICU_DATA_NAME}.dat" | head -1)
if [ -z "$DATA_FILE" ]; then
    echo "ERROR: Could not find ICU data file"
    exit 1
fi
echo "Found ICU data at: $DATA_FILE"

# Copy the full ICU data file
echo "Copying full ICU data..."
cp "$DATA_FILE" "${OUTPUT_DIR}/share/icu/full_${ICU_DATA_NAME}.dat"
DATA_SIZE=$(stat -c%s "$DATA_FILE")
echo "Full ICU data size: $DATA_SIZE bytes"

# Create a C file for loading ICU data
cat > "${OUTPUT_DIR}/share/icu/icu_loader.c" << EOF
#include <stddef.h>
#include <unicode/udata.h>

// Symbol for ICU data access
__attribute__((section(".icu_data")))
extern const unsigned char icu_data[];
extern const size_t icu_data_size;

// Initialize ICU with the embedded data
UBool icu_data_initializer(void) {
    UErrorCode status = U_ZERO_ERROR;
    udata_setCommonData(icu_data, &status);
    return U_SUCCESS(status);
}
EOF

# Create small ICU data (English-only version)
echo "Creating small ICU data..."
mkdir -p "${BUILD_DIR}/small_icu"
cd "${BUILD_DIR}/small_icu"

export DATA_FILE_BASENAME=$(basename "$DATA_FILE")

# Copy the original data file as a starting point
cp "${DATA_FILE}" "${DATA_FILE_BASENAME}" || true
bash /webkit/.bun/create-small-icu-data.sh "${DATA_FILE_BASENAME}" "${ICU_DATA_NAME}_small.dat"

# Copy the small ICU data file
cp "${ICU_DATA_NAME}_small.dat" "${OUTPUT_DIR}/share/icu/small_${ICU_DATA_NAME}.dat"
SMALL_SIZE=$(stat -c%s "${ICU_DATA_NAME}_small.dat")
echo "Small ICU data size: $SMALL_SIZE bytes"

# Copy headers and libraries
echo "Copying headers and libraries..."
cd "${BUILD_DIR}/source"
find . -path "*/common/unicode/*.h" -exec cp {} "${OUTPUT_DIR}/include/unicode/" \; 2>/dev/null || true
find . -path "*/i18n/unicode/*.h" -exec cp {} "${OUTPUT_DIR}/include/unicode/" \; 2>/dev/null || true
find "${BUILD_DIR}" -name "libicu*.so*" -exec cp {} "${OUTPUT_DIR}/lib/" \; 2>/dev/null || true
find "${BUILD_DIR}" -name "libicu*.a" -exec cp {} "${OUTPUT_DIR}/lib/" \; 2>/dev/null || true

echo "ICU build completed successfully"
echo "Full ICU data: ${OUTPUT_DIR}/share/icu/full_${ICU_DATA_NAME}.dat ($DATA_SIZE bytes)"
echo "Small ICU data: ${OUTPUT_DIR}/share/icu/small_${ICU_DATA_NAME}.dat ($SMALL_SIZE bytes)"