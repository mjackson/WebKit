#!/bin/bash
# Script to create different Bun builds with varied ICU support

set -euo pipefail

# Expected parameters
OUTPUT_DIR="$1"  # Where to put the final binaries
ICU_DATA_DIR="$2"  # Directory containing ICU data files
SOURCE_JSC="$3"    # Path to the source JSC binary

# Check if the required directories exist
if [ ! -d "${ICU_DATA_DIR}" ]; then
    echo "ERROR: ICU data directory not found: ${ICU_DATA_DIR}"
    exit 1
fi

if [ ! -f "${SOURCE_JSC}" ]; then
    echo "ERROR: Source JSC binary not found: ${SOURCE_JSC}"
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"

# Create a directory structure for the resulting binaries
mkdir -p "${OUTPUT_DIR}/bin"
mkdir -p "${OUTPUT_DIR}/lib"
mkdir -p "${OUTPUT_DIR}/include"
mkdir -p "${OUTPUT_DIR}/share/icu"

# Copy ICU data files to the output directory
cp "${ICU_DATA_DIR}/full_icudt"*".dat" "${OUTPUT_DIR}/share/icu/" 2>/dev/null || true
cp "${ICU_DATA_DIR}/small_icudt"*".dat" "${OUTPUT_DIR}/share/icu/" 2>/dev/null || true

# Gather system information
ARCH=$(uname -m)
OS=$(uname -s)
CC=${CC:-clang}
CXX=${CXX:-clang++}
OBJCOPY=${OBJCOPY:-objcopy}

# Determine linker flags based on OS
LDFLAGS="-Wl,-rpath,\${ICU_DATA_DIR}/../lib"
if [ "$OS" = "Darwin" ]; then
    LDFLAGS=""  # macOS doesn't use rpath in the same way
fi

# Compile the ICU data loader
echo "Compiling ICU data loader..."
${CC} -c "$(dirname "$0")/ICUDataLoader.c" -o "${OUTPUT_DIR}/ICUDataLoader.o" -I${ICU_DATA_DIR}/../include

# Verify that the ICU data files exist
if [ ! -f "${ICU_DATA_DIR}/full_icu_data.o" ]; then
    echo "ERROR: Full ICU data object file not found: ${ICU_DATA_DIR}/full_icu_data.o"
    exit 1
fi

if [ ! -f "${ICU_DATA_DIR}/small_icu_data.o" ]; then
    echo "ERROR: Small ICU data object file not found: ${ICU_DATA_DIR}/small_icu_data.o"
    exit 1
fi

# Create JSC binary with full ICU data (main jsc shell binary)
echo "Creating JSC shell with full ICU data..."
${CC} -o "${OUTPUT_DIR}/bin/jsc" "${SOURCE_JSC}" \
    "${ICU_DATA_DIR}/full_icu_data.o" \
    "${OUTPUT_DIR}/ICUDataLoader.o" \
    ${LDFLAGS} \
    -DWITH_ICU_DYNAMIC_LOAD=1 \
    -L"${ICU_DATA_DIR}/../lib" -licuuc -licudata


# Create standalone object files for both ICU data versions that can be linked at build time
# This is used for Bun's build system to create swap-able ICU data
echo "Creating standalone object files for Bun build system..."
mkdir -p "${OUTPUT_DIR}/obj"

# Create separate object files for the small and full ICU data
# These can be used for linking at build time and binary patching at runtime
${CC} -DWITH_ICU_DYNAMIC_LOAD=1 -c "$(dirname "$0")/ICUDataLoader.c" -o "${OUTPUT_DIR}/obj/ICUDataLoader_standalone.o" -I${ICU_DATA_DIR}/../include
cp "${ICU_DATA_DIR}/full_icu_data.o" "${OUTPUT_DIR}/obj/full_icu_data.o"
cp "${ICU_DATA_DIR}/small_icu_data.o" "${OUTPUT_DIR}/obj/small_icu_data.o"

# Create a combined object file with the small ICU data for easy linking
${CC} -r -o "${OUTPUT_DIR}/obj/small_icu_combined.o" "${OUTPUT_DIR}/obj/small_icu_data.o" "${OUTPUT_DIR}/obj/ICUDataLoader_standalone.o"

# Create a combined object file with the full ICU data for runtime replacement
${CC} -r -o "${OUTPUT_DIR}/obj/full_icu_combined.o" "${OUTPUT_DIR}/obj/full_icu_data.o" "${OUTPUT_DIR}/obj/ICUDataLoader_standalone.o"

# Copy shared libraries to the output directory
echo "Copying ICU shared libraries..."
if [ "$OS" = "Darwin" ]; then
    cp "${ICU_DATA_DIR}"/../lib/libicu*.dylib "${OUTPUT_DIR}/lib/" 2>/dev/null || true
else
    cp "${ICU_DATA_DIR}"/../lib/libicu*.so* "${OUTPUT_DIR}/lib/" 2>/dev/null || true
fi

echo "Builds created successfully in ${OUTPUT_DIR}"
echo "  - Full ICU: ${OUTPUT_DIR}/bin/jsc"
echo "  - Small ICU: ${OUTPUT_DIR}/bin/jsc-small"
echo "  - Build objects in: ${OUTPUT_DIR}/obj/"
echo ""
echo "Environment variables for dynamic loading:"
echo "  export BUN_ICU_DATA=${OUTPUT_DIR}/share/icu"
echo "  export NODE_ICU_DATA=${OUTPUT_DIR}/share/icu"