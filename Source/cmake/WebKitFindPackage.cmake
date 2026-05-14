# This file overrides the behavior of find_package for WebKit projects.
#
# CMake provides Find modules that are used within WebKit. However there are ports
# where the default behavior does not work and they need to explicitly set their
# values. There are also targets available in later versions of CMake which are
# nicer to work with.
#
# The purpose of this file is to make the behavior of find_package consistent
# across ports and CMake versions.

# CMake provided targets. Remove wrappers whenever the minimum version is bumped.
#
# ICU::<C> : need to be kept for Apple ICU

if (NOT APPLE)
    return()
endif ()

macro(find_package package)
    set(_found_package OFF)

    # Apple builds have a unique location for ICU
    if (USE_APPLE_ICU AND "${package}" STREQUAL "ICU")
        set(_found_package ON)

        set(ICU_INCLUDE_DIRS ${CMAKE_BINARY_DIR}/ICU/Headers)

        # Apple just has a single tbd/dylib for ICU.
        find_library(ICU_I18N_LIBRARY icucore)
        find_library(ICU_UC_LIBRARY icucore)
        find_library(ICU_DATA_LIBRARY icucore)

        set(ICU_LIBRARIES ${ICU_UC_LIBRARY})
        set(ICU_FOUND ON)
        message(STATUS "Found ICU: ${ICU_LIBRARIES}")
    endif ()

    if (NOT _found_package)
        _find_package(${ARGV})
    endif ()

    # Create targets that are present in later versions of CMake or are referenced above
    if ("${package}" STREQUAL "ICU")
        if (ICU_FOUND AND NOT TARGET ICU::data)
            # On iOS, bundled ICU headers shadow SDK ICU in explicit module
            # PCM builds, breaking TextInput_Private.
            if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
                set(_icu_include_dirs "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:${ICU_INCLUDE_DIRS}>")
            else ()
                set(_icu_include_dirs "${ICU_INCLUDE_DIRS}")
            endif ()

            add_library(ICU::data UNKNOWN IMPORTED)
            set_target_properties(ICU::data PROPERTIES
                IMPORTED_LOCATION "${ICU_DATA_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${_icu_include_dirs}"
                IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
            )

            add_library(ICU::i18n UNKNOWN IMPORTED)
            set_target_properties(ICU::i18n PROPERTIES
                IMPORTED_LOCATION "${ICU_I18N_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${_icu_include_dirs}"
                IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
            )

            add_library(ICU::uc UNKNOWN IMPORTED)
            set_target_properties(ICU::uc PROPERTIES
                IMPORTED_LOCATION "${ICU_UC_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${_icu_include_dirs}"
                IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
            )
        endif ()
    endif ()
endmacro()
