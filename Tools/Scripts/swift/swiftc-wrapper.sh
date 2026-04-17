#!/bin/bash
# cmake accumulates CFLAGS from pkg-config, and then passes them to swiftc.
# This script filters out the arguments that swiftc cannot accommodate.

set -e

REAL_SWIFTC=swiftc
args=()

# Detect link mode: CMake passes -emit-library or -emit-executable for link steps.
# In link mode, standalone -I flags (from CMake INCLUDES) must be wrapped with -Xcc
# so the Clang importer sees them but they don't leak to ld as input files.
# Joined -I<path> flags (from CMake FLAGS, e.g. Swift module search) are left as-is.
is_link=
for arg in "$@"; do
    case "$arg" in
        "-emit-library"|"-emit-executable") is_link=1; break ;;
    esac
done

for arg in "$@"; do
    case "$arg" in
        "-mfpmath=sse") ;;
        "-msse") ;;
        "-msse2") ;;
        "-pthread") ;;
        "-include") skip_next=1 ;;
        # CMake passes -output-file-map to compile steps but also to the combined
        # compile+link step. During linking, ld receives the .json as an input file.
        # Only strip in link mode; compile-only steps need it.
        "-output-file-map")
            if [[ -n "$is_link" ]]; then
                skip_next=1
                output_file_map_next=1
            else
                args+=("$arg")
            fi
            ;;
        "-emit-module-path")
            args+=("$arg")
            emit_module_path_next=1
            ;;
        # Standalone -I: CMake INCLUDES use "-I" "/path" as separate args.
        # In link mode, wrap with -Xcc so the path goes to the Clang importer
        # instead of leaking to ld (which tries to mmap directories as inputs).
        "-I")
            if [[ -n "$is_link" ]]; then
                redirect_next_as_xcc_include=1
            else
                args+=("$arg")
            fi
            ;;
        # CMake leaks clang linker flags into swiftc; translate them.
        "-weak_framework")
            args+=("-Xlinker" "-weak_framework")
            skip_next_as_xlinker=1
            ;;
        "-Wl,"*)
            # Split -Wl,arg1,arg2 into -Xlinker arg1 -Xlinker arg2
            IFS=',' read -ra _wl_args <<< "${arg#-Wl,}"
            for _wl in "${_wl_args[@]}"; do
                args+=("-Xlinker" "$_wl")
            done
            ;;
        "--original-swift-compiler="*)
            REAL_SWIFTC="${arg#--original-swift-compiler=}"
            ;;
        *)
            if [[ -n "$skip_next" ]]; then
                if [[ -n "$output_file_map_next" ]]; then
                    stripped_output_file_map="$arg"
                    output_file_map_next=
                fi
                skip_next=
            elif [[ -n "$emit_module_path_next" ]]; then
                emit_module_path="$arg"
                args+=("$arg")
                emit_module_path_next=
            elif [[ -n "$skip_next_as_xlinker" ]]; then
                args+=("-Xlinker" "$arg")
                skip_next_as_xlinker=
            elif [[ -n "$redirect_next_as_xcc_include" ]]; then
                args+=("-Xcc" "-I$arg")
                redirect_next_as_xcc_include=
            else
                args+=("$arg")
            fi
            ;;
    esac
done

"$REAL_SWIFTC" "${args[@]}" || swiftc_status=$?
swiftc_status=${swiftc_status:-0}

# In the combined compile+link step, we strip -output-file-map to prevent ld
# from receiving the .json as an input file.  However, without it swiftc in WMO
# mode does not produce individual .o files that CMake's Ninja generator declares
# as build outputs.  Touch the expected .o paths so Ninja does not consider the
# rule perpetually out-of-date.  Also touch the .swiftmodule since swiftc may
# skip writing it when content is unchanged.
if [[ $swiftc_status -eq 0 && -n "$stripped_output_file_map" && -f "$stripped_output_file_map" ]]; then
    python3 -c "
import json, pathlib, sys
with open(sys.argv[1]) as f:
    for v in json.load(f).values():
        o = v.get('object')
        if o:
            pathlib.Path(o).touch()
" "$stripped_output_file_map" 2>/dev/null || true
fi
if [[ $swiftc_status -eq 0 && -n "$emit_module_path" && -f "$emit_module_path" ]]; then
    touch "$emit_module_path" 2>/dev/null || true
fi

exit $swiftc_status
