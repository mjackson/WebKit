if(NOT WIN32)
    message(FATAL_ERROR "JamWindowsICU.cmake is only for Windows builds")
endif()

if(TARGET ICU::uc)
    return()
endif()

function(add_jam_windows_icu_target component archive)
    add_library(ICU::${component} UNKNOWN IMPORTED GLOBAL)
    set_target_properties(ICU::${component} PROPERTIES
        IMPORTED_LOCATION "${JAM_WINDOWS_ICU_ROOT}/${archive}"
        INTERFACE_INCLUDE_DIRECTORIES "${JAM_WINDOWS_ICU_INCLUDE}"
    )
endfunction()

add_jam_windows_icu_target(i18n icuin.lib)
add_jam_windows_icu_target(uc icuuc.lib)
add_jam_windows_icu_target(data icudt.lib)
