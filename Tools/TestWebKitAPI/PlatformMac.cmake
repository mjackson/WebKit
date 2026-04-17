find_library(CARBON_LIBRARY Carbon)
find_library(QUARTZCORE_LIBRARY QuartzCore)

set(TESTWEBKITAPI_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

# Use the generic gtest runner (not App/mac/main.mm which is the Xcode .app entry point).
set(_test_main_SOURCES generic/main.cpp)

# TestWTF
list(APPEND TestWTF_SOURCES
    ${_test_main_SOURCES}
    Helpers/cocoa/UtilitiesCocoa.mm
)

list(APPEND TestWTF_LIBRARIES
    ${CARBON_LIBRARY}
    "-framework Cocoa"
    "-framework CoreFoundation"
)

# TestJavaScriptCore
list(APPEND TestJavaScriptCore_SOURCES
    ${_test_main_SOURCES}
)

# TestWebCore
list(APPEND TestWebCore_SOURCES
    ${_test_main_SOURCES}
    Helpers/cocoa/TestNSBundleExtras.m
    Helpers/cocoa/UtilitiesCocoa.mm
)

list(APPEND TestWebCore_LIBRARIES
    ${QUARTZCORE_LIBRARY}
)

# TestWebKitLegacy
list(APPEND TestWebKitLegacy_SOURCES
    ${_test_main_SOURCES}
    Helpers/cocoa/TestNSBundleExtras.m
)

list(APPEND TestWebKitLegacy_LIBRARIES
    WebKit
    ${CARBON_LIBRARY}
)

# TestWebKit
list(APPEND TestWebKit_SOURCES
    ${_test_main_SOURCES}
    Helpers/cocoa/TestNSBundleExtras.m
    Helpers/cocoa/UtilitiesCocoa.mm

    Helpers/mac/OffscreenWindow.mm
    Helpers/mac/PlatformUtilitiesMac.mm
    Helpers/mac/PlatformWebViewMac.mm
)

list(APPEND TestWebKit_PRIVATE_INCLUDE_DIRECTORIES
    ${WTF_FRAMEWORK_HEADERS_DIR}
    ${bmalloc_FRAMEWORK_HEADERS_DIR}
    ${WebKit_FRAMEWORK_HEADERS_DIR}
    ${WebKitLegacy_FRAMEWORK_HEADERS_DIR}
)

list(APPEND TestWebKit_LIBRARIES
    JavaScriptCore
    WTF
    ${CARBON_LIBRARY}
)

# NSWindow.autodisplay is deprecated since 10.14 but still used in OffscreenWindow.mm.
WEBKIT_ADD_TARGET_CXX_FLAGS(TestWebKit -Wno-deprecated-declarations)

# run-api-tests expects the binary to be named TestWebKitAPI.
set_target_properties(TestWebKit PROPERTIES OUTPUT_NAME TestWebKitAPI)

# Common framework header directories needed by config.h (<wtf/Platform.h>, <WebKit/WebKit2_C.h>, etc.)
set(_testapi_framework_headers
    ${WTF_FRAMEWORK_HEADERS_DIR}
    ${bmalloc_FRAMEWORK_HEADERS_DIR}
    ${JavaScriptCore_FRAMEWORK_HEADERS_DIR}
    ${JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS_DIR}
    ${PAL_FRAMEWORK_HEADERS_DIR}
    ${WebCore_PRIVATE_FRAMEWORK_HEADERS_DIR}
    ${WebKit_FRAMEWORK_HEADERS_DIR}
    ${WebKitLegacy_FRAMEWORK_HEADERS_DIR}
)

# TestWebKitAPIBase needs framework headers for config.h includes.
target_include_directories(TestWebKitAPIBase PRIVATE ${_testapi_framework_headers})

# TestWebKitAPIInjectedBundle -- .bundle for NSBundle loading on Mac.
target_sources(TestWebKitAPIInjectedBundle PRIVATE
    ${TESTWEBKITAPI_DIR}/Helpers/cocoa/TestNSBundleExtras.m
    ${TESTWEBKITAPI_DIR}/Helpers/cocoa/UtilitiesCocoa.mm
    ${TESTWEBKITAPI_DIR}/InjectedBundle/mac/InjectedBundleControllerMac.mm
    ${TESTWEBKITAPI_DIR}/Helpers/mac/PlatformUtilitiesMac.mm
)

target_include_directories(TestWebKitAPIInjectedBundle PRIVATE
    ${_testapi_framework_headers}
    ${TESTWEBKITAPI_DIR}/InjectedBundle
)

set_target_properties(TestWebKitAPIInjectedBundle PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION bundle
    OUTPUT_NAME InjectedBundleTestWebKitAPI
)

# The InjectedBundle is loaded into a WebKit process that already has WTF.
# WebCoreTestSupport (static) references WTF symbols that the bundle's own
# .o files don't use directly, so the linker can't resolve them at link time.
# Use -undefined dynamic_lookup since the hosting process provides them.
target_link_options(TestWebKitAPIInjectedBundle PRIVATE "LINKER:-undefined,dynamic_lookup")
target_link_libraries(TestWebKitAPIInjectedBundle PRIVATE
    JavaScriptCore
    WebCoreTestSupport
    WebKit
    "-framework Cocoa"
    "-framework Foundation"
)

set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -framework Cocoa")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -framework Cocoa")

# TestWebKitAPIResources.bundle -- test resource files loaded via
# [NSBundle.test_resourcesBundle URLForResource:withExtension:].
# For a non-.app executable, NSBundle.mainBundle is the directory containing
# the binary, so the .bundle must sit next to the test executables.
set(_resources_bundle_dir "${TESTWEBKITAPI_RUNTIME_OUTPUT_DIRECTORY}/TestWebKitAPIResources.bundle")
file(GLOB _resources_top "${TESTWEBKITAPI_DIR}/Resources/*.*")
file(GLOB _resources_cocoa "${TESTWEBKITAPI_DIR}/Resources/cocoa/*.*")
WEBKIT_COPY_FILES(TestWebKitAPIResources
    DESTINATION "${_resources_bundle_dir}"
    FILES ${_resources_top} ${_resources_cocoa}
    FLATTENED
)
# Ensure all test targets depend on the resources bundle.
foreach (_test_target TestWTF TestJavaScriptCore TestWebCore TestWebKitLegacy TestWebKit)
    if (TARGET ${_test_target})
        add_dependencies(${_test_target} TestWebKitAPIResources)
    endif ()
endforeach ()
