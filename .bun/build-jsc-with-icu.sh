#!/bin/bash
# Script to build JSC with ICU data embedded

set -euo pipefail

# Expected parameters
BUILD_DIR="$1"  # Path to WebKit build directory
ICU_DATA_DIR="$2"  # Path to directory containing ICU data files and object files

# Check if the required files exist
if [ ! -f "${ICU_DATA_DIR}/full_icu_data.o" ]; then
    echo "ERROR: full_icu_data.o not found in ${ICU_DATA_DIR}"
    exit 1
fi

# Compile our ICU data loader
gcc -c "${BUILD_DIR}/.bun/ICUDataLoader.c" -o "${BUILD_DIR}/ICUDataLoader.o" -I/usr/local/include

# Create a link command file that includes all the necessary objects
echo "Creating combined JSC binary with ICU data..."

# Original JSC binary path
JSC_BIN="${BUILD_DIR}/bin/jsc"

# Backup the original
if [ -f "${JSC_BIN}" ]; then
    mv "${JSC_BIN}" "${JSC_BIN}.orig"
fi

# Combine the original JSC with the ICU data objects
clang -o "${JSC_BIN}" "${JSC_BIN}.orig" \
    "${ICU_DATA_DIR}/full_icu_data.o" \
    "${BUILD_DIR}/ICUDataLoader.o" \
    -Wl,-rpath,/usr/local/lib \
    -DWITH_ICU_DYNAMIC_LOAD \
    -L/usr/local/lib -licuuc -licudata

echo "Successfully created JSC with embedded ICU data at ${JSC_BIN}"