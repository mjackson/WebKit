#!/bin/bash
# Script to create a properly filtered small ICU data file from a full ICU data file
# This is meant to be run outside the Docker build environment

set -euo pipefail

# Expected parameters
FULL_ICU_DATA="$1"  # Full ICU data file path (e.g., full_icudt75l.dat)
OUTPUT_DIR="$2"     # Output directory for the small ICU data

mkdir -p "${OUTPUT_DIR}"

# Extract the ICU version from the filename
ICU_VERSION_MAJOR=$(basename "${FULL_ICU_DATA}" | sed -E 's/.*icudt([0-9]+).*\.dat/\1/')
if [ -z "${ICU_VERSION_MAJOR}" ]; then
    echo "ERROR: Could not determine ICU version from filename: ${FULL_ICU_DATA}"
    echo "Expected format: full_icudt<N>l.dat where N is the ICU version number"
    exit 1
fi

echo "Detected ICU version: ${ICU_VERSION_MAJOR}"
ICU_DATA_NAME="icudt${ICU_VERSION_MAJOR}l"
SMALL_ICU_DATA="${OUTPUT_DIR}/small_${ICU_DATA_NAME}.dat"

# Create a temporary directory
TMPDIR=${TMPDIR:-/tmp}
WORK_DIR="${TMPDIR}/icu-filter-$RANDOM-$RANDOM"
mkdir -p "${WORK_DIR}"

cleanup() {
    echo "Cleaning up temporary files..."
    rm -rf "${WORK_DIR}"
}

# Set up cleanup of temporary files on exit
trap cleanup EXIT

# Check if icupkg is available
if ! command -v icupkg &> /dev/null; then
    echo "WARNING: icupkg tool not found, cannot create filtered small ICU data"
    echo "Copying full ICU data as fallback..."
    cp "${FULL_ICU_DATA}" "${SMALL_ICU_DATA}"
    exit 0
fi

# Copy the full ICU data file to the temporary directory
cp "${FULL_ICU_DATA}" "${WORK_DIR}/${ICU_DATA_NAME}.dat"

# Create a package list file to see what's in the full ICU data
echo "Listing contents of full ICU data file..."
icupkg -l "${WORK_DIR}/${ICU_DATA_NAME}.dat" > "${WORK_DIR}/package-list.txt"

# Create a new empty ICU data file
echo "Creating new empty ICU data file..."
icupkg -c "${WORK_DIR}/${ICU_DATA_NAME}_small.dat"

# Extract essential files (based on Node.js ICU filter)
echo "Extracting essential files..."

# Root and English locales
for locale in root en; do
    echo "  Adding locale: ${locale}"
    icupkg -x "*/${locale}.res" "${WORK_DIR}/${ICU_DATA_NAME}.dat" "${WORK_DIR}/${locale}.res" || true
    if [ -f "${WORK_DIR}/${locale}.res" ]; then
        icupkg -a "${WORK_DIR}/${locale}.res" "${WORK_DIR}/${ICU_DATA_NAME}_small.dat" || true
    fi
done

# Essential files that must be kept
for file in pool supplementalData zoneinfo64 likelySubtags; do
    echo "  Adding essential file: ${file}.res"
    icupkg -x "*/${file}.res" "${WORK_DIR}/${ICU_DATA_NAME}.dat" "${WORK_DIR}/${file}.res" || true
    if [ -f "${WORK_DIR}/${file}.res" ]; then
        icupkg -a "${WORK_DIR}/${file}.res" "${WORK_DIR}/${ICU_DATA_NAME}_small.dat" || true
    fi
done

# Check if the small ICU data file exists and has content
if [ ! -s "${WORK_DIR}/${ICU_DATA_NAME}_small.dat" ]; then
    echo "ERROR: Failed to create small ICU data file"
    echo "Copying full ICU data as fallback..."
    cp "${FULL_ICU_DATA}" "${SMALL_ICU_DATA}"
    exit 0
fi

# Compare sizes
FULL_SIZE=$(stat -c%s "${FULL_ICU_DATA}" 2>/dev/null || stat -f%z "${FULL_ICU_DATA}")
SMALL_SIZE=$(stat -c%s "${WORK_DIR}/${ICU_DATA_NAME}_small.dat" 2>/dev/null || stat -f%z "${WORK_DIR}/${ICU_DATA_NAME}_small.dat")
REDUCTION=$((100 - (SMALL_SIZE * 100 / FULL_SIZE)))

echo "ICU data file size reduction: ${REDUCTION}% (from ${FULL_SIZE} to ${SMALL_SIZE} bytes)"

# Copy the small ICU data file to the output directory
cp "${WORK_DIR}/${ICU_DATA_NAME}_small.dat" "${SMALL_ICU_DATA}"

echo "Successfully created small ICU data file: ${SMALL_ICU_DATA}"

# Create object files from the data files if requested
if [ $# -ge 3 ] && [ "$3" = "--create-objects" ]; then
    echo "Creating object files from data files..."
    
    # Check if objcopy is available
    if ! command -v objcopy &> /dev/null; then
        echo "WARNING: objcopy tool not found, cannot create object files"
        exit 0
    fi
    
    # Determine architecture
    ARCH=$(uname -m)
    if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "amd64" ]; then
        FMT="elf64-x86-64"
        MACH="i386"
    elif [ "$ARCH" = "arm64" ] || [ "$ARCH" = "aarch64" ]; then
        FMT="elf64-littleaarch64"
        MACH="aarch64"
    else
        echo "Unsupported architecture: $ARCH"
        FMT="elf64-x86-64"
        MACH="i386"
    fi
    
    # Create object file from small ICU data
    echo "Creating object file from small ICU data..."
    objcopy -I binary -O $FMT -B $MACH --rename-section .data=.small_icu_data,alloc,load,readonly,data,contents \
        "${SMALL_ICU_DATA}" "${OUTPUT_DIR}/small_icu_data.o"
    
    echo "Successfully created object file: ${OUTPUT_DIR}/small_icu_data.o"
fi

exit 0