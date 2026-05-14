set_property(DIRECTORY . PROPERTY FOLDER "WTF")

add_subdirectory(wtf)

# Apple builds have the ICU headers checked into ${WTF_DIR}/icu
# Copy them into ${ICU_INCLUDE_DIRS} so the build behaves like find_package was used
if (USE_APPLE_ICU)
    file(COPY ${WTF_DIR}/icu/unicode DESTINATION ${ICU_INCLUDE_DIRS})
    # WebKitAdditions may overlay SDK ICU headers after the bundled copy.
    if (WEBKIT_ADDITIONS_INCLUDE_PATH AND EXISTS "${WEBKIT_ADDITIONS_INCLUDE_PATH}/WebKitAdditions/CMake/ICU.cmake")
        include("${WEBKIT_ADDITIONS_INCLUDE_PATH}/WebKitAdditions/CMake/ICU.cmake")
    endif ()
endif ()
