#!/bin/bash
# Script to replace ICU data in a binary for Linux (x86_64/arm64)

set -euo pipefail

# Expected parameters
TARGET_BINARY="$1"  # Binary to patch
ICU_DATA_FILE="$2"  # ICU data file to use

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

# Check if the required files exist
if [ ! -f "${TARGET_BINARY}" ]; then
    echo "ERROR: Target binary not found: ${TARGET_BINARY}"
    exit 1
fi

if [ ! -f "${ICU_DATA_FILE}" ]; then
    echo "ERROR: ICU data file not found: ${ICU_DATA_FILE}"
    exit 1
fi

# Create a temporary working directory
TMPDIR=$(mktemp -d)
cd "$TMPDIR"

# Make a backup of the original binary if not already exists
BACKUP_BINARY="${TARGET_BINARY}.bak"
if [ ! -f "${BACKUP_BINARY}" ]; then
    echo "Creating backup of original binary: ${BACKUP_BINARY}"
    cp "${TARGET_BINARY}" "${BACKUP_BINARY}"
fi

# Copy the ICU data file
cp "${ICU_DATA_FILE}" "./icu_data.dat"
DATA_SIZE=$(stat -c%s "./icu_data.dat")
echo "ICU data file size: $DATA_SIZE bytes"

# Create an object file with the ICU data
echo "Creating object file with ICU data..."
if [ "$ARCH" = "x86_64" ]; then
    # x86_64 architecture
    objcopy -I binary -O elf64-x86-64 -B i386 \
        --rename-section .data=.icu_data,alloc,load,readonly,data \
        "./icu_data.dat" "./icu_data.o"
else
    # arm64 architecture
    objcopy -I binary -O elf64-littleaarch64 -B aarch64 \
        --rename-section .data=.icu_data,alloc,load,readonly,data \
        "./icu_data.dat" "./icu_data.o"
fi

# Create a temporary binary with the .icu_data section removed
echo "Removing existing ICU data section from binary..."
objcopy --remove-section=.icu_data "${TARGET_BINARY}" "./temp_binary"

# Add the new .icu_data section
echo "Adding new ICU data section to binary..."
objcopy --add-section=.icu_data="./icu_data.dat" \
    --set-section-flags=.icu_data=alloc,load,readonly,data \
    "./temp_binary" "./patched_binary"

# Replace the original with the patched version
mv "./patched_binary" "${TARGET_BINARY}"

echo "ICU data replacement completed successfully."
echo "Binary: ${TARGET_BINARY}"
echo "New ICU data: ${ICU_DATA_FILE} ($DATA_SIZE bytes)"

# Clean up
cd - > /dev/null
rm -rf "$TMPDIR"