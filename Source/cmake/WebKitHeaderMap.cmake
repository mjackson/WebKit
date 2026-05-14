# FIXME: re-enable for WPE/GTK once forwarded headers land. https://bugs.webkit.org/show_bug.cgi?id=180063
if (COMPILER_IS_CLANG AND NOT COMPILER_IS_CLANG_CL AND NOT PORT STREQUAL "WPE" AND NOT PORT STREQUAL "GTK")
    set(_USE_HEADER_MAPS_DEFAULT ON)
else ()
    set(_USE_HEADER_MAPS_DEFAULT OFF)
endif ()
option(USE_HEADER_MAPS "Collapse per-target include directories into a Clang header map" ${_USE_HEADER_MAPS_DEFAULT})

function(WEBKIT_MAKE_HEADER_MAP _target _source_root _dirs_var)
    set(_hmap_dirs)
    set(_result)
    set(_placeholder "@HMAP@")
    cmake_path(SET _root NORMALIZE "${_source_root}")
    foreach (_dir IN LISTS ${_dirs_var})
        cmake_path(SET _dir_n NORMALIZE "${_dir}")
        cmake_path(IS_PREFIX _root "${_dir_n}" _under_root)
        if (_under_root AND NOT "${_dir_n}" STREQUAL "${_root}")
            if (IS_DIRECTORY "${_dir}")
                if (NOT _hmap_dirs)
                    list(APPEND _result "${_placeholder}")
                endif ()
                list(APPEND _hmap_dirs "${_dir}")
            endif ()
        else ()
            list(APPEND _result "${_dir}")
        endif ()
    endforeach ()

    if (NOT _hmap_dirs)
        return()
    endif ()

    set(_dirs_file "${CMAKE_CURRENT_BINARY_DIR}/${_target}HeaderMap.dirs")
    set(_hmap_file "${CMAKE_CURRENT_BINARY_DIR}/${_target}.hmap")
    list(JOIN _hmap_dirs "\n" _dirs_content)
    file(WRITE "${_dirs_file}" "${_dirs_content}\n")

    execute_process(
        COMMAND "${PYTHON_EXECUTABLE}" "${TOOLS_DIR}/Scripts/generate-header-map"
                --dirs-file "${_dirs_file}" -o "${_hmap_file}"
        RESULT_VARIABLE _hmap_result
    )
    if (NOT _hmap_result EQUAL 0)
        message(WARNING "generate-header-map failed for ${_target}; falling back to -I directories")
        return()
    endif ()

    list(TRANSFORM _result REPLACE "^${_placeholder}$" "${_hmap_file}")
    set(${_dirs_var} "${_result}" PARENT_SCOPE)
endfunction()
