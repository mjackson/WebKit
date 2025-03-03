# ICU Integration in Bun's WebKit Fork

This document explains how International Components for Unicode (ICU) is integrated into Bun's WebKit fork.

## Overview

The ICU system supports two main configurations:

1. **JSC Shell** - Uses full ICU data
2. **Bun Runtime** - Has two options:
   - **Full ICU** - Complete locale support (user default)
   - **Small ICU** - English-only locale support (build system default)

## ICU Build Configuration

WebKit is configured to use static ICU with the `-DU_STATIC_IMPLEMENTATION=1` flag, which embeds ICU data directly in the binary.

## ICU Data Files

- `full_icudtXXl.dat` - Complete ICU data with all locales
- `small_icudtXXl.dat` - Minimal ICU data with English locale only

These data files are embedded in the binary in a section named `.icu_data`, accessible via the symbols `icu_data` and `icu_data_size`.

## Scripts

- `create-small-icu-data.sh` - Simple script that creates a small ICU data file with English locale support
- `create-small-icu-bun.sh` - Creates a JSC binary with embedded ICU data (small or full)
- `replace-icu-data.sh` - Replaces ICU data in an existing binary

## Using the Scripts

### Creating a Small ICU Data File

```bash
./create-small-icu-data.sh input.dat output.dat
```

The script creates a minimal ICU data file containing only essential resources for English locale support.

### Building with Small ICU

```bash
./create-small-icu-bun.sh OUTPUT_DIR ICU_DATA_DIR SOURCE_JSC
```

### Replacing ICU Data

```bash
./replace-icu-data.sh TARGET_BINARY ICU_DATA_FILE
```

This allows replacing the ICU data in a binary at runtime without relinking.

## Data Loading

At runtime, the binary will use the embedded ICU data. The binaries produced by our build system use the small ICU data by default for size efficiency.