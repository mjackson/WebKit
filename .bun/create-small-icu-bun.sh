#!/bin/bash
# Script to create a JSC binary with ICU data for Linux (x86_64/arm64)

set -euo pipefail

# Expected parameters
OUTPUT_DIR="$1"  # Where to put the final binary
ICU_DATA_DIR="$2"  # Directory containing ICU data files
SOURCE_JSC="$3"    # Path to the source JSC binary
OUTPUT_NAME="${4:-jsc-small}"  # Optional name for the output binary
ICU_TYPE="${5:-small}"  # Optional ICU type, defaults to small (can be 'small' or 'full')

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

# Check if the required files/directories exist
if [ ! -d "${ICU_DATA_DIR}" ]; then
    echo "ERROR: ICU data directory not found: ${ICU_DATA_DIR}"
    exit 1
fi

if [ ! -f "${SOURCE_JSC}" ]; then
    echo "ERROR: Source JSC binary not found: ${SOURCE_JSC}"
    exit 1
fi

# Ensure directories exist
mkdir -p "${OUTPUT_DIR}/bin"
mkdir -p "${OUTPUT_DIR}/obj"

# Set compiler
CC=${CC:-clang}

# Choose which ICU data file to use
if [ "$ICU_TYPE" = "full" ]; then
    ICU_DATA_FILE="${ICU_DATA_DIR}/full_icudt"*".dat"
    echo "Using full ICU data"
else
    ICU_DATA_FILE="${ICU_DATA_DIR}/small_icudt"*".dat"
    echo "Using small ICU data"
fi

# Find the actual file
ICU_DATA_FILE=$(echo $ICU_DATA_FILE)
if [ ! -f "$ICU_DATA_FILE" ]; then
    echo "ERROR: ICU data file not found: $ICU_DATA_FILE"
    exit 1
fi

echo "Using ICU data file: $ICU_DATA_FILE ($(stat -c%s "$ICU_DATA_FILE") bytes)"

# Create temporary working directory
TMPDIR=$(mktemp -d)
cp "$ICU_DATA_FILE" "$TMPDIR/icu_data.dat"

# Create object file with ICU data for selected architecture
echo "Creating object file with ICU data..."
if [ "$ARCH" = "x86_64" ]; then
    objcopy -I binary -O elf64-x86-64 -B i386 \
        --rename-section .data=.icu_data,alloc,load,readonly,data \
        "$TMPDIR/icu_data.dat" "$TMPDIR/icu_data_bin.o"
else
    objcopy -I binary -O elf64-littleaarch64 -B aarch64 \
        --rename-section .data=.icu_data,alloc,load,readonly,data \
        "$TMPDIR/icu_data.dat" "$TMPDIR/icu_data_bin.o"
fi

# Compile the ICU loader
echo "Compiling ICU loader..."
cp "${ICU_DATA_DIR}/icu_loader.c" "$TMPDIR/icu_loader.c"
${CC} -c "$TMPDIR/icu_loader.c" -I${ICU_DATA_DIR}/../include -o "$TMPDIR/icu_loader.o"

# Link everything together
echo "Creating final binary with ICU data..."
${CC} -o "${OUTPUT_DIR}/bin/${OUTPUT_NAME}" "${SOURCE_JSC}" \
    "$TMPDIR/icu_data_bin.o" "$TMPDIR/icu_loader.o" \
    -L"${ICU_DATA_DIR}/../lib" -licuuc -licudata -Wl,-rpath,\${ICU_DATA_DIR}/../lib

# Also create a standalone object file for future binary patching
echo "Creating standalone object file for future patching..."
${CC} -r -o "${OUTPUT_DIR}/obj/${ICU_TYPE}_icu_data.o" \
    "$TMPDIR/icu_data_bin.o" "$TMPDIR/icu_loader.o"

# Clean up
rm -rf "$TMPDIR"

echo "Build completed successfully: ${OUTPUT_DIR}/bin/${OUTPUT_NAME}"
echo "Standalone object: ${OUTPUT_DIR}/obj/${ICU_TYPE}_icu_data.o"